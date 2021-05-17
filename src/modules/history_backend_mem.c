/* src/modules/history_backend_mem.c - History Backend: memory
 * (C) Copyright 2019-2021 Bram Matthys (Syzop) and the UnrealIRCd team
 * License: GPLv2
 */
#include "unrealircd.h"

/* This is the memory type backend. It is optimized for speed.
 * For example, per-channel, it caches the field "number of lines"
 * and "oldest record", so frequent cleaning operations such as
 * "delete any record older than time T" or "keep only N lines"
 * are executed as fast as possible.
 */

ModuleHeader MOD_HEADER
= {
	"history_backend_mem",
	"2.0",
	"History backend: memory",
	"UnrealIRCd Team",
	"unrealircd-5",
};

/* Defines */
#define OBJECTLEN	((NICKLEN > CHANNELLEN) ? NICKLEN : CHANNELLEN)
#define HISTORY_BACKEND_MEM_HASH_TABLE_SIZE 1019

/* The regular history cleaning (by timer) is spread out
 * a bit, rather than doing ALL channels every T time.
 * HISTORY_SPREAD: how much to spread the "cleaning", eg 1 would be
 *  to clean everything in 1 go, 2 would mean the first event would
 *  clean half of the channels, and the 2nd event would clean the rest.
 *  Obviously more = better to spread the load, but doing a reasonable
 *  amount of work is also benefitial for performance (think: CPU cache).
 * HISTORY_MAX_OFF_SECS: how many seconds may the history be 'off',
 *  that is: how much may we store the history longer than required.
 * The other 2 macros are calculated based on that target.
 *
 * Update April 2021: these values are now also used for saving the
 * history if the persistent option is enabled. Therefore changed the
 * values to spread it even more out: from 16/128 to 60/300 so
 * in case of persistent it will save every 5 minutes.
 */
#ifdef DEBUGMODE
#define HISTORY_CLEAN_PER_LOOP HISTORY_BACKEND_MEM_HASH_TABLE_SIZE
#define HISTORY_TIMER_EVERY 5
#else
#define HISTORY_SPREAD	60
#define HISTORY_MAX_OFF_SECS	300
#define HISTORY_CLEAN_PER_LOOP	(HISTORY_BACKEND_MEM_HASH_TABLE_SIZE/HISTORY_SPREAD)
#define HISTORY_TIMER_EVERY	(HISTORY_MAX_OFF_SECS/HISTORY_SPREAD)
#endif

/* Some magic numbers used in the database format */
#define HISTORYDB_MAGIC_FILE_START	0xFEFEFEFE
#define HISTORYDB_MAGIC_FILE_END	0xEFEFEFEF
#define HISTORYDB_MAGIC_ENTRY_START	0xFFFFFFFF
#define HISTORYDB_MAGIC_ENTRY_END	0xEEEEEEEE

/* Definitions (structs, etc.) -- all for persistent history */
struct cfgstruct {
	int persist;
	char *directory;
	char *masterdb; /* Autogenerated for convenience, not a real config item */
	char *db_secret;
	char *prehash;
	char *posthash;
};

typedef struct HistoryLogObject HistoryLogObject;
struct HistoryLogObject {
	HistoryLogObject *prev, *next;
	HistoryLogLine *head; /**< Start of the log (the earliest entry) */
	HistoryLogLine *tail; /**< End of the log (the latest entry) */
	int num_lines; /**< Number of lines of log */
	time_t oldest_t; /**< Oldest time in log */
	int max_lines; /**< Maximum number of lines permitted */
	long max_time; /**< Maximum number of seconds to retain history */
	int dirty; /**< Dirty flag, used for disk writing */
	char name[OBJECTLEN+1];
};

/* Global variables */
struct cfgstruct cfg;
struct cfgstruct test;
static char siphashkey_history_backend_mem[SIPHASH_KEY_LENGTH];
HistoryLogObject *history_hash_table[HISTORY_BACKEND_MEM_HASH_TABLE_SIZE];
static int already_loaded = 0;

/* Forward declarations */
int hbm_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int hbm_config_posttest(int *errs);
int hbm_config_run(ConfigFile *cf, ConfigEntry *ce, int type);
int hbm_rehash(void);
int hbm_rehash_complete(void);
static void setcfg(struct cfgstruct *cfg);
static void freecfg(struct cfgstruct *cfg);
static void init_history_storage(ModuleInfo *modinfo);
int hbm_modechar_del(Channel *channel, int modechar);
int hbm_history_add(char *object, MessageTag *mtags, char *line);
int hbm_history_cleanup(HistoryLogObject *h);
HistoryResult *hbm_history_request(char *object, HistoryFilter *filter);
int hbm_history_destroy(char *object);
int hbm_history_set_limit(char *object, int max_lines, long max_time);
EVENT(history_mem_clean);
EVENT(history_mem_init);
static int hbm_read_masterdb(void);
static void hbm_read_dbs(void);
static int hbm_read_db(char *fname);
static int hbm_write_masterdb(void);
static int hbm_write_db(HistoryLogObject *h);
static void hbm_delete_db(HistoryLogObject *h);

MOD_TEST()
{
	memset(&cfg, 0, sizeof(cfg));
	memset(&test, 0, sizeof(test));
	setcfg(&test);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, hbm_config_test);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGPOSTTEST, 0, hbm_config_posttest);

	return MOD_SUCCESS;
}

MOD_INIT()
{
	HistoryBackendInfo hbi;

	MARK_AS_OFFICIAL_MODULE(modinfo);
	ModuleSetOptions(modinfo->handle, MOD_OPT_PERM, 1);

	setcfg(&cfg);

	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, hbm_config_run);
	HookAdd(modinfo->handle, HOOKTYPE_MODECHAR_DEL, 0, hbm_modechar_del);
	HookAdd(modinfo->handle, HOOKTYPE_REHASH, 0, hbm_rehash);
	HookAdd(modinfo->handle, HOOKTYPE_REHASH_COMPLETE, 0, hbm_rehash_complete);


	memset(&history_hash_table, 0, sizeof(history_hash_table));
	siphash_generate_key(siphashkey_history_backend_mem);

	memset(&hbi, 0, sizeof(hbi));
	hbi.name = "mem";
	hbi.history_add = hbm_history_add;
	hbi.history_request = hbm_history_request;
	hbi.history_destroy = hbm_history_destroy;
	hbi.history_set_limit = hbm_history_set_limit;
	if (!HistoryBackendAdd(modinfo->handle, &hbi))
		return MOD_FAILED;

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	EventAdd(modinfo->handle, "history_mem_init", history_mem_init, NULL, 1, 1);
	EventAdd(modinfo->handle, "history_mem_clean", history_mem_clean, NULL, HISTORY_TIMER_EVERY*1000, 0);
	init_history_storage(modinfo);
	return MOD_SUCCESS;
}

/* Read the .db if 'persist' mode is enabled.
 * Normally this would be in MOD_LOAD, but the load order always
 * must be: channeldb first, this module second, and since we
 * cannot influence the load order we do this silly trick
 * with a one-time 1msec event.
 */
EVENT(history_mem_init)
{
	if (!already_loaded)
	{
		/* Initial boot / load of the module... */
		already_loaded = 1;
		if (cfg.persist)
			hbm_read_dbs();
	}
}

MOD_UNLOAD()
{
	freecfg(&test);
	freecfg(&cfg);
	return MOD_SUCCESS;
}

/** Set cfg->masterdb based on cfg->directory, for convenience */
static void hbm_set_masterdb_filename(struct cfgstruct *cfg)
{
	char buf[512];

	safe_free(cfg->masterdb);
	if (cfg->directory)
	{
		snprintf(buf, sizeof(buf), "%s/master.db", cfg->directory);
		safe_strdup(cfg->masterdb, buf);
	}
}

/** Default configuration for set::history::channel */
static void setcfg(struct cfgstruct *cfg)
{
	safe_strdup(cfg->directory, "history");
	convert_to_absolute_path(&cfg->directory, PERMDATADIR);
	hbm_set_masterdb_filename(cfg);
}

static void freecfg(struct cfgstruct *cfg)
{
	safe_free(cfg->directory);
	safe_free(cfg->db_secret);
	safe_free(cfg->prehash);
	safe_free(cfg->posthash);
}


/** Test the set::history::channel configuration */
int hbm_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;

	if ((type != CONFIG_SET_HISTORY_CHANNEL) || !ce || !ce->ce_varname)
		return 0;

	if (!strcmp(ce->ce_varname, "persist"))
	{
		if (!ce->ce_vardata)
		{
			config_error("%s:%i: missing parameter",
				ce->ce_fileptr->cf_filename, ce->ce_varlinenum);
			errors++;
		} else {
			test.persist = config_checkval(ce->ce_vardata, CFG_YESNO);
		}
	} else
	if (!strcmp(ce->ce_varname, "db-secret"))
	{
		char *err;
		if ((err = unrealdb_test_secret(ce->ce_vardata)))
		{
			config_error("%s:%i: set::history::channel::db-secret: %s", ce->ce_fileptr->cf_filename, ce->ce_varlinenum, err);
			errors++;
		}
		safe_strdup(test.db_secret, ce->ce_vardata);
	} else
	if (!strcmp(ce->ce_varname, "directory")) // or "path" ?
	{
		if (!ce->ce_vardata)
		{
			config_error("%s:%i: missing parameter",
				ce->ce_fileptr->cf_filename, ce->ce_varlinenum);
			errors++;
		} else
		{
			safe_strdup(test.directory, ce->ce_vardata);
			hbm_set_masterdb_filename(&test);
		}
	} else
	{
		return 0; /* unknown option to us, let another module handle it */
	}

	*errs = errors;
	return errors ? -1 : 1;
}

/** Post-configuration test on set::history::channel */
int hbm_config_posttest(int *errs)
{
	int errors = 0;

	if (test.db_secret && !test.persist)
	{
		config_error("set::history::channel::db-secret is set but set::history::channel::persist is disabled, this makes no sense. "
			     "Either use 'persist yes' or comment out / delete 'db-secret'.");
		errors++;
	} else
	if (!test.db_secret && test.persist)
	{
		config_error("set::history::channel::db-secret needs to be set."); // TODO: REFER TO FAQ OR OTHER ENTRY!!!!
		errors++;
	} else
	if (test.db_secret && test.persist)
	{
		/* Configuration is good, now check if the password is correct
		 * (if we can check at all, that is)...
		 */
		char *errstr = NULL;
		if (test.masterdb && ((errstr = unrealdb_test_db(test.masterdb, test.db_secret))))
		{
			config_error("[history] %s", errstr);
			errors++;
			goto hbm_config_posttest_end;
		}

		/* Ensure directory exists and is writable */
#ifdef _WIN32
		(void)mkdir(test.directory); /* (errors ignored) */
#else
		(void)mkdir(test.directory, S_IRUSR|S_IWUSR|S_IXUSR); /* (errors ignored) */
#endif
		if (!file_exists(test.directory))
		{
			config_error("[history] Directory %s does not exist and could not be created",
				test.directory);
			errors++;
		} else
		{
			/* Only do this if directory actually exists, hence in the 'else' block */
			if (!hbm_read_masterdb())
				errors++;
		}
	}

hbm_config_posttest_end:
	freecfg(&test);
	setcfg(&test);
	*errs = errors;
	return errors ? -1 : 1;
}

/** Configure ourselves based on the set::history::channel settings */
int hbm_config_run(ConfigFile *cf, ConfigEntry *ce, int type)
{
	if ((type != CONFIG_SET_HISTORY_CHANNEL) || !ce || !ce->ce_varname)
		return 0;

	if (!strcmp(ce->ce_varname, "persist"))
	{
		cfg.persist = config_checkval(ce->ce_vardata, CFG_YESNO);
	} else
	if (!strcmp(ce->ce_varname, "directory")) // or "path" ?
	{
		safe_strdup(cfg.directory, ce->ce_vardata);
		convert_to_absolute_path(&cfg.directory, PERMDATADIR);
		hbm_set_masterdb_filename(&cfg);
	} else
	if (!strcmp(ce->ce_varname, "db-secret"))
	{
		safe_strdup(cfg.db_secret, ce->ce_vardata);
	} else
	{
		return 0; /* unknown option to us, let another module handle it */
	}

	return 1; /* handled by us */
}

int hbm_rehash(void)
{
	freecfg(&cfg);
	setcfg(&cfg);
	return 0;
}

int hbm_rehash_complete(void)
{
	return 0;
}

char *history_storage_capability_parameter(Client *client)
{
	static char buf[128];

	if (cfg.persist)
		strlcpy(buf, "memory,disk=encrypted", sizeof(buf));
	else
		strlcpy(buf, "memory", sizeof(buf));

	return buf;
}

static void init_history_storage(ModuleInfo *modinfo)
{
	ClientCapabilityInfo cap;

	memset(&cap, 0, sizeof(cap));
	cap.name = "unrealircd.org/history-storage";
	cap.flags = CLICAP_FLAGS_ADVERTISE_ONLY;
	cap.parameter = history_storage_capability_parameter;
	ClientCapabilityAdd(modinfo->handle, &cap, NULL);
}

uint64_t hbm_hash(char *object)
{
	return siphash_nocase(object, siphashkey_history_backend_mem) % HISTORY_BACKEND_MEM_HASH_TABLE_SIZE;
}

HistoryLogObject *hbm_find_object(char *object)
{
	int hashv = hbm_hash(object);
	HistoryLogObject *h;

	for (h = history_hash_table[hashv]; h; h = h->next)
	{
		if (!strcasecmp(object, h->name))
			return h;
	}
	return NULL;
}

HistoryLogObject *hbm_find_or_add_object(char *object)
{
	int hashv = hbm_hash(object);
	HistoryLogObject *h;

	for (h = history_hash_table[hashv]; h; h = h->next)
	{
		if (!strcasecmp(object, h->name))
			return h;
	}
	/* Create new one */
	h = safe_alloc(sizeof(HistoryLogObject));
	strlcpy(h->name, object, sizeof(h->name));
	AddListItem(h, history_hash_table[hashv]);
	return h;
}

void hbm_delete_object_hlo(HistoryLogObject *h)
{
	int hashv;

	if (cfg.persist)
		hbm_delete_db(h);

	hashv = hbm_hash(h->name);
	DelListItem(h, history_hash_table[hashv]);
	safe_free(h);
}

int hbm_modechar_del(Channel *channel, int modechar)
{
	HistoryLogObject *h;

	if (!cfg.persist)
		return 0;

	if ((modechar == 'P') && ((h = hbm_find_object(channel->chname))))
	{
		/* Channel went from +P to -P and also has channel history: delete the history file */
		hbm_delete_db(h);

		h->dirty = 1;
		/* The reason for marking the entry as 'dirty' is that someone may later
		 * set the channel +P again. If we would not set the h->dirty=1 then this
		 * would mean the history log would not get rewritten until someone speaks.
		 */
	}

	return 0;
}

void hbm_duplicate_mtags(HistoryLogLine *l, MessageTag *m)
{
	MessageTag *n;

	/* Duplicate all message tags */
	for (; m; m = m->next)
	{
		n = duplicate_mtag(m);
		AppendListItem(n, l->mtags);
	}
	n = find_mtag(l->mtags, "time");
	if (!n)
	{
		/* This is duplicate code from src/modules/server-time.c
		 * which seems silly.
		 */
		struct timeval t;
		struct tm *tm;
		time_t sec;
		char buf[64];

		gettimeofday(&t, NULL);
		sec = t.tv_sec;
		tm = gmtime(&sec);
		snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
			tm->tm_year + 1900,
			tm->tm_mon + 1,
			tm->tm_mday,
			tm->tm_hour,
			tm->tm_min,
			tm->tm_sec,
			(int)(t.tv_usec / 1000));

		n = safe_alloc(sizeof(MessageTag));
		safe_strdup(n->name, "time");
		safe_strdup(n->value, buf);
		AddListItem(n, l->mtags);
	}
	/* Now convert the "time" message tag to something we can use in l->t */
	l->t = server_time_to_unix_time(n->value);
}

/** Add a line to a history object */
void hbm_history_add_line(HistoryLogObject *h, MessageTag *mtags, char *line)
{
	HistoryLogLine *l = safe_alloc(sizeof(HistoryLogLine) + strlen(line));
	strcpy(l->line, line); /* safe, see memory allocation above ^ */
	hbm_duplicate_mtags(l, mtags);
	if (h->tail)
	{
		/* append to tail */
		h->tail->next = l;
		l->prev = h->tail;
		h->tail = l;
	} else {
		/* no tail, no head */
		h->head = h->tail = l;
	}
	h->dirty = 1;
	h->num_lines++;
	if ((l->t < h->oldest_t) || (h->oldest_t == 0))
		h->oldest_t = l->t;
}

/** Delete a line from a history object */
void hbm_history_del_line(HistoryLogObject *h, HistoryLogLine *l)
{
	if (l->prev)
		l->prev->next = l->next;
	if (l->next)
		l->next->prev = l->prev;
	if (h->head == l)
	{
		/* New head */
		h->head = l->next;
	}
	if (h->tail == l)
	{
		/* New tail */
		h->tail = l->prev; /* could be NULL now */
	}

	free_message_tags(l->mtags);
	safe_free(l);

	h->dirty = 1;
	h->num_lines--;

	/* IMPORTANT: updating h->oldest_t takes place at the caller
	 * because it is in a better position to optimize the process
	 */
}

/** Add history entry */
int hbm_history_add(char *object, MessageTag *mtags, char *line)
{
	HistoryLogObject *h = hbm_find_or_add_object(object);
	if (!h->max_lines)
	{
		sendto_realops("hbm_history_add() for '%s', which has no limit", h->name);
#ifdef DEBUGMODE
		abort();
#else
		h->max_lines = 50;
		h->max_time = 86400;
#endif
	}
	if (h->num_lines >= h->max_lines)
	{
		/* Delete previous line */
		hbm_history_del_line(h, h->head);
	}
	hbm_history_add_line(h, mtags, line);
	return 0;
}

HistoryLogLine *duplicate_log_line(HistoryLogLine *l)
{
	HistoryLogLine *n = safe_alloc(sizeof(HistoryLogLine) + strlen(l->line));
	strcpy(n->line, l->line); /* safe, see memory allocation above ^ */
	hbm_duplicate_mtags(n, l->mtags);
	return n;
}

HistoryResult *hbm_history_request(char *object, HistoryFilter *filter)
{
	HistoryResult *r;
	HistoryLogObject *h = hbm_find_object(object);
	HistoryLogLine *l;
	long redline; /* Imaginary timestamp. Before the red line, history is too old. */
	int lines_sendable = 0, lines_to_skip = 0, cnt = 0;

	if (!h)
		return NULL; /* nothing found */

	r = safe_alloc(sizeof(HistoryResult));
	safe_strdup(r->object, object);

	/* Decide on red line, under this the history is too old.
	 * Filter can be more strict than history object (but not the other way around):
	 */
	if (filter && filter->last_seconds && (filter->last_seconds < h->max_time))
		redline = TStime() - filter->last_seconds;
	else
		redline = TStime() - h->max_time;

	/* Once the filter API expands, the following will change too.
	 * For now, this is sufficient, since requests are only about lines:
	 */
	lines_sendable = 0;
	for (l = h->head; l; l = l->next)
		if (l->t >= redline)
			lines_sendable++;
	if (filter && (lines_sendable > filter->last_lines))
		lines_to_skip = lines_sendable - filter->last_lines;

	for (l = h->head; l; l = l->next)
	{
		/* Make sure we don't send too old entries:
		 * We only have to check for time here, as line count is already
		 * taken into account in hbm_history_add.
		 */
		if (l->t >= redline && (++cnt > lines_to_skip))
		{
			/* Add to result */
			HistoryLogLine *n = duplicate_log_line(l);
			if (!r->log)
			{
				/* First item */
				r->log = r->log_tail = n;
			} else
			{
				/* Quick append to tail */
				r->log_tail->next = n;
				n->prev = r->log_tail;
				r->log_tail = n; /* we are the new tail */
			}
		}
	}

	return r;
}

/** Clean up expired entries */
int hbm_history_cleanup(HistoryLogObject *h)
{
	HistoryLogLine *l, *l_next = NULL;
	long redline = TStime() - h->max_time;

	/* First enforce 'h->max_time', after that enforce 'h->max_lines' */

	/* Checking for time */
	if (h->oldest_t < redline)
	{
		h->oldest_t = 0; /* recalculate in next loop */

		for (l = h->head; l; l = l_next)
		{
			l_next = l->next;
			if (l->t < redline)
			{
				hbm_history_del_line(h, l); /* too old, delete it */
				continue;
			}
			if ((h->oldest_t == 0) || (l->t < h->oldest_t))
				h->oldest_t = l->t;
		}
	}

	if (h->num_lines > h->max_lines)
	{
		h->oldest_t = 0; /* recalculate in next loop */

		for (l = h->head; l; l = l_next)
		{
			l_next = l->next;
			if (h->num_lines > h->max_lines)
			{
				hbm_history_del_line(h, l);
				continue;
			}
			if ((h->oldest_t == 0) || (l->t < h->oldest_t))
				h->oldest_t = l->t;
		}
	}

	return 1;
}

int hbm_history_destroy(char *object)
{
	HistoryLogObject *h = hbm_find_object(object);
	HistoryLogLine *l, *l_next;

	if (!h)
		return 0;

	for (l = h->head; l; l = l_next)
	{
		l_next = l->next;
		/* We could use hbm_history_del_line() here but
		 * it does unnecessary work, this is quicker.
		 * The only danger is that we may forget to free some
		 * fields that are added later there but not here.
		 */
		free_message_tags(l->mtags);
		safe_free(l);
	}

	hbm_delete_object_hlo(h);
	return 1;
}

/** Set new limit on history object */
int hbm_history_set_limit(char *object, int max_lines, long max_time)
{
	HistoryLogObject *h = hbm_find_or_add_object(object);
	h->max_lines = max_lines;
	h->max_time = max_time;
	hbm_history_cleanup(h); /* impose new restrictions */
	return 1;
}

/** Read the master.db file, this is done at the INIT stage so we can still
 * reject the configuration / boot attempt.
 */
static int hbm_read_masterdb(void)
{
	UnrealDB *db;
	uint32_t mdb_version;

	db = unrealdb_open(test.masterdb, UNREALDB_MODE_READ, test.db_secret);

	if (!db)
	{
		if (unrealdb_get_error_code() == UNREALDB_ERROR_FILENOTFOUND)
		{
			/* Database does not exist. Could be first boot */
			config_warn("[history] No database present at '%s', will start a new one", cfg.masterdb);
			// TODO: maybe check for condition where 'master.db' does not exist but
			// there are other .db files.
			if (!hbm_write_masterdb())
				return 0; /* fatal error */
			return 1;
		} else
		{
			config_warn("[history] Unable to open the database file '%s' for reading: %s", cfg.masterdb, unrealdb_get_error_string());
			return 0;
		}
	}

	safe_free(cfg.prehash);
	safe_free(cfg.posthash);

	/* Master db has an easy format:
	 * 64 bits: version number
	 * string:  pre hash
	 * string:  post hash
	 */
	if (!unrealdb_read_int32(db, &mdb_version) ||
	    !unrealdb_read_str(db, &cfg.prehash) ||
	    !unrealdb_read_str(db, &cfg.posthash))
	{
		config_error("[history] Read error from database file '%s': %s",
			cfg.masterdb, unrealdb_get_error_string());
		unrealdb_close(db);
		return 0;
	}
	unrealdb_close(db);
	return 1;
}

/** Write the master.db file. Only call this if it does not exist yet! */
static int hbm_write_masterdb(void)
{
	UnrealDB *db;
	uint32_t mdb_version;
	char buf[512];

	if (!test.db_secret)
		abort();

	db = unrealdb_open(test.masterdb, UNREALDB_MODE_WRITE, test.db_secret);
	if (!db)
	{
		config_error("[history] Unable to write to '%s': %s",
			test.masterdb, unrealdb_get_error_string());
		return 0;
	}

	if (!cfg.prehash)
	{
		gen_random_alnum(buf, 128);
		safe_strdup(cfg.prehash, buf);
	}
	if (!cfg.posthash)
	{
		gen_random_alnum(buf, 128);
		safe_strdup(cfg.posthash, buf);
	}

	mdb_version = 5000;
	if (!unrealdb_write_int32(db, mdb_version) ||
	    !unrealdb_write_str(db, cfg.prehash) ||
	    !unrealdb_write_str(db, cfg.posthash))
	{
		config_error("[history] Unable to write to '%s': %s",
			test.masterdb, unrealdb_get_error_string());
		return 0;
	}
	unrealdb_close(db);
	return 1;
}

/** Read all database files (except master.db, which is already loaded) */
static void hbm_read_dbs(void)
{
	char buf[512];
#ifndef _WIN32
	struct dirent *dir;
	DIR *fd = opendir(cfg.directory);

	if (!fd)
		return;

	while ((dir = readdir(fd)))
	{
		char *fname = dir->d_name;
#else
	/* Windows */
	WIN32_FIND_DATA hData;
	HANDLE hFile;
	char xbuf[512];

	snprintf(xbuf, sizeof(xbuf), "%s/*.db", cfg.directory);

	hFile = FindFirstFile(xbuf, &hData);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	do
	{
		char *fname = hData.cFileName;
#endif

		/* Common section for both *NIX and Windows */

		snprintf(buf, sizeof(buf), "%s/%s", cfg.directory, fname);
		if (filename_has_suffix(fname, ".db") && strcmp(fname, "master.db"))
		{
			if (!hbm_read_db(buf))
			{
				/* On error, we move the file to the 'bad' subdirectory,
				 * eg data/history/bad/xyz.db
				 */
				char buf2[512];
				snprintf(buf2, sizeof(buf2), "%s/bad", cfg.directory);
#ifdef _WIN32
				(void)mkdir(buf2); /* (errors ignored) */
#else
				(void)mkdir(buf2, S_IRUSR|S_IWUSR|S_IXUSR); /* (errors ignored) */
#endif
				snprintf(buf2, sizeof(buf2), "%s/bad/%s", cfg.directory, fname);
				unlink(buf2);
				(void)rename(buf, buf2);
			}
		}

		/* End of common section */
#ifndef _WIN32
	}
	closedir(fd);
#else
	} while (FindNextFile(hFile, &hData));
	FindClose(hFile);
#endif
}

#define RESET_VALUES_LOOP()	do { \
					safe_free(mtag_name); \
					safe_free(mtag_value); \
					safe_free(line); \
					free_message_tags(mtags); \
					mtags = NULL; \
					magic = 0; \
					line_ts = 0; \
				} while(0)

#define R_SAFE_CLEANUP()	do { \
					unrealdb_close(db); \
					RESET_VALUES_LOOP(); \
					safe_free(prehash); \
					safe_free(posthash); \
					safe_free(object); \
				} while(0)
#define R_SAFE(x) \
	do { \
		if (!(x)) { \
			config_warn("[history] Read error from database file '%s' (possible corruption): %s", fname, unrealdb_get_error_string()); \
			R_SAFE_CLEANUP(); \
			return 0; \
		} \
	} while(0)


/** Read a channel history db file */
static int hbm_read_db(char *fname)
{
	UnrealDB *db = NULL;
	// header
	uint32_t magic = 0;
	uint32_t version = 0;
	char *prehash = NULL;
	char *posthash = NULL;
	char *object = NULL;
	uint64_t max_lines = 0;
	uint64_t max_time = 0;
	// then, for each entry:
	// (magic)
	uint64_t line_ts;
	char *mtag_name = NULL;
	char *mtag_value = NULL;
	MessageTag *mtags = NULL, *m;
	char *line = NULL;
	HistoryLogObject *h;

	db = unrealdb_open(fname, UNREALDB_MODE_READ, cfg.db_secret);
	if (!db)
	{
		config_warn("[history] Unable to open the database file '%s' for reading: %s", fname, unrealdb_get_error_string());
		return 0;
	}

	R_SAFE(unrealdb_read_int32(db, &magic));
	if (magic != HISTORYDB_MAGIC_FILE_START)
	{
		config_warn("[history] Database '%s' has wrong magic value, possibly corrupt (0x%lx), expected HISTORYDB_MAGIC_FILE_START.",
			fname, (long)magic);
		unrealdb_close(db);
		return 0;
	}

	/* Now do a version check */
	R_SAFE(unrealdb_read_int32(db, &version));
	if (version < 4999)
	{
		config_warn("[history] Database '%s' uses an unsupported - possibly old - format (%ld).", fname, (long)version);
		unrealdb_close(db);
		return 0;
	}
	if (version > 5000)
	{
		config_warn("[history] Database '%s' has version %lu while we only support %lu. Did you just downgrade UnrealIRCd? Sorry this is not suported",
			fname, (unsigned long)version, (unsigned long)5000);
		unrealdb_close(db);
		return 0;
	}

	R_SAFE(unrealdb_read_str(db, &prehash));
	R_SAFE(unrealdb_read_str(db, &posthash));

	if (!prehash || !posthash || strcmp(prehash, cfg.prehash) || strcmp(posthash, cfg.posthash))
	{
		config_warn("[history] Database '%s' does not belong to our 'master.db'. Are you mixing old with new .db files perhaps? This is not supported. File ignored.",
			fname);
		R_SAFE_CLEANUP();
		return 0;
	}

	R_SAFE(unrealdb_read_str(db, &object));
	R_SAFE(unrealdb_read_int64(db, &max_lines));
	R_SAFE(unrealdb_read_int64(db, &max_time));
	h = hbm_find_object(object);
	if (!h)
	{
		config_warn("Channel %s does not have +H set, deleting history", object);
		R_SAFE_CLEANUP();
		unlink(fname);
		return 1; /* No problem */
	}

	while(1)
	{
		RESET_VALUES_LOOP();
		R_SAFE(unrealdb_read_int32(db, &magic));
		if (magic == HISTORYDB_MAGIC_FILE_END)
			break; /* We're done, end gracefully */
		if (magic != HISTORYDB_MAGIC_ENTRY_START)
		{
			config_warn("[history] Read error from database file '%s': wrong magic value in entry (0x%lx), expected HISTORYDB_MAGIC_ENTRY_START",
				fname, (long)magic);
			R_SAFE_CLEANUP();
			return 0;
		}

		R_SAFE(unrealdb_read_int64(db, &line_ts));
		while(1)
		{
			R_SAFE(unrealdb_read_str(db, &mtag_name));
			R_SAFE(unrealdb_read_str(db, &mtag_value));
			if (!mtag_name && !mtag_value)
				break; /* We're done reading mtags for this particular line */
			m = safe_alloc(sizeof(MessageTag));
			safe_strdup(m->name, mtag_name);
			safe_strdup(m->value, mtag_value);
			AppendListItem(m, mtags);
			safe_free(mtag_name);
			safe_free(mtag_value);
		}
		R_SAFE(unrealdb_read_str(db, &line));
		R_SAFE(unrealdb_read_int32(db, &magic));
		if (magic != HISTORYDB_MAGIC_ENTRY_END)
		{
			config_warn("[history] Read error from database file '%s': wrong magic value in entry (0x%lx), expected HISTORYDB_MAGIC_ENTRY_END",
				fname, (long)magic);
			R_SAFE_CLEANUP();
			return 0;
		}
		hbm_history_add(object, mtags, line);
	}

	/* Prevent directly rewriting the channel, now that we have just read it.
	 * This could cause things not to fire in case of corner issues like
	 * hot-loading but that should be acceptable. The alternative is that
	 * all log files are written again with identical contents for no reason,
	 * which is a waste of resources.
	 */
	h->dirty = 0;

	R_SAFE_CLEANUP();
	return 1;
}

/** Periodically clean the history.
 * Instead of doing all channels in 1 go, we do a limited number
 * of channels each call, hence the 'static int' and the do { } while
 * rather than a regular for loop.
 * Note that we already impose the line limit in hbm_history_add,
 * so this history_mem_clean is for removals due to max_time limits.
 */
EVENT(history_mem_clean)
{
	static int hashnum = 0;
	int loopcnt = 0;
	Channel *channel;
	HistoryLogObject *h;

	do
	{
		for (h = history_hash_table[hashnum]; h; h = h->next)
		{
			hbm_history_cleanup(h);
			if (cfg.persist && h->dirty)
				hbm_write_db(h);
		}

		hashnum++;

		if (hashnum >= HISTORY_BACKEND_MEM_HASH_TABLE_SIZE)
			hashnum = 0;
	} while(loopcnt++ < HISTORY_CLEAN_PER_LOOP);
}

char *hbm_history_filename(HistoryLogObject *h)
{
	static char fname[512];
	char oname[OBJECTLEN+1];
	char hashdata[512];
	char hash[128];

	if (!cfg.prehash || !cfg.posthash)
		abort(); /* impossible */

	strtolower_safe(oname, h->name, sizeof(oname));
	snprintf(hashdata, sizeof(hashdata), "%s %s %s", cfg.prehash, oname, cfg.posthash);
	sha256hash(hash, hashdata, strlen(hashdata));

	snprintf(fname, sizeof(fname), "%s/%s.db", cfg.directory, hash);
	return fname;
}

#define WARN_WRITE_ERROR(fname) \
	do { \
		sendto_realops_and_log("[history] Error writing to temporary database file " \
		                       "'%s': %s (DATABASE NOT SAVED)", \
		                       fname, unrealdb_get_error_string()); \
	} while(0)

#define W_SAFE(x) \
	do { \
		if (!(x)) { \
			WARN_WRITE_ERROR(tmpfname); \
			unrealdb_close(db); \
			return 0; \
		} \
	} while(0)


// FIXME: the code below will cause massive floods on disk or I/O errors if hundreds of
// channel logs fail to write... fun.
static int hbm_write_db(HistoryLogObject *h)
{
	UnrealDB *db;
	char *realfname;
	char tmpfname[512];
	HistoryLogLine *l;
	MessageTag *m;
	Channel *channel;

	if (!cfg.db_secret)
		abort();

	channel = find_channel(h->name, NULL);
	if (!channel || !has_channel_mode(channel, 'P'))
		return 1; /* Don't save this channel, pretend success */

	realfname = hbm_history_filename(h);
	snprintf(tmpfname, sizeof(tmpfname), "%s.tmp", realfname);

	db = unrealdb_open(tmpfname, UNREALDB_MODE_WRITE, cfg.db_secret);
	if (!db)
	{
		WARN_WRITE_ERROR(tmpfname);
		return 0;
	}

	W_SAFE(unrealdb_write_int32(db, HISTORYDB_MAGIC_FILE_START));
	W_SAFE(unrealdb_write_int32(db, 5000)); /* VERSION */
	W_SAFE(unrealdb_write_str(db, cfg.prehash));
	W_SAFE(unrealdb_write_str(db, cfg.posthash));
	W_SAFE(unrealdb_write_str(db, h->name));

	W_SAFE(unrealdb_write_int64(db, h->max_lines));
	W_SAFE(unrealdb_write_int64(db, h->max_time));

	for (l = h->head; l; l = l->next)
	{
		W_SAFE(unrealdb_write_int32(db, HISTORYDB_MAGIC_ENTRY_START));
		W_SAFE(unrealdb_write_int64(db, l->t));
		for (m = l->mtags; m; m = m->next)
		{
			W_SAFE(unrealdb_write_str(db, m->name));
			W_SAFE(unrealdb_write_str(db, m->value)); /* can be NULL */
		}
		W_SAFE(unrealdb_write_str(db, NULL));
		W_SAFE(unrealdb_write_str(db, NULL));
		W_SAFE(unrealdb_write_str(db, l->line));
		W_SAFE(unrealdb_write_int32(db, HISTORYDB_MAGIC_ENTRY_END));
	}
	W_SAFE(unrealdb_write_int32(db, HISTORYDB_MAGIC_FILE_END));

	if (!unrealdb_close(db))
	{
		WARN_WRITE_ERROR(tmpfname);
		return 0;
	}

#ifdef _WIN32
	/* The rename operation cannot be atomic on Windows as it will cause a "file exists" error */
	unlink(realfname);
#endif
	if (rename(tmpfname, realfname) < 0)
	{
		sendto_realops_and_log("[history] Error renaming '%s' to '%s': %s (HISTORY NOT SAVED)",
			tmpfname, realfname, strerror(errno));
		return 0;
	}

	/* Now that everything was successful, clear the dirty flag */
	h->dirty = 0;
	return 1;
}

static void hbm_delete_db(HistoryLogObject *h)
{
	UnrealDB *db;
	char *fname;
	if (!cfg.persist || !cfg.prehash || !cfg.posthash)
	{
#ifdef DEBUGMODE
		abort(); /* we should not be called, so debug this */
#endif
		return;
	}
	fname = hbm_history_filename(h);
	unlink(fname);
}
