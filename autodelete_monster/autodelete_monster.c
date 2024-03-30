//===== Hercules/Heracles Plugin ==============================
//= auto delete monster plugin
//===== By: ===================================================
//= csnv
//===== Version: ======================================
//= 1.1
//===== Description: ==========================================
//= Enables summoning monsters that will be removed, if not
//= killed previously, after the specified time in seconds.
//= Syntax (same as monster command, duration in 8th position):
//= admonster <map name>,<x>,<y>,<xs>,<ys>%TAB%monster%TAB%<monster name>%TAB%<mob id>,<amount>,<delay1>,<delay2>,<duration>{,<event>,<mob size>,<mob ai>}
//= Dead branch example that auto deletes monsters in 1 hour:
//=   admonster "this",-1,-1,"--ja--",-1,1,3600,"";
//===== Repository: ===========================================
//= https://github.com/csnv/hercules-plugins
//=============================================================

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
#include "map/mob.h"
#include "map/script.h"
#include "map/instance.h"
#include "map/battle.h"
#include "map/guild.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h"

HPExport struct hplugin_info pinfo = {
	"monster_command_autodelete",			// Plugin name
	SERVER_TYPE_MAP,	// Which server types this plugin works with?
	"1.0",				// Plugin version
	HPM_VERSION,		// HPM Version (don't change, macro is automatically updated)
};

// Auto delete mob
static int mob_timer_delete2(int tid, int64 tick, int id, intptr_t data)
{
	struct mob_data* md = map->id2md(id);

	if (md != NULL) {
		if (md->deletetimer != tid) {
			ShowError("monster-auto-delete-plugin: Timer mismatch: %d != %d\n", tid, md->deletetimer);
			return 0;
		}
		md->deletetimer = INVALID_TIMER;
		status_kill(&md->bl);
	}
	return 0;
}

// admonster script command
// Just like monster command but additional duration argument at position 8
static BUILDIN(monsterautodelete) {
	const char* mapn = script_getstr(st, 2);
	int x = script_getnum(st, 3);
	int y = script_getnum(st, 4);
	const char* str = script_getstr(st, 5);
	int class_ = script_getnum(st, 6);
	int amount = script_getnum(st, 7);
	int duration = script_getnum(st, 8);
	const char* event = "";
	unsigned int size = SZ_SMALL;
	unsigned int ai = AI_NONE;
	int mob_id;

	int16 m;

	if (script_hasdata(st, 9))
	{
		event = script_getstr(st, 9);
		script->check_event(st, event);
	}

	if (script_hasdata(st, 10))
	{
		size = script_getnum(st, 10);
		if (size > 3)
		{
			ShowWarning("buildin_monster: Attempted to spawn non-existing size %u for monster class %d\n", size, class_);
			return false;
		}
	}

	if (script_hasdata(st, 11))
	{
		ai = script_getnum(st, 11);
		if (ai > AI_FLORA) {
			ShowWarning("buildin_monster: Attempted to spawn non-existing ai %u for monster class %d\n", ai, class_);
			return false;
		}
	}

	if (class_ >= 0 && !mob->db_checkid(class_))
	{
		ShowWarning("buildin_monster: Attempted to spawn non-existing monster class %d\n", class_);
		return false;
	}

	struct map_session_data* sd = map->id2sd(st->rid);
	if (sd != NULL && strcmp(mapn, "this") == 0) {
		m = sd->bl.m;
	}
	else {
		if ((m = map->mapname2mapid(mapn)) == -1) {
			ShowWarning("buildin_monster: Attempted to spawn monster class %d on non-existing map '%s'\n", class_, mapn);
			return false;
		}

		if (map->list[m].flag.src4instance && st->instance_id >= 0) { // Try to redirect to the instance map, not the src map
			if ((m = instance->mapid2imapid(m, st->instance_id)) < 0) {
				ShowError("buildin_monster: Trying to spawn monster (%d) on instance map (%s) without instance attached.\n", class_, mapn);
				return false;
			}
		}
	}

	mob_id = mob->once_spawn(sd, m, x, y, str, class_, amount, event, size, ai);

	struct mob_data* md = map->id2md(mob_id);
	md->deletetimer = timer->add(timer->gettick() + duration * 1000, mob_timer_delete2, md->bl.id, 0);

	script_pushint(st, mob_id);
	return true;
}

// Delete timer when the mob dies, otherwise mob_dead tries to delete it unsuccessfully
static int mob_dead_pre(struct mob_data** md, struct block_list** src, int* type) {
	int tid = (* md)->deletetimer;
	if (tid != INVALID_TIMER) {
		const struct TimerData* td = timer->get(tid);
		if (td->func == mob_timer_delete2) {
			timer->delete(tid, mob_timer_delete2);
			(*md)->deletetimer = INVALID_TIMER;
		}
	}
	return 0;
}

// Plugin initialization
HPExport void plugin_init(void)
{
	addScriptCommand("admonster", "siisiii???", monsterautodelete);
	timer->add_func_list(mob_timer_delete2, "monster-auto-delete-plugin:mob_timer_delete2");
	addHookPre(mob, dead, mob_dead_pre);
}
