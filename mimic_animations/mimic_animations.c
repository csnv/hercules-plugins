//===== Hercules/Heracles Plugin ===================================
//= Skill animation mimic plugin
//===== By: ========================================================
//= csnv
//===== Version: ===================================================
//= 1.1
//===== Description: ===============================================
//= Imitates skill animations removed in 2018+ clients
//===== Repository: ================================================
//= https://github.com/csnv/hercules-plugins
//==================================================================

#include "common/hercules.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "common/HPMi.h"
#include "common/memmgr.h"
#include "common/nullpo.h"
#include "common/packets.h"
#include "common/socket.h"
#include "common/timer.h"

#include "map/battle.h"
#include "map/clif.h"
#include "map/pc.h"
#include "map/skill.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h"

struct skill_animation_data {
	int skill_id; // Skill ID
	int start; // Start animation after X ms have passed (-1 for sdelay)
	int interval; // Time between actions
	int motion_speed; // Animation speed for each action
	int motion_count; // Number of actions per animation
	bool spin; // Make target spin (Sonic blow)
};

/*
 * List of skills configuration
 */
struct skill_animation_data sad_list[] = {
	{ AS_SONICBLOW, -1, 180, 150, 8, true },
	{ CG_ARROWVULCAN, -1, 200, 100, 9, false }
}; // ;(

/*
 * Auxiliary data
 */
struct skill_environment_data {
	struct skill_animation_data* anim_data;
	int target_id;
	int8 dir;
	int8 step;
};

struct skill_animation_timer {
	int64 tid;
};

void send_attack_packet(struct block_list* bl, int target_id, int motion_speed);
void send_simple_dir(struct block_list* src, int target_id, int dir);
static int use_animation(int tid, int64 tick, int id, intptr_t data);
static int check_used_skill(int retVal, struct block_list* src, struct block_list* dst, int64 tick, int sdelay, int ddelay, int64 in_damage, int div, uint16 skill_id, uint16 skill_lv, enum battle_dmg_type type);
static struct skill_animation_data* get_animation_info(int skill_id);
static void on_unit_teleported(struct map_session_data* sd, short m, int x, int y);
static void clear_timer(struct block_list* bl, bool free_res);

HPExport struct hplugin_info pinfo = {
	"mimic_animations",   // Plugin name
	SERVER_TYPE_MAP,             // Which server types this plugin works with?
	"1.0",                       // Plugin version
	HPM_VERSION,                 // HPM Version (don't change, macro is automatically updated)
};

/**
 * Recover TID of current timer, if any
 */
static struct skill_animation_timer* get_timer(struct map_session_data* sd)
{
	struct skill_animation_timer* data = getFromMSD(sd, 0);

	if (data == NULL) {
		CREATE(data, struct skill_animation_timer, 1);
		data->tid = INVALID_TIMER;

		addToMSD(sd, data, 0, true);
	}

	return data;
}

/*
 * Sends a single attack animation packet
 * This method mimics the attack animation once.
 */
void send_attack_packet(struct block_list* bl, int target_id, int motion_speed)
{

	struct packet_damage p;

	p.PacketType = damageType;
	p.GID = bl->id;
	p.attackMT = motion_speed;
	p.count = 1;
	p.action = BDT_NORMAL;
	if (bl->type == BL_MOB) {
		// Sending the target fixes monsters flipping arround when attacking
		// This also reproduces additional hitting animations, but better than nothing
		p.targetGID = target_id;
	}

	clif->send(&p, sizeof(p), bl, AREA);
}

/*
 * Sends a single direction change packet
 * Used for the spinning effect of some skills.
 */
void send_simple_dir(struct block_list* src, int target_id, int dir)
{
	unsigned char buf[64];
	WBUFW(buf, 0) = 0x9c;
	WBUFL(buf, 2) = target_id;
	WBUFW(buf, 6) = 0;
	WBUFB(buf, 8) = dir;

	clif->send(buf, packet_len(0x9c), src, AREA);
}

/*
 * Timer function triggered by skill usage.
 * Sends attack packet and triggers direction change depending on the skill animation data.
 */
static int use_animation(int tid, int64 tick, int id, intptr_t data) {
	struct block_list* bl = map->id2bl(id);

	if (bl == NULL) {
		aFree((void*)data);
		return 0;
	}

	struct skill_environment_data* skill_env = (struct skill_environment_data*)data;

	struct skill_animation_data* anim_data = skill_env->anim_data;
	send_attack_packet(bl, skill_env->target_id, anim_data->motion_speed);

	if (anim_data->spin && skill_env->dir != -1) {
		send_simple_dir(bl, skill_env->target_id, skill_env->dir);
		skill_env->dir = unit_get_ccw90_dir(skill_env->dir);
	}

	skill_env->step++;
	int64 timer_id = INVALID_TIMER;

	if (skill_env->step < anim_data->motion_count) {
		timer_id = timer->add(tick + anim_data->interval, use_animation, id, (intptr)skill_env);
	} else {
		aFree((void*)data);
	}

	if (bl->type == BL_PC) {
		struct skill_animation_timer* skill_timer = get_timer(BL_CAST(BL_PC, bl));
		skill_timer->tid = timer_id;
	}

	return 0;
}

/*
 * Post hook for clif_skill_damage.
 * Processes skill animation based on the configured skill animation data.
 */
static int check_used_skill(int retVal, struct block_list* src, struct block_list* dst, int64 tick, int sdelay, int ddelay, int64 in_damage, int div, uint16 skill_id, uint16 skill_lv, enum battle_dmg_type type)
{
	struct skill_animation_data* anim_data = get_animation_info(skill_id);

	if (anim_data == NULL)
		return 0; // Not a skill that requires this

	int start_time = anim_data->start == -1 ? sdelay : anim_data->start;
	int target_id = dst->id;
	enum unit_dir dir = in_damage != 0 ? unit->getdir(dst) : -1; // If source misses, there's no direction change on target

	struct skill_environment_data* skill_env = aMalloc(sizeof(struct skill_environment_data));
	skill_env->anim_data = anim_data;
	skill_env->target_id = target_id;
	if (anim_data->spin && dir != -1)
		dir = unit_get_ccw90_dir(dir);
	skill_env->dir = dir;
	skill_env->step = 0;

	clear_timer(src, true); // Remove previous animation if any
	int64 tid = timer->add(tick + start_time + anim_data->interval, use_animation, src->id, (intptr)skill_env);

	if (src->type == BL_PC) {
		struct skill_animation_timer* skill_timer = get_timer(BL_CAST(BL_PC, src));
		skill_timer->tid = tid;
	}

	return 0;
}

/**
 * Remove animation when teleporting away (player only)
 */
static void on_unit_teleported(struct map_session_data* sd, short m, int x, int y) {
	clear_timer(&sd->bl, true);
}


/**
 * Removes any pending animation
 */
static void clear_timer(struct block_list* bl, bool free_res) {
	if (bl == NULL || bl->type != BL_PC)
		return;

	struct map_session_data* sd = BL_CAST(BL_PC, bl);
	struct skill_animation_timer* skill_timer = get_timer(sd);

	if (skill_timer->tid == INVALID_TIMER)
		return;

	struct TimerData *td = timer->get(skill_timer->tid);
	timer->delete(skill_timer->tid, use_animation);
	skill_timer->tid = INVALID_TIMER;

	if (free_res && td->data) {
		aFree((void*)td->data);
	}
}

/*
 * Gets the matching animation data of a skill, if any
 */
static struct skill_animation_data* get_animation_info(int skill_id)
{
	for (int i = 0; i < ARRAYLENGTH(sad_list); i++) {
		if (sad_list[i].skill_id == skill_id) {
			return &sad_list[i];
		}
	}

	return NULL;
}

HPExport void plugin_init(void)
{
#if PACKETVER >= 20181128
	addHookPost(clif, skill_damage, check_used_skill);
	addHookPost(clif, changemap, on_unit_teleported);
	ShowInfo("Mimic animations plugin loaded.\n");
#endif
}