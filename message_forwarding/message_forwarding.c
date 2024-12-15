//===== Hercules/Heracles Plugin =========================================================================
//= Message forwarding
//===== By: ==============================================================================================
//= csnv
//= Original idea: Mateus(mytails) @ Payon Stories
//===== Version: =========================================================================================
//= 1.0
//===== Description: =====================================================================================
//= Message forwarding for characters in the same account
//= Usage:
//= "@mesfw <name>" Forwards messages directed at given name to current character 
//= "@mesfw <name> stop" Stops forwarding messages for given name
//= "@mesfwall" Redirects messages from all characters in the current account to current character
//= "@mesfwall stop" Stops redirection of all messages
//= "@mesfwall list" Displays a list of characters whose messages are being forwarded to current character
//===== Repository: ======================================================================================
//= https://github.com/csnv/hercules-plugins
//========================================================================================================

#include <stdlib.h>
#include <string.h>

#include "common/hercules.h" /* Should always be the first Hercules file included! (if you don't make it first, you won't be able to use interfaces) */

#include "common/memmgr.h"
#include "common/msgtable.h"
#include "common/socket.h"
#include "common/sql.h"

#include "char/inter.h"
#include "char/mapif.h"

#include "map/atcommand.h"
#include "map/channel.h"
#include "map/chrif.h"
#include "map/clif.h"
#include "map/npc.h"
#include "map/pc.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h" /* should always be the last Hercules file included! (if you don't make it last, it'll intentionally break compile time) */

// Packet number for inter server communication from map to char server
#define CHARNAME_REQUEST_PACKET_ID 0x13
// Packet number for inter server communication from char to map server
#define CHARNAME_RESPONSE_PACKET_ID 0x14

enum responseType {
	MESFW_FAIL = 0x00,
	MESFW_SUCCESS = 0x01,
	MESFW_CAST_NAME = 0x02,
	MESFW_CAST_COUNT = 0x04
};

/* DB for redirecting PM to the required user */
struct DBMap* pm_db;
/* Vector of character names with mesfw enabled related to a character */
struct pm_store {
	VECTOR_DECL(char*) names;
};

bool check_db_pm = false;
struct map_session_data* caller_sd = NULL;
const char *target_name = NULL;

HPExport struct hplugin_info pinfo = {
	"mesfw",    // Plugin name
	SERVER_TYPE_CHAR | SERVER_TYPE_MAP,// Which server types this plugin works with?
	"1.0",       // Plugin version
	HPM_VERSION, // HPM Version (don't change, macro is automatically updated)
};

static struct pm_store* init_pm_store(struct map_session_data* sd);
static bool unforward(int fd, struct map_session_data* sd, const char* char_name);
static bool unforward_all(int fd, struct map_session_data* sd);
static bool forwarding_list(int fd, struct map_session_data* sd);
static void request_charname(int char_id, bool single, const char* name);
static void parse_charname_request(int fd);
static void char_check_charname(int char_id, bool single, const char* charname);
static void response_charname(int char_id, uint16 type, const char* name);
static void parse_charname_response(int fd);
static int map_quit_pre(struct map_session_data** sd);
static void get_all_chars(int char_id);
static void response_count(int char_id, int count);
static int remove_all(struct map_session_data* sd);

/**
 * Single character forwarding command
 * Usage: @mesfw <character_name> <stop?>
 */
ACMD(forwardpm)
{
	char char_name[NAME_LENGTH];
	char cmd[5];

	if (!*message || sscanf(message, "%23s %4[^\n]", char_name, cmd) < 1) {
		clif->message(fd, "[Message forwarding]: Enter a character name (usage: @mesfw <char_name>).");
		return false;
	}

	if (strcmpi(cmd, "stop") == 0) {
		return unforward(fd, sd, char_name);
	}

	struct map_session_data* tsd = strdb_get(pm_db, char_name);
	if (tsd != NULL) {
		if (tsd == sd) {
			char output[CHAT_SIZE_MAX];
			sprintf(output, "[Message forwarding]: Messages for (%s) are already being forwarded. Use \"@mesfw %s stop\" to stop forwarding messages.", char_name, char_name);
			clif->message(sd->fd, output);
		} else {
			clif->message(sd->fd, "[Message forwarding]: Invalid character name");
		}

		return false;
	}

	request_charname(sd->status.char_id, true, char_name);
	return true;
}

/**
 * All related characters forwarding command
 * Usage: @mesfw <stop | list?>
 */
ACMD(forwardpmall)
{
	char cmd[5];

	if (*message && sscanf(message, "%4[^\n]", cmd) == 1) {

		if (strcmpi(cmd, "stop") == 0) {
			return unforward_all(fd, sd);
		}

		if (strcmpi(cmd, "list") == 0) {
			return forwarding_list(fd, sd);
		}
	}

	request_charname(sd->status.char_id, false, NULL);
	return true;
}

/**
 * Stops forwarding messages from a given charname to current character
 */
static bool unforward(int fd, struct map_session_data *sd, const char *char_name)
{
	struct pm_store* data = getFromMSD(sd, 0);
	if (!data) {
		clif->message(fd, "[Message forwarding]: No character found in your forwarding settings.");
		return false;
	}

	int i = 0;
	char output[128];

	ARR_FIND(0, VECTOR_LENGTH(data->names), i, strcmp(VECTOR_INDEX(data->names, i), char_name) == 0);

	if (i == VECTOR_LENGTH(data->names)) {
		sprintf(output, "[Message forwarding]: Character (%s) not found in your forwarding settings.", char_name);
		clif->message(sd->fd, output);
		return false;
	}

	char* target = VECTOR_INDEX(data->names, i);
	sprintf(output, "[Message forwarding]: Messages for character (%s) will not be forwarded to you anymore.", char_name);
	clif->message(sd->fd, output);
	strdb_remove(pm_db, char_name);
	VECTOR_ERASE(data->names, i);
	aFree(target);

	return true;
}

/**
 * Stops forwarding messages from all related characters
 */
static bool unforward_all(int fd, struct map_session_data* sd)
{
	int removed = remove_all(sd);

	if (removed == 0) {
		clif->message(fd, "[Message forwarding]: No character found in your forwarding settings.");
		return false;
	}

	char output[128];
	sprintf(output, "[Message forwarding]: Removed %d characters from your message forwarding settings.", removed);
	clif->message(sd->fd, output);

	return true;
}

/**
 * Displays the current list of characters whose messages are being forwarded.
 */
static bool forwarding_list(int fd, struct map_session_data* sd)
{
	struct pm_store* data = getFromMSD(sd, 0);
	if (!data) {
		clif->message(fd, "[Message forwarding]: No characters found.");
		return false;
	}

	char output[CHAT_SIZE_MAX];
	int length = VECTOR_LENGTH(data->names);
	char* char_name = NULL;

	snprintf(output, sizeof(output), "[Message forwarding]: Currently forwarding messages from %d characters:", length);
	clif->message(fd, output);

	for (int i = 0; i < length; i++) {
		char_name = VECTOR_INDEX(data->names, i);
		snprintf(output, sizeof(output), "- %s", char_name);
		clif->message(fd, output);
	}
	return true;
}

/**
 * Map requests char server to find the desired char name or to gather all related characters
 */
static void request_charname(int char_id, bool single, const char *name)
{
	int fd = chrif->fd;

	WFIFOHEAD(fd, 10);
	WFIFOW(fd, 0) = CHARNAME_REQUEST_PACKET_ID;
	WFIFOB(fd, 2) = single;
	WFIFOL(fd, 3) = char_id;
	if (single)
		safestrncpy(WFIFOP(fd, 7), name, NAME_LENGTH);
	WFIFOSET(fd, 7 + NAME_LENGTH);
}

/**
 * Char server processes request from map server
 * The map server wants to:
 * - Check if the given name is valid (single = true)
 * - Gather all character names from current account (single = false)
 */
static void parse_charname_request(int fd)
{
	bool single = RFIFOB(fd, 2);
	int char_id = RFIFOL(fd, 3);

	if (single) {
		char charname[NAME_LENGTH];
		memcpy(charname, RFIFOP(fd, 7), NAME_LENGTH);
		char_check_charname(char_id, single, charname);
	} else {
		get_all_chars(char_id);
	}
}

/**
 * Checks if the given name is related to current character's account
 * NOTE: Customize this SQL syntax if you use a Master Account System
 */
static void char_check_charname(int char_id, bool single, const char* charname)
{
	char esc_name[NAME_LENGTH];
	SQL->EscapeStringLen(inter->sql_handle, esc_name, charname, strnlen(charname, NAME_LENGTH));

	if (SQL_ERROR == SQL->Query(inter->sql_handle,
		"SELECT 1 FROM `char` c1 JOIN `char` c2 ON c1.account_id = c2.account_id WHERE c2.char_id = %d AND c1.name = '%s' AND c1.char_id != %d LIMIT 1",
		char_id, esc_name, char_id))
	{
		Sql_ShowDebug(inter->sql_handle);
		return;
	}

	if (SQL->NumRows(inter->sql_handle) == 0) {
		response_charname(char_id, MESFW_FAIL, NULL);
		return;
	}

	response_charname(char_id, MESFW_SUCCESS | MESFW_CAST_NAME, esc_name);
}

/**
 * Gathers names from all characters related to querying character's account
 * NOTE: Customize this SQL syntax if you use a Master Account System
 */
static void get_all_chars(int char_id)
{
	if (SQL_ERROR == SQL->Query(inter->sql_handle,
		"SELECT c1.`name` FROM `char` c1 JOIN `char` c2 ON c1.account_id = c2.account_id WHERE c2.char_id = %d AND c1.char_id != %d",
		char_id, char_id))
	{
		Sql_ShowDebug(inter->sql_handle);
		return;
	}

	if (SQL->NumRows(inter->sql_handle) > 0) {
		int count = 0;
		while (SQL_SUCCESS == SQL->NextRow(inter->sql_handle)) {
			char *name;
			SQL->GetData(inter->sql_handle, 0, &name, NULL);
			response_charname(char_id, MESFW_SUCCESS, name);
			count++;
		}
		response_count(char_id, count);
	}


	SQL->FreeResult(inter->sql_handle);
}

/**
 * Char server tells map server how many characters found in current account
 */
static void response_count(int char_id, int count)
{
	uint8 buf[8 + NAME_LENGTH];
	WBUFW(buf, 0) = CHARNAME_RESPONSE_PACKET_ID;
	WBUFW(buf, 2) = MESFW_CAST_COUNT;
	WBUFL(buf, 4) = char_id;
	WBUFL(buf, 8) = count;
	mapif->send(buf, 8 + NAME_LENGTH);
}

/**
 * Char server tells map server how many characters found in current account
 */
static void response_charname(int char_id, uint16 type, const char* name)
{
	uint8 buf[8 + NAME_LENGTH];
	WBUFW(buf, 0) = CHARNAME_RESPONSE_PACKET_ID;
	WBUFW(buf, 2) = type;
	WBUFL(buf, 4) = char_id;
	memcpy(WBUFP(buf, 8), name, NAME_LENGTH);
	mapif->send(buf, 8 + NAME_LENGTH);
}

/**
 * Map server processes response from char server
 * type & MESFW_FAIL: Charname not found or other error
 * type & MESFW_SUCCESS: Not checked directly. Used to NOT display any message
 * type & MESFW_CAST_COUNT: Display number of characters found
 * type & MESFW_CAST_NAME: Display requested character name has been added to message forwarding 
 */
static void parse_charname_response(int fd)
{
	uint16 type = RFIFOW(fd, 2);
	int char_id = RFIFOL(fd, 4);
	struct map_session_data* sd = map->charid2sd(char_id);

	if (sd == NULL)
		return;

	if (type == MESFW_FAIL) {
		clif->message(sd->fd, "[Message forwarding]: Character name not found. Make sure it is spelled correctly and belongs to your account.");
		return;
	}

	if (type & MESFW_CAST_COUNT) {
		int count = RFIFOL(fd, 8);
		char output[128];
		sprintf(output, "[Message forwarding]: Messages sent to %d characters in your account will be forwarded to you.", count);
		clif->message(sd->fd, output);
		return;
	}


	char* name = aStrdup(RFIFOP(fd, 8));
	struct pm_store* store = init_pm_store(sd);

	// Check name is not already being forwarded
	// This method should be faster than directly querying the db
	int i = 0;
	ARR_FIND(0, VECTOR_LENGTH(store->names), i, strcmp(VECTOR_INDEX(store->names, i), name) == 0);

	if (i != VECTOR_LENGTH(store->names)) // Found
		return;

	// Pushes name into db and associate it with player
	VECTOR_PUSH(store->names, name);
	strdb_put(pm_db, name, sd);

	if (type & MESFW_CAST_NAME) {
		char output[128];
		sprintf(output, "[Message forwarding]: Messages sent to (%s) will be forwarded to you.", name);
		clif->message(sd->fd, output);
	}
}

/**
 * Initializes vector for storing all character names that are being "supplanted".
 * This vector is linked to character session.
 */
static struct pm_store* init_pm_store(struct map_session_data* sd)
{
	struct pm_store *data = getFromMSD(sd, 0);

	if (data == NULL) {
		CREATE(data, struct pm_store, 1);
		VECTOR_INIT(data->names);
		addToMSD(sd, data, 0, true);
	}

	VECTOR_ENSURE(data->names, 1, 1);

	return data;
}

/**
 * map->quit pre-hook
 * Removes all character names from database
 */
static int map_quit_pre(struct map_session_data** sd)
{
	if (sd == NULL)
		return 0;

	remove_all(*sd);
	return 0;
}

/**
 * Removes all character names from database and clears vector
 */
static int remove_all(struct map_session_data* sd)
{
	struct pm_store* data = getFromMSD(sd, 0);

	if (!data)
		return 0;

	int length = VECTOR_LENGTH(data->names);

	if (length == 0)
		return 0;

	for (int i = 0; i < length; i++) {
		char* name = VECTOR_INDEX(data->names, i);
		strdb_remove(pm_db, name);
		aFree(name);
	}
	VECTOR_CLEAR(data->names);

	return length;
}

///
// THIS IS HORRENDOUS!
// But the original function does not leave an alternative other than copy-pasting the entire function, so this is how it works:
// - Set flag to check pm_db in custom nick2sd
// - Save sender SD for later check
// - If whisper fails "normally" try to get a target from PM_DB
// - Use target_name for holding the target name and as a flag that a forwarding event happens
///
/**
* 
 * clif_parse_wisMessage pre hook
 * 
 * Sets flag for nick2sd
 * Saves caller sd
 * Removes target, just in case
 */
static void pwis_message_pre(int *p_fd, struct map_session_data** p_sd)
{
	struct map_session_data* sd = *p_sd;
	check_db_pm = true;
	caller_sd = sd;
	target_name = NULL;
}

/**
 * map_nick2sd post hook
 * 
 * Recovers SD from PM_DB if no SD has been found and check_db_pm flag is true
 */
static struct map_session_data* nick2sd_post(struct map_session_data* retVal, const char* nick, bool allow_partial)
{
	if (retVal)
		return retVal;

	if (check_db_pm && (retVal = strdb_get(pm_db, nick)) && caller_sd != retVal) {
		target_name = retVal->status.name;
	} else {
		target_name = NULL;
		retVal = NULL;
	}

	return retVal;
}

/**
 * clif_wis_end post hook
 * 
 * Called several times in clif_parse_wisMessage
 *
 * Sends a confirmation message that the private message is being redirected, only when target_name has value
 * as it means a message-forwarding target SD was found
 */
static void wis_end_post(int fd, int flag)
{
	if (flag == 0 && target_name) {
		// Forwarded to another character
		char output[256];
		sprintf(output, "[Message forwarding]: Your message has been forwarded to (%s)", target_name);
		clif->messagecolor_self(fd, COLOR_YELLOW, output);
	}
}

/**
 * clif_parse_wis_message post hook
 * 
 * Message process finished in any form or way, just reset auxiliary data
 */
static void pwis_message_post(int fd, struct map_session_data* sd)
{
	check_db_pm = false;
	target_name = NULL;
	caller_sd = NULL;
}

/* run when server starts */
HPExport void plugin_init (void) {

	if (SERVER_TYPE == SERVER_TYPE_MAP) {
		pm_db = strdb_alloc(DB_OPT_BASE, NAME_LENGTH);

		addAtcommand("mesfw", forwardpm);
		addAtcommand("mesfwall", forwardpmall);

		addHookPre(clif, pWisMessage, pwis_message_pre);
		addHookPre(map, quit, map_quit_pre);

		addHookPost(map, nick2sd, nick2sd_post);
		addHookPost(clif, pWisMessage, pwis_message_post);
		addHookPost(clif, wis_end, wis_end_post);

		addPacket(CHARNAME_RESPONSE_PACKET_ID, 8 + NAME_LENGTH, parse_charname_response, hpChrif_Parse);
	} else if (SERVER_TYPE == SERVER_TYPE_CHAR) {
		addPacket(CHARNAME_REQUEST_PACKET_ID, 7 + NAME_LENGTH, parse_charname_request, hpParse_FromMap);
	}
}
