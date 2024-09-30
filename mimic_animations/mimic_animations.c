//===== Hercules/Heracles Plugin ===================================
//= Skill animation mimic plugin
//===== By: ========================================================
//= csnv
//===== Version: ===================================================
//= 1.3
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
	int skill_id;     // Skill ID
	int start;        // Start animation after X ms have passed (-1 for sdelay)
	int interval;     // Time between actions
	int motion_speed; // Animation speed for each action
	int motion_count; // Number of actions per animation
	bool spin;        // Make target spin (Sonic blow)
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
	struct skill_animation_data* anim_data; // Animation data (check struct skill_animation_data)
	int target_id;                          // Skill target id
	uint16 target_x;                        // Target's X coordinate at each iteration
	uint16 target_y;                        // Target's Y coordinate at each iteration
	int8 dir;                               // Target's direction - only useful when the skill makes the target spin
	int8 step;                              // Iteration step
};

struct skill_animation_timer {
	int tid;
};

void send_dir_packet(struct block_list* bl, int target_id, int8 dir);
void send_attack_packet(struct block_list* bl, int target_id, int motion_speed);
static int use_animation(int tid, int64 tick, int id, intptr_t data);
static int check_used_skill(int retVal, struct block_list* src, struct block_list* dst, int64 tick, int sdelay, int ddelay, int64 in_damage, int div, uint16 skill_id, uint16 skill_lv, enum battle_dmg_type type);
static struct skill_animation_data* get_animation_info(int skill_id);
static void on_unit_teleported(struct map_session_data* sd, short m, int x, int y);
static void clear_timer(struct block_list* bl, bool free_res);

HPExport struct hplugin_info pinfo = {
	"mimic_animations",   // Plugin name
	SERVER_TYPE_MAP,      // Which server types this plugin works with?
	"1.3",                // Plugin version
	HPM_VERSION,          // HPM Version (don't change, macro is automatically updated)
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

	clif->send(&p, sizeof(p), bl, AREA);
}

/**
 * Force display a certain direction on the unit
 */
void send_dir_packet(struct block_list* bl, int target_id, int8 dir)
{
	unsigned char buf[64];

	nullpo_retv(bl);
	WBUFW(buf, 0) = 0x9c;
	WBUFL(buf, 2) = target_id ? target_id : bl->id;
	WBUFW(buf, 6) = bl->type == BL_PC ? BL_UCCAST(BL_PC, bl)->head_dir : 0;
	WBUFB(buf, 8) = dir;

	clif->send(buf, packet_len(0x9c), bl, AREA);

	if (clif->isdisguised(bl)) {
		WBUFL(buf, 2) = -bl->id;
		WBUFW(buf, 6) = 0;
		clif->send(buf, packet_len(0x9c), bl, SELF);
	}
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
	struct block_list* target = map->id2bl(skill_env->target_id);

	if (target) {
		skill_env->target_x = target->x;
		skill_env->target_y = target->y;
	}

	if (!status->isdead(bl)) {
		send_attack_packet(bl, skill_env->target_id, anim_data->motion_speed);

		// Fixes direction of units attacking while moving
		int dir = map->calc_dir(bl, skill_env->target_x, skill_env->target_y);
		send_dir_packet(bl, 0, dir);
	}

	// Skill requires target to spin
	if (anim_data->spin && skill_env->dir != -1) {
		send_dir_packet(bl, skill_env->target_id, skill_env->dir);
		skill_env->dir = unit_get_ccw90_dir(skill_env->dir);
	}

	skill_env->step++;
	int timer_id = INVALID_TIMER;

	if (skill_env->step < anim_data->motion_count)
		timer_id = timer->add(tick + anim_data->interval, use_animation, id, (intptr)skill_env);
	else
		aFree((void*)data);

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
		return retVal; // Not a skill that requires this

	int start_time = anim_data->start == -1 ? sdelay : anim_data->start;
	int target_id = dst->id;
	enum unit_dir dir = in_damage != 0 ? unit->getdir(dst) : -1; // If source misses, there's no direction change on target

	struct skill_environment_data* skill_env = aMalloc(sizeof(struct skill_environment_data));
	skill_env->anim_data = anim_data;
	skill_env->target_id = target_id;
	if (anim_data->spin && dir != -1)
		dir = unit_get_ccw90_dir(dir);

	skill_env->target_x = dst->x;
	skill_env->target_y = dst->y;
	skill_env->dir = dir;
	skill_env->step = 0;

	clear_timer(src, true); // Remove previous animation if any
	int tid = timer->add(tick + start_time + anim_data->interval, use_animation, src->id, (intptr)skill_env);

	if (src->type == BL_PC) {
		struct skill_animation_timer* skill_timer = get_timer(BL_CAST(BL_PC, src));
		skill_timer->tid = tid;
	}

	return retVal;
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

	const struct TimerData* td = timer->get(skill_timer->tid);
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