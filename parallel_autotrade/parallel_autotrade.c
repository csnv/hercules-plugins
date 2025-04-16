//===== Hercules Plugin ======================================
//= Parallel autotrade
//===== By: ==================================================
//= csnv
//===== Current Version: =====================================
//= 1.0alpha
//===== Compatible With: ===================================== 
//= Hercules, Heracles
//===== Description: =========================================
//= Create an autotrade clone that allows you to use
//= characters in the same account instead of forcing you to
//= completely log out
//===== Usage: =================================  
//= -- AtCommands --
//= @autotrade
//= @at
//===== Important: ==========================================
//= Increase MAX_MOB_DB and MOB_CLONE_START in mob.h for
//= accommodating the necessary number of @at clones
//=
//= Compatible with AUTOTRADE_PERSISTENCY
//===========================================================

#include "stdlib.h"
#include "common/hercules.h"
#include "common/core.h"
#include "common/msgtable.h"
#include "common/memmgr.h"
#include "common/nullpo.h"
#include "common/socket.h"
#include "common/sql.h"
#include "common/timer.h"
#include "common/utils.h"
#include "char/char.h"
#include "char/mapif.h"
#include "map/achievement.h"
#include "map/atcommand.h"
#include "map/battle.h"
#include "map/channel.h"
#include "map/chrif.h"
#include "map/clif.h"
#include "map/mob.h"
#include "map/pc.h"
#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h"

// Max number of autotrade characters allowed per account. Use 0 to disable check
#define MAX_AUTOTRADE_PER_ACCOUNT 3

// Support autotrade persistency for this plugin only (overwrites default @at persistency)
#define SUPPORT_AT_PERSISTENCY

// Packet number for inter server communication from char to map server
#define CHAR_MAP_PACKET_ID 0x15

HPExport struct hplugin_info pinfo = {
	"parallel_autotrade",
	SERVER_TYPE_CHAR | SERVER_TYPE_MAP,
	"1.0",
	HPM_VERSION,
};

enum trade_type {
	TD_VC,
	TD_BC
};

struct trade_options {
	int pushcart;
	int time;
	int save_timer;
	int quit_timer;
};

struct trade_clone {
	enum trade_type type;
	struct mob_data* md;
	struct map_session_data sd;
	struct trade_options options;
};

// Databases to keep track of clones
struct DBMap* clone_db;
struct DBMap* vender_db;
struct DBMap* buyer_db;

// Table names
const char cart_db[256] = "cart_inventory";
const char inventory_db[256] = "inventory";
const char character_db[256] = "char";
const char login_db[256] = "login";
const char sc_data_db[256] = "sc_data";

//====== Function declarations =========
static int at_clone_spawn_vending(struct map_session_data* sd);
static void clif_getareachar_unit_post(struct map_session_data* sd, struct block_list* bl);
static struct trade_clone* id2tc(int id);
static void save(struct trade_clone* tc);
static void clif_authok_post(struct map_session_data* sd);
static void remove_clone(int char_id);
static int getitemdata_from_sql(struct item* items, int max, int guid, enum inventory_table_type table);
#if defined(AUTOTRADE_PERSISTENCY) && defined(SUPPORT_AT_PERSISTENCY)
static int memitemdata_to_sql(const struct item* p_items, int current_size, int guid, enum inventory_table_type table);
static void pc_autotrade_load_pre(void);
static void autotrade_clone(struct trade_clone* tc);
static void autotrade_populate(struct trade_clone* tc);
#endif
static int char_getgender(char sex, char sex2);
static void save_zeny(struct trade_clone* tc);
static void save_timeout(struct trade_clone* tc, bool remove);
static int char_delete_char_sql_post(int result, int char_id);
static void parse_delete_char_packet(int fd);
static int battle_check_target_post(int retval, struct block_list* src, struct block_list* target, int flag);
static int timeout_timer(int tid, int64 tick, int id, intptr_t data);
static int map_quit_timer(int tid, int64 tick, int id, intptr_t data);
// ====================================

/**
 * Creates a clone of the current user as well as its cart, inventory, vending and buying shop items
 */
static int at_clone_spawn_vending(struct map_session_data* sd)
{
	int class_;
	int16 m = sd->bl.m;
	int16 x = sd->bl.x;
	int16 y = sd->bl.y;

	ARR_FIND(MOB_CLONE_START, MOB_CLONE_END, class_, mob->db_data[class_] == NULL);
	if (class_ >= MOB_CLONE_END)
		return 0;

	// Copy name, looks, lv, etc
	struct mob_db* mdb = mob->db_data[class_] = (struct mob_db*)aCalloc(1, sizeof(struct mob_db));
	struct status_data* mstatus = &mdb->status;

	safestrncpy(mdb->sprite, sd->status.name, NAME_LENGTH);
	safestrncpy(mdb->name, sd->status.name, NAME_LENGTH);
	safestrncpy(mdb->jname, sd->status.name, NAME_LENGTH);

	memcpy(mstatus, &sd->base_status, sizeof(struct status_data));
	mstatus->mode = 0;
	mstatus->hp = mstatus->max_hp = 1;
	mdb->lv = status->get_lv(&sd->bl);
	mdb->option = 0;
	memcpy(&mdb->vd, &sd->vd, sizeof(struct view_data));

	// Create monster
	struct mob_data* md = mob->once_spawn_sub(&sd->bl, m, x, y, sd->status.name, class_, "", SZ_SMALL, AI_NONE, 0);
	if (md == NULL)
		return 0;

	md->special_state.clone = 1;
	mob->spawn(md);
	unit->set_dir(&md->bl, unit->getdir(&sd->bl));

	// Notify clients
	if (sd->vd.dead_sit == 2)
		clif->sitting(&md->bl);

	if (map->list[sd->bl.m].users)
		clif->showvendingboard(&md->bl, sd->message, 0);

	struct trade_clone* tc;
	CREATE(tc, struct trade_clone, 1);
	tc->md = md;

	memcpy(&tc->sd.bl, &md->bl, sizeof(struct block_list));

	tc->type = sd->state.vending == 1 ? TD_VC : TD_BC;
	tc->sd.status.zeny = sd->status.zeny;
	tc->sd.status.char_id = sd->status.char_id;
	tc->sd.status.account_id = sd->status.account_id;
	tc->sd.status.sex = sd->status.sex;
	tc->sd.weight = sd->weight;
	tc->sd.max_weight = sd->max_weight;
	tc->sd.vend_num = sd->vend_num;
	tc->sd.vender_id = sd->vender_id;
	tc->sd.buyer_id = sd->buyer_id;
	tc->sd.buyingstore.slots = sd->buyingstore.slots;
	tc->sd.buyingstore.zenylimit = sd->buyingstore.zenylimit;
	tc->sd.state.vending = sd->state.vending;
	tc->sd.state.buyingstore = sd->state.buyingstore;
	tc->sd.state.autotrade = 1;
	tc->sd.status.inventorySize = sd->status.inventorySize;
	pc->set_group(&tc->sd, sd->group_id);

	safestrncpy(tc->sd.message, sd->message, MESSAGE_SIZE);

	if (sd->sc.data[SC_PUSH_CART])
		tc->options.pushcart = sd->sc.data[SC_PUSH_CART]->val1;

	if (sd->sc.data[SC_AUTOTRADE]) {
		int64 tick = timer->gettick();
		tc->options.time = sd->sc.data[SC_AUTOTRADE]->total_tick;
		tc->options.quit_timer = timer->add(tick + tc->options.time, timeout_timer, md->bl.id, 1);
		tc->options.save_timer = INVALID_TIMER;
		if (tc->options.time > map->autosave_interval)
			tc->options.save_timer = timer->add_interval(tick + map->autosave_interval, timeout_timer, md->bl.id, 0, map->autosave_interval);
	}

	// Vending, buying, inventory and cart data
	memcpy(&tc->sd.vending, &sd->vending, sizeof(struct s_vending[MAX_VENDING]));
	memcpy(&tc->sd.buyingstore.items, sd->buyingstore.items, (sizeof(struct s_buyingstore_item[MAX_BUYINGSTORE_SLOTS])));
	memcpy(&tc->sd.status.inventory, &sd->status.inventory, sizeof(struct item[MAX_INVENTORY]));
	memcpy(&tc->sd.status.cart, &sd->status.cart, sizeof(struct item[MAX_CART]));

	int trade_id = tc->type == TD_VC ? tc->sd.vender_id : tc->sd.buyer_id;
	if (sd->state.vending == 1) {
		idb_put(vender_db, trade_id, tc); // Vender id to TC
		idb_put(vending->db, sd->status.char_id, &tc->sd); // Char id to TC (vending)
	} else {
		idb_put(buyer_db, trade_id, tc);
#ifdef HERACLES_VERSION
		idb_put(buyingstore->db, sd->status.char_id, &tc->sd);
#endif
	}

	idb_put(clone_db, sd->status.char_id, tc); // Char id to TC (autotraders only)

	if (tc->sd.state.vending == 1)
		pc->autotrade_update(&tc->sd, PAUC_START);

	addToMOBDATA(md, tc, 0, true);

	sd->state.vending = 0; // Remove flag in original sd so the clone doesn't get removed from vending
	sd->state.buyingstore = 0; // Same with buying store

	return md->bl.id;
}

/**
 * Prevent trading clones to receive damage
 */
static int status_damage_pre(struct block_list** src, struct block_list** target, int64* in_hp, int64* in_sp, int* walkdelay, int* flag)
{
	struct block_list* tgt = *target;
	if (tgt->type != BL_MOB)
		return 0;

	struct trade_clone* td = getFromMOBDATA((struct mob_data*)tgt, 0);
	if (td == NULL)
		return 0;

	hookStop(); // Don't let trade clones receive damage
	return 0;
}

/**
 * clif->getareachar_unit posthook
 *
 * Notifies approaching clients of the visual details (pushcat, vending/buying board)
 */
static void clif_getareachar_unit_post(struct map_session_data* sd, struct block_list* bl)
{
	if (bl->type != BL_MOB)
		return;

	struct trade_clone* td = getFromMOBDATA((struct mob_data*)bl, 0);
	if (td == NULL)
		return;

	if (td->options.pushcart)
		clif->sc_load(&sd->bl, bl->id, SELF, status->get_sc_icon(SC_ON_PUSH_CART), td->options.pushcart, 0, 0);

	if (td->type == TD_VC)
		clif->showvendingboard(bl, td->sd.message, sd->fd);
	else if (td->type == TD_BC)
		clif->buyingstore_entry_single(bl, td->sd.message, sd->fd);
}

/**
 * map->id2sd posthook
 *
 * Converts mob_data id into a corresponding map_session_data if it's a clone
 */
static struct map_session_data* map_id2sd_post(struct map_session_data* retval, int id)
{
	if (retval != NULL)
		return retval;

	struct trade_clone* tc = id2tc(id);

	return tc != NULL ? &tc->sd : NULL;
}

/**
 * chrif->save prehook
 *
 * Skip saving clones' data
 */
static bool chrif_save_pre(struct map_session_data** sd, int* flag)
{
	struct trade_clone* tc = idb_get(clone_db, (*sd)->status.char_id);

	if (tc == NULL || tc->sd.bl.id != (*sd)->bl.id) // Real SD being saved
		return true;

	hookStop();
	save(tc);

	return true;
}

/**
 * Quickly convert a mob id into the corresponding clone
 */
static struct trade_clone* id2tc(int id)
{
	struct mob_data* md;
	if ((md = map->id2md(id)) != NULL)
		return getFromMOBDATA(md, 0);
	else
		return NULL;
}

/**
 * Saves clone inventory data of a character into database
 */
static void save(struct trade_clone* tc)
{
	struct map_session_data* sd = &tc->sd;
	if (tc->type == TD_BC) // Don't attempt for vending, inventory is not recovered in persistence mode
		memitemdata_to_sql(sd->status.inventory, -1, sd->status.char_id, TABLE_INVENTORY);
	memitemdata_to_sql(sd->status.cart, -1, sd->status.char_id, TABLE_CART);
	save_zeny(tc);
}

/**
 * Saves clone's zeny into database
 */
static void save_zeny(struct trade_clone* tc)
{
	if (SQL_ERROR == SQL->Query(map->mysql_handle, "UPDATE `%s` SET `zeny`='%d'"
		" WHERE `account_id`='%d' AND `char_id` = '%d'",
		character_db, tc->sd.status.zeny, tc->sd.status.account_id, tc->sd.status.char_id))
	{
		Sql_ShowDebug(map->mysql_handle);
	}
}

/**
 * Saves clone's timeout into database
 */
static void save_timeout(struct trade_clone* tc, bool remove)
{
	if (remove) {
		if (SQL_ERROR == SQL->Query(map->mysql_handle, "DELETE FROM `%s` WHERE `account_id`='%d' AND `char_id` = '%d' AND `type` = '%d'",
			sc_data_db, tc->sd.status.account_id, tc->sd.status.char_id, SC_AUTOTRADE))
		{
			Sql_ShowDebug(map->mysql_handle);
		}
	} else {
		if (SQL_ERROR == SQL->Query(map->mysql_handle, "UPDATE `%s` SET `tick`='%d'"
			" WHERE `account_id`='%d' AND `char_id` = '%d' AND `type` = '%d'",
			sc_data_db, tc->options.time, tc->sd.status.account_id, tc->sd.status.char_id, SC_AUTOTRADE))
		{
			Sql_ShowDebug(map->mysql_handle);
		}
	}
}

/**
 * map->quit prehook
 *
 * Skip quitting clone characters
 */
static int map_quit_pre(struct map_session_data** sd)
{
	if (sd == NULL)
		return 0;

	struct trade_clone* tc = idb_get(clone_db, (*sd)->status.char_id);
	if (tc == NULL || tc->sd.bl.id != (*sd)->bl.id) // Real SD going out
		return 0;

	hookStop();
	remove_clone(tc->sd.status.char_id);

	return 0;
}

/**
 * clif->authok posthook
 *
 * Removes clones when a matching char_id is selected
 */
static void clif_authok_post(struct map_session_data* sd)
{
	if (sd == NULL)
		return;

	remove_clone(sd->status.char_id);
}

/**
 * Closes boards and removes clone from server
 */
static void remove_clone(int char_id)
{
	struct trade_clone* tc = idb_get(clone_db, char_id);
	if (tc == NULL)
		return;

	struct mob_data* md = tc->md;
	if (tc->type == TD_VC) {
			clif->closevendingboard(&md->bl, 0);
		pc->autotrade_update(&tc->sd, PAUC_REMOVE);
		idb_remove(vending->db, char_id);
		idb_remove(vender_db, tc->sd.vender_id);
	} else {
			clif->buyingstore_disappear_entry(&tc->md->bl);
#ifdef HERACLES_VERSION
		idb_remove(buyingstore->db, char_id);
#endif
		idb_remove(buyer_db, tc->sd.buyer_id);
	}

	if (battle->bc->at_timeout) {
		if (tc->options.quit_timer != INVALID_TIMER) {
			timer->delete(tc->options.quit_timer, timeout_timer);
		}
		if (tc->options.save_timer != INVALID_TIMER) {
			timer->delete(tc->options.save_timer, timeout_timer);
		}
	}

	save_timeout(tc, true);
	idb_remove(clone_db, char_id);
	unit->free(&md->bl, CLR_OUTSIGHT);
}

/**
 * Clif->search_store_info_ack prehook
 *
 * Replaces ids from account_id to the mob data id of trade clones
 */
static void clif_searchstoreinfo_pre(struct map_session_data** _sd)
{
	struct map_session_data* sd = *_sd;
	struct trade_clone* tc;

	for (int i = 0; i < sd->searchstore.count; i++) {
		int store_id = sd->searchstore.items[i].store_id;
		if (sd->searchstore.type == SEARCHTYPE_VENDING)
			tc = idb_get(vender_db, store_id);
		else
			tc = idb_get(buyer_db, store_id);

		if (tc != NULL)
			sd->searchstore.items[i].account_id = tc->md->bl.id;
	}
}

/**
 * Retrieves item data from database
 */
static int getitemdata_from_sql(struct item* items, int max, int guid, enum inventory_table_type table)
{
	int i = 0;
	struct SqlStmt* stmt = NULL;
	const char* tablename = NULL;
	const char* selectoption = NULL;
	bool has_favorite = false;
	StringBuf buf;
	struct item item = { 0 }; // temp storage variable

	if (max > 0)
		nullpo_retr(-1, items);
	Assert_retr(-1, guid > 0);

	// Initialize the array.
	if (max > 0)
		memset(items, 0x0, sizeof(struct item) * max);

	switch (table) {
	case TABLE_INVENTORY:
		tablename = inventory_db;
		selectoption = "char_id";
		has_favorite = true;
		break;
	case TABLE_CART:
		tablename = cart_db;
		selectoption = "char_id";
		break;
	case TABLE_GUILD_STORAGE:
	case TABLE_STORAGE:
	default:
		ShowError("char_getitemdata_from_sql: Invalid table type %d!\n", (int)table);
		Assert_retr(-1, table);
		return -1;
	}

	StrBuf->Init(&buf);
	StrBuf->AppendStr(&buf, "SELECT `id`, `nameid`, `amount`, `equip`, `identify`, `refine`, `grade`, `attribute`, `expire_time`, `bound`, `unique_id`");
	for (i = 0; i < MAX_SLOTS; i++)
		StrBuf->Printf(&buf, ", `card%d`", i);
	for (i = 0; i < MAX_ITEM_OPTIONS; i++)
		StrBuf->Printf(&buf, ", `opt_idx%d`, `opt_val%d`", i, i);
	if (has_favorite)
		StrBuf->AppendStr(&buf, ", `favorite`");
	StrBuf->Printf(&buf, " FROM `%s` WHERE `%s`=?", tablename, selectoption);

	stmt = SQL->StmtMalloc(map->mysql_handle);
	if (SQL_ERROR == SQL->StmtPrepareStr(stmt, StrBuf->Value(&buf))
		|| SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_INT, &guid, sizeof guid)
		|| SQL_ERROR == SQL->StmtExecute(stmt)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		StrBuf->Destroy(&buf);
		return -1;
	}

	if (SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_INT, &item.id, sizeof item.id, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 1, SQLDT_INT, &item.nameid, sizeof item.nameid, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 2, SQLDT_SHORT, &item.amount, sizeof item.amount, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 3, SQLDT_UINT, &item.equip, sizeof item.equip, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 4, SQLDT_CHAR, &item.identify, sizeof item.identify, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 5, SQLDT_CHAR, &item.refine, sizeof item.refine, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 6, SQLDT_CHAR, &item.grade, sizeof item.grade, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 7, SQLDT_CHAR, &item.attribute, sizeof item.attribute, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 8, SQLDT_UINT, &item.expire_time, sizeof item.expire_time, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 9, SQLDT_UCHAR, &item.bound, sizeof item.bound, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 10, SQLDT_UINT64, &item.unique_id, sizeof item.unique_id, NULL, NULL)
		) {
		SqlStmt_ShowDebug(stmt);
	}

	for (i = 0; i < MAX_SLOTS; i++) {
		if (SQL_ERROR == SQL->StmtBindColumn(stmt, 11 + i, SQLDT_INT, &item.card[i], sizeof item.card[i], NULL, NULL))
			SqlStmt_ShowDebug(stmt);
	}

	for (i = 0; i < MAX_ITEM_OPTIONS; i++) {
		if (SQL_ERROR == SQL->StmtBindColumn(stmt, 11 + MAX_SLOTS + i * 2, SQLDT_INT16, &item.option[i].index, sizeof item.option[i].index, NULL, NULL)
			|| SQL_ERROR == SQL->StmtBindColumn(stmt, 12 + MAX_SLOTS + i * 2, SQLDT_INT16, &item.option[i].value, sizeof item.option[i].index, NULL, NULL))
			SqlStmt_ShowDebug(stmt);
	}

	if (has_favorite) {
		if (SQL_ERROR == SQL->StmtBindColumn(stmt, 11 + MAX_SLOTS + MAX_ITEM_OPTIONS * 2, SQLDT_CHAR, &item.favorite, sizeof item.favorite, NULL, NULL))
			SqlStmt_ShowDebug(stmt);
	}

	if (SQL->StmtNumRows(stmt) > 0) {
		i = 0;
		while (SQL_SUCCESS == SQL->StmtNextRow(stmt) && i < max) {
			items[i++] = item;
		}
	}

	SQL->StmtFree(stmt);
	StrBuf->Destroy(&buf);

	return i;
}

/**
 * Saves item data to database
 */
static int memitemdata_to_sql(const struct item* p_items, int current_size, int guid, enum inventory_table_type table)
{
	const char* tablename = NULL;
	const char* selectoption = NULL;
	bool has_favorite = false;
	int total_updates = 0, total_deletes = 0, total_inserts = 0;
	int max_size = 0;
	int db_size = 0;

	switch (table) {
	case TABLE_INVENTORY:
		tablename = inventory_db;
		selectoption = "char_id";
		has_favorite = true;
		max_size = MAX_INVENTORY;
		break;
	case TABLE_CART:
		tablename = cart_db;
		selectoption = "char_id";
		max_size = MAX_CART;
		break;
	case TABLE_GUILD_STORAGE:
	case TABLE_STORAGE:
	default:
		ShowError("Invalid table type %d!\n", (int)table);
		Assert_retr(-1, table);
		return -1;
	}
	if (current_size == -1)
		current_size = max_size;

	bool* matched_p = NULL;
	if (current_size > 0) {
		nullpo_ret(p_items);

		matched_p = aCalloc(current_size, sizeof(bool));
	}

	StringBuf buf;
	StrBuf->Init(&buf);

	/**
	 * If the storage table is not empty, check for items and replace or delete where needed.
	 */
	struct item* cp_items = aCalloc(max_size, sizeof(struct item));
	if ((db_size = getitemdata_from_sql(cp_items, max_size, guid, table)) > 0) {
		int* deletes = aCalloc(db_size, sizeof(struct item));

		for (int i = 0; i < db_size; i++) {
			const struct item* cp_it = &cp_items[i];

			int j = 0;
			ARR_FIND(0, current_size, j,
				matched_p[j] != true
				&& p_items[j].nameid != 0
				&& cp_it->nameid == p_items[j].nameid
				&& cp_it->unique_id == p_items[j].unique_id
				&& memcmp(p_items[j].card, cp_it->card, sizeof(int) * MAX_SLOTS) == 0
				&& memcmp(p_items[j].option, cp_it->option, 5 * MAX_ITEM_OPTIONS) == 0);

			if (j < current_size) { // Item found.
				matched_p[j] = true; // Mark the item as matched.

				// If the amount has changed, set for replacement with current item properties.
				if (memcmp(cp_it, &p_items[j], sizeof(struct item)) != 0) {
					if (total_updates == 0) {
						StrBuf->Clear(&buf);
						StrBuf->Printf(&buf, "REPLACE INTO `%s` (`id`, `%s`, `nameid`, `amount`, `equip`, `identify`, `refine`, `grade`, `attribute`", tablename, selectoption);
						for (int k = 0; k < MAX_SLOTS; k++)
							StrBuf->Printf(&buf, ", `card%d`", k);
						for (int k = 0; k < MAX_ITEM_OPTIONS; k++)
							StrBuf->Printf(&buf, ", `opt_idx%d`, `opt_val%d`", k, k);
						StrBuf->AppendStr(&buf, ", `expire_time`, `bound`, `unique_id`");
						if (has_favorite)
							StrBuf->AppendStr(&buf, ", `favorite`");

						StrBuf->AppendStr(&buf, ") VALUES ");

					}

					StrBuf->Printf(&buf, "%s('%d', '%d', '%d', '%d', '%u', '%d', '%d', '%d', '%d'",
						total_updates > 0 ? ", " : "", cp_it->id, guid, p_items[j].nameid, p_items[j].amount, p_items[j].equip, p_items[j].identify, p_items[j].refine, p_items[j].grade, p_items[j].attribute);
					for (int k = 0; k < MAX_SLOTS; k++)
						StrBuf->Printf(&buf, ", '%d'", p_items[j].card[k]);
					for (int k = 0; k < MAX_ITEM_OPTIONS; ++k)
						StrBuf->Printf(&buf, ", '%d', '%d'", p_items[j].option[k].index, p_items[j].option[k].value);
					StrBuf->Printf(&buf, ", '%u', '%d', '%"PRIu64"'", p_items[j].expire_time, p_items[j].bound, p_items[j].unique_id);
					if (has_favorite)
						StrBuf->Printf(&buf, ", %d", p_items[j].favorite);

					StrBuf->AppendStr(&buf, ")");

					total_updates++;
				}
			}
			else { // Doesn't exist in the table, set for deletion.
				deletes[total_deletes++] = cp_it->id;
			}
		}

		if (total_updates > 0 && SQL_ERROR == SQL->QueryStr(map->mysql_handle, StrBuf->Value(&buf)))
			Sql_ShowDebug(map->mysql_handle);

		/**
		 * Handle deletions, if any.
		 */
		if (total_deletes > 0) {
			StrBuf->Clear(&buf);
			StrBuf->Printf(&buf, "DELETE FROM `%s` WHERE `id` IN (", tablename);
			for (int i = 0; i < total_deletes; i++)
				StrBuf->Printf(&buf, "%s'%d'", i == 0 ? "" : ", ", deletes[i]);

			StrBuf->AppendStr(&buf, ");");

			if (SQL_ERROR == SQL->QueryStr(map->mysql_handle, StrBuf->Value(&buf)))
				Sql_ShowDebug(map->mysql_handle);
		}

		aFree(deletes);
	}

	/**
	 * Check for new items and add if required.
	 */
	for (int i = 0; i < current_size; i++) {
		const struct item* p_it = &p_items[i];

		if (matched_p[i] || p_it->nameid == 0)
			continue;

		if (total_inserts == 0) {
			StrBuf->Clear(&buf);
			StrBuf->Printf(&buf, "INSERT INTO `%s` (`%s`, `nameid`, `amount`, `equip`, `identify`, `refine`, `grade`, `attribute`, `expire_time`, `bound`, `unique_id`", tablename, selectoption);
			for (int j = 0; j < MAX_SLOTS; ++j)
				StrBuf->Printf(&buf, ", `card%d`", j);
			for (int j = 0; j < MAX_ITEM_OPTIONS; ++j)
				StrBuf->Printf(&buf, ", `opt_idx%d`, `opt_val%d`", j, j);
			if (has_favorite)
				StrBuf->AppendStr(&buf, ", `favorite`");
			StrBuf->AppendStr(&buf, ") VALUES ");
		}

		StrBuf->Printf(&buf, "%s('%d', '%d', '%d', '%u', '%d', '%d', '%d', '%d', '%u', '%d', '%"PRIu64"'",
			total_inserts > 0 ? ", " : "", guid, p_it->nameid, p_it->amount, p_it->equip, p_it->identify, p_it->refine, p_it->grade,
			p_it->attribute, p_it->expire_time, p_it->bound, p_it->unique_id);

		for (int j = 0; j < MAX_SLOTS; ++j)
			StrBuf->Printf(&buf, ", '%d'", p_it->card[j]);
		for (int j = 0; j < MAX_ITEM_OPTIONS; ++j)
			StrBuf->Printf(&buf, ", '%d', '%d'", p_it->option[j].index, p_it->option[j].value);

		if (has_favorite)
			StrBuf->Printf(&buf, ", '%d'", p_it->favorite);

		StrBuf->AppendStr(&buf, ")");

		total_inserts++;
	}

	if (total_inserts > 0 && SQL_ERROR == SQL->QueryStr(map->mysql_handle, StrBuf->Value(&buf)))
		Sql_ShowDebug(map->mysql_handle);

	StrBuf->Destroy(&buf);

	aFree(cp_items);
	if (matched_p != NULL)
		aFree(matched_p);

	ShowInfo("%s save complete - guid: %d (replace: %d, insert: %d, delete: %d)\n", tablename, guid, total_updates, total_inserts, total_deletes);

	return total_updates + total_inserts + total_deletes;
}

#if defined(AUTOTRADE_PERSISTENCY) && defined(SUPPORT_AT_PERSISTENCY)
/**
 * pc->autotrade_load prehook
 *
 * Replaces default functionality. Loads vending data into a clone.
 */
static void pc_autotrade_load_pre(void)
{
	hookStop(); // Don't execute original pc_autotrade_load

	struct SqlStmt* stmt = SQL->StmtMalloc(map->mysql_handle);
	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	int account_id, char_id;
	char title[MESSAGE_SIZE];
	unsigned char sex;

	if (SQL_ERROR == SQL->StmtPrepare(stmt, "SELECT `account_id`,`char_id`,`sex`,`title` FROM `%s`", map->autotrade_merchants_db)
		|| SQL_ERROR == SQL->StmtExecute(stmt)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_INT, &account_id, sizeof account_id, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 1, SQLDT_INT, &char_id, sizeof char_id, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 2, SQLDT_CHAR, &sex, sizeof sex, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 3, SQLDT_STRING, &title, sizeof title, NULL, NULL)) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}

	while (SQL_SUCCESS == SQL->StmtNextRow(stmt)) {
		struct trade_clone* tc;
		CREATE(tc, struct trade_clone, 1);

		tc->sd.status.account_id = account_id;
		tc->sd.status.char_id = char_id;
		tc->sd.status.sex = sex;
		tc->type = TD_VC;
		safestrncpy(tc->sd.message, title, MESSAGE_SIZE);

		autotrade_clone(tc);
	}

	SQL->StmtFree(stmt);
}

/**
 * Generates a clone from database
 */
static void autotrade_clone(struct trade_clone* tc)
{
	struct map_session_data* sd = &tc->sd;
	struct SqlStmt* stmt;
	int class_;

	ARR_FIND(MOB_CLONE_START, MOB_CLONE_END, class_, mob->db_data[class_] == NULL);
	if (class_ >= MOB_CLONE_END)
		return;

	stmt = SQL->StmtMalloc(map->mysql_handle);
	if (stmt == NULL) {
		SqlStmt_ShowDebug(stmt);
		return;
	}

	struct mob_db* mdb = mob->db_data[class_] = (struct mob_db*)aCalloc(1, sizeof(struct mob_db));
	memset(mdb, 0, sizeof(struct mob_db));
	struct status_data* mstatus = &mdb->status;
	struct view_data* vd = &mdb->vd;
	int16 m, x, y;
	char last_map[MAP_NAME_LENGTH_EXT];
	char sex[2];
	char account_sex[2];
	int group_id;

	mdb->option = 0;
	mstatus->mode = 0;
	mstatus->hp = mstatus->max_hp = 1;

	if (SQL_ERROR == SQL->StmtPrepare(stmt, "SELECT "
		"`c`.`name`,`c`.`base_level`,`c`.`class`,`c`.`zeny`,`c`.`sex`,`c`.`hair`,`c`.`hair_color`,`c`.`clothes_color`,`c`.`body`,"
		"`c`.`weapon`,`c`.`shield`,`c`.`head_top`,`c`.`head_mid`,`c`.`head_bottom`,`c`.`last_map`,`c`.`robe`,`c`.`last_x`,`c`.`last_y`,"
		"`l`.`group_id`, `l`.`sex`"
		"FROM `%s` AS `c` JOIN `%s` AS `l` ON `c`.`account_id` = `l`.`account_id` WHERE `char_id`=? LIMIT 1; ", character_db, login_db)
		|| SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_INT, &sd->status.char_id, sizeof sd->status.char_id)
		|| SQL_ERROR == SQL->StmtExecute(stmt)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_STRING, &mdb->name, sizeof mdb->name, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 1, SQLDT_SHORT, &mdb->lv, sizeof mdb->lv, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 2, SQLDT_INT, &vd->class, sizeof & vd->class, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 3, SQLDT_INT, &sd->status.zeny, sizeof sd->status.zeny, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 4, SQLDT_ENUM, &sex, sizeof sex, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 5, SQLDT_INT, &vd->hair_style, sizeof vd->hair_style, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 6, SQLDT_SHORT, &vd->hair_color, sizeof vd->hair_color, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 7, SQLDT_SHORT, &vd->cloth_color, sizeof vd->cloth_color, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 8, SQLDT_INT, &vd->body_style, sizeof vd->body_style, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 9, SQLDT_INT, &vd->weapon, sizeof vd->weapon, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 10, SQLDT_INT, &vd->shield, sizeof vd->shield, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 11, SQLDT_INT, &vd->head_top, sizeof vd->head_top, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 12, SQLDT_INT, &vd->head_mid, sizeof vd->head_mid, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 13, SQLDT_INT, &vd->head_bottom, sizeof vd->head_bottom, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 14, SQLDT_STRING, &last_map, sizeof last_map, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 15, SQLDT_INT, &vd->robe, sizeof vd->robe, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 16, SQLDT_SHORT, &x, sizeof x, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 17, SQLDT_SHORT, &y, sizeof y, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 18, SQLDT_INT, &group_id, sizeof group_id, NULL, NULL)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 19, SQLDT_ENUM, &account_sex, sizeof account_sex, NULL, NULL)
		) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}
	if (SQL_SUCCESS != SQL->StmtNextRow(stmt))
	{
		ShowError("Requested non-existant character id: %d!\n", sd->status.char_id);
		SQL->StmtFree(stmt);
		return;
	}

	sd->status.sex = vd->sex = char_getgender(sex[0], account_sex[0]);
	pc->set_group(sd, group_id);
	sd->group_id = group_id;
	sd->vender_id = ++vending->next_id;
	sd->buyer_id = buyingstore->getuid();
	sd->state.vending = 1;
	sd->state.autotrade = 1;

	safestrncpy(mdb->sprite, mdb->name, NAME_LENGTH);
	safestrncpy(mdb->jname, mdb->name, NAME_LENGTH);

	m = map->mapname2mapid(last_map);

	// Create monster
	struct mob_data* md = mob->once_spawn_sub(NULL, m, x, y, mdb->name, class_, "", SZ_SMALL, AI_NONE, 0);
	if (md == NULL)
		return;
	md->special_state.clone = 1;
	mob->spawn(md);
	unit->set_dir(&md->bl, UNIT_DIR_SOUTH);

	tc->md = md;
	memcpy(&tc->sd.bl, &md->bl, sizeof(struct block_list));

	addToMOBDATA(md, tc, 0, true);

	int cart_type;
	//int sc_type = SC_PUSH_CART;
	if (SQL_ERROR == SQL->StmtPrepare(stmt, "SELECT `val1`"
		" FROM `%s` WHERE `account_id`=? AND `char_id`=? AND `type`='%d' LIMIT 1", sc_data_db, SC_PUSH_CART)
		|| SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_INT, &sd->status.account_id, sizeof sd->status.account_id)
		|| SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_INT, &sd->status.char_id, sizeof sd->status.char_id)
		|| SQL_ERROR == SQL->StmtExecute(stmt)
		|| SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_INT, &cart_type, sizeof cart_type, NULL, NULL)
		) {
		SqlStmt_ShowDebug(stmt);
		SQL->StmtFree(stmt);
		return;
	}
	if (SQL_SUCCESS == SQL->StmtNextRow(stmt))
		tc->options.pushcart = cart_type;

	if (battle->bc->at_timeout) {
		int timeout = battle->bc->at_timeout * 60000;
		if (SQL_ERROR == SQL->StmtPrepare(stmt, "SELECT `tick`"
			" FROM `%s` WHERE `account_id`=? AND `char_id`=? AND `type`='%d' LIMIT 1", sc_data_db, SC_AUTOTRADE)
			|| SQL_ERROR == SQL->StmtBindParam(stmt, 0, SQLDT_INT, &sd->status.account_id, sizeof sd->status.account_id)
			|| SQL_ERROR == SQL->StmtBindParam(stmt, 1, SQLDT_INT, &sd->status.char_id, sizeof sd->status.char_id)
			|| SQL_ERROR == SQL->StmtExecute(stmt)
			|| SQL_ERROR == SQL->StmtBindColumn(stmt, 0, SQLDT_INT, &timeout, sizeof timeout, NULL, NULL)
			) {
			SqlStmt_ShowDebug(stmt);
			SQL->StmtFree(stmt);
			return;
		}
		SQL->StmtNextRow(stmt);

		int64 tick = timer->gettick();
		tc->options.time = timeout;
		tc->options.quit_timer = timer->add(tick + timeout, timeout_timer, md->bl.id, 1);
		tc->options.save_timer = INVALID_TIMER;
		if (timeout > map->autosave_interval)
			tc->options.save_timer = timer->add_interval(tick + map->autosave_interval, timeout_timer, md->bl.id, 0, map->autosave_interval);
	}

	if (map->list[m].users)
		clif->showvendingboard(&md->bl, sd->message, 0);

	SQL->StmtFree(stmt);

	autotrade_populate(tc);

	// Store for easy access
	// Persistence supports vending only
	idb_put(clone_db, sd->status.char_id, tc);
	idb_put(vender_db, sd->vender_id, tc);
	idb_put(vending->db, sd->status.char_id, tc);
}

/**
 * Fetches cart inventory data and populates vending data
 */
static void autotrade_populate(struct trade_clone* tc)
{
	// General data
	int i = 0;
	struct SqlStmt* stmt = NULL;
	struct map_session_data* sd = &tc->sd;

	// Load cart items
	int result = getitemdata_from_sql(tc->sd.status.cart, MAX_CART, tc->sd.status.char_id, TABLE_CART);
	if (result == 0)
		return;

	// Calc weight & num
	for (i = 0; i < MAX_CART; i++) {
		if (sd->status.cart[i].nameid == 0)
			continue;
		sd->cart_weight += itemdb_weight(sd->status.cart[i].nameid) * sd->status.cart[i].amount;
		sd->cart_num++;
	}

	// Autotrade data
	char* data;
	int count = 0;

	stmt = SQL->StmtMalloc(map->mysql_handle);
	if (SQL_ERROR == SQL->Query(map->mysql_handle, "SELECT `itemkey`,`amount`,`price` FROM `%s` WHERE `char_id` = '%d'", map->autotrade_data_db, tc->sd.status.char_id))
		Sql_ShowDebug(map->mysql_handle);

	while (SQL_SUCCESS == SQL->NextRow(map->mysql_handle)) {
		int itemkey, amount, price;

		SQL->GetData(map->mysql_handle, 0, &data, NULL); itemkey = atoi(data);
		SQL->GetData(map->mysql_handle, 1, &data, NULL); amount = atoi(data);
		SQL->GetData(map->mysql_handle, 2, &data, NULL); price = atoi(data);

		ARR_FIND(0, MAX_CART, i, tc->sd.status.cart[i].id == itemkey);
		if (i != MAX_CART && itemdb_cantrade(&tc->sd.status.cart[i], 0, 0)) {
			if (amount > tc->sd.status.cart[i].amount)
				amount = tc->sd.status.cart[i].amount;

			if (amount) {
				tc->sd.vending[count].index = i;
				tc->sd.vending[count].amount = amount;
				tc->sd.vending[count].value = cap_value(price, 0, (unsigned int)battle->bc->vending_max_value);
				count++;
			}
		}
	}

	tc->sd.vend_num = count;

	SQL->StmtFree(stmt);
}

#endif

/**
 * Gets gender of character
 */
static int char_getgender(char sex, char sex2)
{
	switch (sex) {
	case 'M':
		return SEX_MALE;
	case 'F':
		return SEX_FEMALE;
	case 'U':
	default:
		if (sex2)
			return char_getgender(sex2, 0);
		else
			return 99;
	}
}

/**
 * char->delete_char_sql posthook
 *
 * Sends a packet informing of a char delection to the map server
 * so the map server can delete the related clone if any
 */
static int char_delete_char_sql_post(int result, int char_id)
{
	if (result == 0) {
		uint8 buf[6];
		WBUFW(buf, 0) = CHAR_MAP_PACKET_ID;
		WBUFL(buf, 2) = char_id;
		mapif->send(buf, 6);
	}

	return 0;
}

/**
 * Parses packet informing of a char deletion in the char server.
 * Removes clone if any found.
 */
static void parse_delete_char_packet(int fd)
{
	int char_id = RFIFOL(fd, 2);

	if (char_id)
		remove_clone(char_id);
}

/**
 * Makes trade clones not targeteable to other players
 */
int battle_check_target_post(int retval, struct block_list* src, struct block_list* target, int flag)
{
	if (retval == 1 && target->type == BL_MOB) {
		struct base_data* bd = getFromMOBDATA((struct mob_data*)target, 0);
		if (bd != NULL)
			return -1;
	}
	return retval;
}

/**
 * Autotrade timeout timer.
 *
 * If data = 0, update remaining time and save
 * If data = 1, timer expired and clone must be removed
 */
static int timeout_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct trade_clone* tc = id2tc(id);
	if (tc == NULL)
		return 0;

	if (data == 1)
		tc->options.time = 0;
	else
		tc->options.time -= map->autosave_interval;

	if (tc->options.time <= 0) {
		ShowInfo("[at2] Removing clone: %d\n", tc->sd.status.char_id);
		remove_clone(tc->sd.status.char_id);
	} else {
		save_timeout(tc, false);
	}

	return 1;
}

/**
 * Timed function for exiting the server with a small delay
 * Note: this is necessary as to not freeze the client
 */
static int map_quit_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data* sd = map->id2sd(id);
	if (sd != NULL) {
		map->quit(sd);
	}

	return 0;
}

ACMD(autotrade2) {
	if (map->list[sd->bl.m].flag.autotrade != battle->bc->autotrade_mapflag) {
		clif->message(fd, msg_fd(fd, MSGTBL_AUTOTRADE_NOT_ALLOWED)); // Autotrade is not allowed in this map.
		return false;
	}

	if (pc_isdead(sd)) {
		clif->message(fd, msg_fd(fd, MSGTBL_CANNOT_AUTOTRADE_WHEN_DEAD)); // You cannot autotrade when dead.
		return false;
	}

	if (!sd->state.vending && !sd->state.buyingstore) { //check if player is vending or buying
		clif->message(fd, msg_fd(fd, MSGTBL_AUTOTRADE_MISSING_SHOP)); // "You should have a shop open in order to use @autotrade."
		return false;
	}

#if MAX_AUTOTRADE_PER_ACCOUNT > 0
	int count = 0;
	struct trade_clone* tmp;
	struct DBIterator* iter = db_iterator(clone_db);

	for (tmp = dbi_first(iter); dbi_exists(iter); tmp = dbi_next(iter)) {
		if (tmp->sd.status.account_id == sd->status.account_id)
			count++;
	}
	dbi_destroy(iter);

	if (count >= MAX_AUTOTRADE_PER_ACCOUNT) {
		clif->message(fd, "You have reached the maximum number of allowed autotrade characters.");
		return false;
	}

#endif

	if (battle->bc->at_timeout) {
		int timeout = atoi(message);
		timeout = ((timeout > 0) ? min(timeout, battle->bc->at_timeout) : battle->bc->at_timeout) * 60000;
		status->change_start(NULL, &sd->bl, SC_AUTOTRADE, 10000, 0, 0, 0, 0, timeout, SCFLAG_NONE, 0);
	}

	channel->quit(sd);
	goldpc->stop(sd);

	at_clone_spawn_vending(sd);

	chrif->charselectreq(sd, sockt->session[fd]->client_addr);
	timer->add(timer->gettick() + 500, map_quit_timer, sd->bl.id, 0);

	return false;/* we fail to not cause it to proceed on is_atcommand */
}

HPExport void plugin_init(void) {
	if (SERVER_TYPE == SERVER_TYPE_MAP) {
		clone_db = idb_alloc(DB_OPT_BASE);
		vender_db = idb_alloc(DB_OPT_BASE);
		buyer_db = idb_alloc(DB_OPT_BASE);

		addAtcommand("autotrade", autotrade2);
		addAtcommand("at", autotrade2);

		addHookPre(chrif, save, chrif_save_pre);
		addHookPre(map, quit, map_quit_pre);
		addHookPre(status, damage, status_damage_pre);
		addHookPre(clif, search_store_info_ack, clif_searchstoreinfo_pre);

		addHookPost(clif, authok, clif_authok_post);
		addHookPost(clif, getareachar_unit, clif_getareachar_unit_post);
		addHookPost(battle, check_target, battle_check_target_post);
		addHookPost(map, id2sd, map_id2sd_post);

		addPacket(CHAR_MAP_PACKET_ID, 6, parse_delete_char_packet, hpChrif_Parse);

#if defined(AUTOTRADE_PERSISTENCY) && defined(SUPPORT_AT_PERSISTENCY)
		addHookPre(pc, autotrade_load, pc_autotrade_load_pre);
#endif
	} else if (SERVER_TYPE == SERVER_TYPE_CHAR) {
		addHookPost(chr, delete_char_sql, char_delete_char_sql_post);
	}

}