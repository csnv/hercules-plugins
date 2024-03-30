//===== Hercules/Heracles Plugin ===================================
//= Skill animation mimic plugin
//===== By: ========================================================
//= csnv
//===== Version: ===================================================
//= 1.0
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
	int dir;
};

void send_attack_packet(struct block_list* bl, int motion_speed);
void send_simple_dir(struct block_list* src, int target_id, int dir);
static int use_animation(int tid, int64 tick, int id, intptr_t data);
static int check_used_skill(int retVal, struct block_list* src, struct block_list* dst, int64 tick, int sdelay, int ddelay, int64 in_damage, int div, uint16 skill_id, uint16 skill_lv, enum battle_dmg_type type);
static struct skill_animation_data* get_animation_info(int skill_id);
static int calc_dir_counter_clockwise(int dir);

HPExport struct hplugin_info pinfo = {
	"mimic_animations",   // Plugin name
	SERVER_TYPE_MAP,             // Which server types this plugin works with?
	"1.0",                       // Plugin version
	HPM_VERSION,                 // HPM Version (don't change, macro is automatically updated)
};

/*
 * Sends a single attack animation packet
 * This method mimics the attack animation once.
 */
void send_attack_packet(struct block_list* bl, int motion_speed)
{
	unsigned char buf[32];
	nullpo_retv(bl);
	WBUFW(buf, 0) = 0x8a;
	WBUFL(buf, 2) = bl->id;
	WBUFL(buf, 14) = motion_speed;
	WBUFB(buf, 26) = BDT_CRIT; // BDT_CRIT should display that star-like multicolored hit effect, but doesn't

	clif->send(buf, packet_len(0x8a), bl, AREA);
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
	send_attack_packet(bl, anim_data->motion_speed);

	if (anim_data->spin && skill_env->dir != -1) {
		send_simple_dir(bl, skill_env->target_id, skill_env->dir);
	}

	aFree((void *)data);

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
	int dir = in_damage != 0 ? unit->getdir(dst) : -1; // If source misses, there's no direction change on target

	for (int n = 0; n < anim_data->motion_count; n++) {
		struct skill_environment_data* skill_env = aMalloc(sizeof(struct skill_environment_data));
		skill_env->anim_data = anim_data;
		skill_env->target_id = target_id;
		if (anim_data->spin && dir != -1)
			dir = calc_dir_counter_clockwise(dir);
		skill_env->dir = dir;
		
		timer->add(tick + start_time + anim_data->interval * n, use_animation, src->id, (intptr)skill_env);
	}

	return 0;
}

/*
 * Calcs direction counter clockwise
 */
static int calc_dir_counter_clockwise(int dir) {
	dir += 2;
	if (dir >= UNIT_DIR_MAX)
		dir = dir - UNIT_DIR_MAX;
	return dir;
}

/*
 * Gets the matching animation data of a skill, if any
 */
static struct skill_animation_data *get_animation_info(int skill_id)
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
#endif
}