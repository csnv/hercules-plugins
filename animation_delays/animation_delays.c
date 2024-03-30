//===== Hercules/Heracles Plugin ===================================
//= Animation delays plugin
//===== By: ========================================================
//= csnv
//===== Version: ===================================================
//= 0.1alpha
//===== Description: ===============================================
//= Enables hard delays based on an aproximation of client behavior
//===== Repository: ================================================
//= https://github.com/csnv/hercules-plugins
//==================================================================

#include "common/hercules.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "common/conf.h"
#include "common/HPMi.h"
#include "common/memmgr.h"
#include "common/mmo.h"
#include "common/nullpo.h"
#include "common/timer.h"

#include "map/pc.h"
#include "map/skill.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h"

#define ANIM_DELAY_LENIENCY 90 // Percentage of animation delay that will be checked

/**
 * Allow skills to bypass animation delays
 * 
 * Comment/uncomment to toggle the bypass behavior.
 * Only skills with the bypass setting defined as true are able to bypass animation delays.
 * This is a defined behavior of the client and usually used for faster spam of other skills
 */
#define ANIM_DELAY_ALLOW_BYPASS

/**
 * Allow breaking animation delays with the player movement. Also called skill dancing
 *
 * Comment/uncomment to toggle the movement/dancing behavior.
 * Client reproduces the walking animation when forced to move, breaking the animation of
 * last skill. Although the client does block moving around during such animations, the user can
 * make use of this thanks to latency and proper timing.
 */
#define ANIM_DELAY_ALLOW_DANCING

HPExport struct hplugin_info pinfo = {
	"animation_delays",   // Plugin name
	SERVER_TYPE_MAP,             // Which server types this plugin works with?
	"0.1",                       // Plugin version
	HPM_VERSION,                 // HPM Version (don't change, macro is automatically updated)
};

/* Stores information as it is described in skill_adelays.conf */
struct skill_adelay_entry {
	int delay; // Skill animation delay
	int delay_others; // skill animation delay imposed on other skills used afterwards
	int max_delay; // Skill maximum delay
#ifdef	ANIM_DELAY_ALLOW_BYPASS
	bool bypass; // Skill bypasses existing delay
#endif
};

/* List of skill adelays */
struct skill_adelay_entry *skill_adelay_list[MAX_SKILL_ID];

/* Last skill used per character */
struct skill_adelay_timer {
	uint16 last_skill_id;
	int64 tick;
#ifdef ANIM_DELAY_ALLOW_DANCING
	int x;
	int y;
#endif
};

/**
 * Get info from last skill used of character and append data struct if needed
 */
static struct skill_adelay_timer* get_adelays_timer(struct map_session_data* sd)
{
	struct skill_adelay_timer* data = getFromMSD(sd, 0);

	if (data == NULL) {
		CREATE(data, struct skill_adelay_timer, 1);
		data->last_skill_id = 0;
		data->tick = 0;
#ifdef ANIM_DELAY_ALLOW_DANCING
		data->x = -1;
		data->y = -1;
#endif
		addToMSD(sd, data, 0, true);
	}

	return data;
}

/**
 * Store last skill usage time and id
 */
static void set_adelays_timer(struct block_list* src, uint16 skill_id)
{
	nullpo_retv(src);
	struct map_session_data* sd = BL_CAST(BL_PC, src);

	if (sd == NULL)
		return;

	int64 tick = timer->gettick();

	struct skill_adelay_timer* adelays_data = get_adelays_timer(sd);
	adelays_data->last_skill_id = skill_id;
	adelays_data->tick = tick;
#ifdef ANIM_DELAY_ALLOW_DANCING
	adelays_data->x = sd->bl.x;
	adelays_data->y = sd->bl.y;
#endif
}

/**
 * Validate "Delay" entry
 */
static void validate_delay(struct config_setting_t* conf, struct skill_adelay_entry* tmp)
{
	nullpo_retv(conf);
	nullpo_retv(tmp);

	tmp->delay = 0;
	int delay;

	if (libconfig->setting_lookup_int(conf, "Delay", &delay) == CONFIG_TRUE) {
		tmp->delay = delay;
	}
}

/**
 * Validate "DelayOthers" entry
 */
static void validate_delay_others(struct config_setting_t* conf, struct skill_adelay_entry* tmp)
{
	nullpo_retv(conf);
	nullpo_retv(tmp);

	tmp->delay_others = 0;
	int delay_others;

	if (libconfig->setting_lookup_int(conf, "DelayOthers", &delay_others) == CONFIG_TRUE) {
		tmp->delay_others = delay_others;
	}
}

/**
 * Validate "Bypass" entry
 */
static void validate_bypass(struct config_setting_t* conf, struct skill_adelay_entry* tmp)
{
#ifdef ANIM_DELAY_ALLOW_BYPASS
	nullpo_retv(conf);
	nullpo_retv(tmp);

	tmp->bypass = false;
	bool bypass;

	if (libconfig->setting_lookup_bool_real(conf, "Bypass", &bypass) == CONFIG_TRUE) {
		tmp->bypass = bypass;
	}
#endif
}

/**
 * Validate "MaxDelay" entry
 */
static void validate_max_delay(struct config_setting_t* conf, struct skill_adelay_entry* tmp)
{
	nullpo_retv(conf);
	nullpo_retv(tmp);

	tmp->max_delay = 0;
	int max_delay;

	if (libconfig->setting_lookup_int(conf, "MaxDelay", &max_delay) == CONFIG_TRUE) {
		tmp->max_delay = max_delay;
	}
}

/**
 * Reads animation delay info from db/skill_adelay.conf
 */
static void read_skill_animation_delays(bool minimal)
{
	struct config_t skill_adelay_conf;
	struct config_setting_t *sk = NULL;
	char config_filename[280];
	int i = 0;

	snprintf(config_filename, sizeof(config_filename), "%s/skill_adelay.conf", map->db_path);
	if (!libconfig->load_file(&skill_adelay_conf, config_filename)) {
		ShowError("Could not read file %s/skill_adelay.conf\n", map->db_path);
		return;
	}

	while ((sk = libconfig->setting_get_elem(skill_adelay_conf.root, i++))) {
		const char *sk_name = config_setting_name(sk);
		int skill_id = skill->name2id(sk_name);

		if (skill_id == 0) {
			ShowWarning("server_adelays: unknown skill '%s'\n", sk_name);
			continue;
		}


		int index = skill->get_index(skill_id);
		struct skill_adelay_entry *tmp;
		skill_adelay_list[index] = CREATE(tmp, struct skill_adelay_entry, 1);

		validate_delay(sk, tmp);
		validate_delay_others(sk, tmp);
		validate_max_delay(sk, tmp);
		validate_bypass(sk, tmp);
	}

	libconfig->destroy(&skill_adelay_conf);
}

/**
 * Checks if it possible to use the requested skill_id taking into account
 * the animation delay of the last used skill
 */
static int post_skill_not_ok(int retVal, uint16 skill_id, struct map_session_data* sd)
{
	if (retVal == 1) {
		// It's already not ok to perform this skill
		return 1;
	}

	struct skill_adelay_timer* data = get_adelays_timer(sd);

	if (data->last_skill_id == 0) {
		return retVal;
	}
#ifdef ANIM_DELAY_ALLOW_DANCING
	if (data->x != sd->bl.x || data->y != sd->bl.y) {
		// User is moving or skill-dancing
		return retVal;
	}
#endif

	int last_skill_index = skill->get_index(data->last_skill_id);
	int current_skill_index = skill->get_index(skill_id);
	struct skill_adelay_entry *last_skill_adelay = skill_adelay_list[last_skill_index];
	struct skill_adelay_entry *current_skill_adelay = skill_adelay_list[current_skill_index];

	if (!last_skill_adelay || !current_skill_adelay) {
		return 0;
	}

#ifdef ANIM_DELAY_ALLOW_BYPASS
	if (current_skill_adelay->bypass) {
		// Skill ignores any delay
		return 0;
	}
#endif

	uint32 sd_adelay = status_get_adelay(&sd->bl);
	int delay = last_skill_adelay->delay;

	if (data->last_skill_id != skill_id && last_skill_adelay->delay_others != 0) {
		delay = last_skill_adelay->delay_others;
	}

	if (delay < 0) { // Delay is a percentage over character's adelay
		delay = abs(delay) * sd_adelay / 100;
	}

	if (last_skill_adelay->max_delay != 0 && delay > last_skill_adelay->max_delay) {
		delay = last_skill_adelay->max_delay;
	}

	int64 tick = timer->gettick();
	int64 tick_diff = tick - data->tick;

	if (tick_diff < (delay * ANIM_DELAY_LENIENCY / 100)) {
		// Last skill delay still in place
		return 1;
	}

	return 0;
}

/****************************/
/* Hooks */
static int post_clif_skill_nodamage(int retVal, struct block_list* src, struct block_list* dst, uint16 skill_id, int heal, int fail)
{
	set_adelays_timer(src, skill_id);
	return retVal;
}

static void post_clif_skill_poseffect(struct block_list* src, uint16 skill_id, int val, int x, int y, int64 tick)
{
	switch (skill_id) {
	// Avoid skills that use poseffect timely
	case WZ_METEOR:
		break;
	default:
		set_adelays_timer(src, skill_id);
	}
}

static int post_clif_skill_damage(int retVal, struct block_list* src, struct block_list* dst, int64 tick, int sdelay, int ddelay, int64 in_damage, int div, uint16 skill_id, uint16 skill_lv, enum battle_dmg_type type)
{
	switch (skill_id) {
	// Avoid registering the delay of skills that do timed damage
	case BA_DISSONANCE:
		break;
	default: 
		if (sdelay > 0)
			set_adelays_timer(src, skill_id);
	}

	return retVal;
}

// Plugin initialization
HPExport void plugin_init(void)
{	
	/* Load skill delays entries */
	addHookPost(skill, read_db, read_skill_animation_delays);
	/* Hook to check skill delays */
	addHookPost(skill, not_ok, post_skill_not_ok);
	/* Hooks to register performed skills */
	addHookPost(clif, skill_nodamage, post_clif_skill_nodamage);
	addHookPost(clif, skill_poseffect, post_clif_skill_poseffect);
	addHookPost(clif, skill_damage, post_clif_skill_damage);
}
