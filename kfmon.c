/*
	KFMon: Kobo inotify-based launcher
	Copyright (C) 2016-2019 NiLuJe <ninuje@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "kfmon.h"

// Fake FBInk in my sandbox...
#ifdef NILUJE
const char*
    fbink_version(void)
{
	return "N/A";
}

int
    fbink_open(void)
{
	return EXIT_SUCCESS;
}

int
    fbink_init(int fbfd __attribute__((unused)), const FBInkConfig* fbinkconfig __attribute__((unused)))
{
	return EXIT_SUCCESS;
}

int
    fbink_print(int                fbfd __attribute__((unused)),
		const char*        string,
		const FBInkConfig* fbinkconfig __attribute__((unused)))
{
	LOG(LOG_INFO, "FBInk: %s", string);
	return EXIT_SUCCESS;
}

int
    fbink_printf(int                  fbfd __attribute__((unused)),
		 const FBInkOTConfig* fbinkotconfig __attribute__((unused)),
		 const FBInkConfig*   fbinkconfig __attribute__((unused)),
		 const char*          fmt,
		 ...)
{
	char buffer[256];

	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

	LOG(LOG_INFO, "FBInk: %s", buffer);
	return EXIT_SUCCESS;
}

int
    fbink_reinit(int fbfd __attribute__((unused)), const FBInkConfig* fbinkconfig __attribute__((unused)))
{
	return EXIT_SUCCESS;
}
#endif

// Because daemon() only appeared in glibc 2.21 (and doesn't double-fork anyway)
static int
    daemonize(void)
{
	int fd;

	switch (fork()) {
		case -1:
			return -1;
		case 0:
			break;
		default:
			_exit(EXIT_SUCCESS);
	}

	if (setsid() == -1) {
		return -1;
	}

	// Double fork, for... reasons!
	// In practical terms, this ensures we get re-parented to init *now*.
	// Ignore SIGHUP while we're there, since we don't want to be killed by it.
	signal(SIGHUP, SIG_IGN);
	switch (fork()) {
		case -1:
			return -1;
		case 0:
			break;
		default:
			_exit(EXIT_SUCCESS);
	}

	if (chdir("/") == -1) {
		return -1;
	}

	// Make sure we keep honoring rcS's umask
	umask(022);    // Flawfinder: ignore

	// Store a copy of stdin, stdout & stderr so we can restore it to our children later on...
	// NOTE: Hence the + 3 in the two (three w/ use_syslog) following fd tests.
	orig_stdin  = dup(fileno(stdin));
	orig_stdout = dup(fileno(stdout));
	orig_stderr = dup(fileno(stderr));

	// Redirect stdin & stdout to /dev/null
	if ((fd = open("/dev/null", O_RDWR)) != -1) {
		dup2(fd, fileno(stdin));
		dup2(fd, fileno(stdout));
		if (fd > 2 + 3) {
			close(fd);
		}
	} else {
		fprintf(stderr, "Failed to redirect stdin & stdout to /dev/null\n");
		return -1;
	}

	// Redirect stderr to our logfile
	int flags = O_WRONLY | O_CREAT | O_APPEND;
	// Check if we need to truncate our log because it has grown too much...
	struct stat st;
	if ((stat(KFMON_LOGFILE, &st) == 0) && (S_ISREG(st.st_mode))) {
		// Truncate if > 1MB
		if (st.st_size > 1 * 1024 * 1024) {
			flags |= O_TRUNC;
		}
	}
	if ((fd = open(KFMON_LOGFILE, flags, 0600)) != -1) {
		dup2(fd, fileno(stderr));
		if (fd > 2 + 3) {
			close(fd);
		}
	} else {
		fprintf(stderr, "Failed to redirect stderr to logfile '%s'\n", KFMON_LOGFILE);
		return -1;
	}

	return 0;
}

// Wrapper around localtime_r, making sure this part is thread-safe (used for logging)
struct tm*
    get_localtime(struct tm* lt)
{
	time_t t = time(NULL);
	tzset();

	return localtime_r(&t, lt);
}

// Wrapper around strftime, making sure this part is thread-safe (used for logging)
char*
    format_localtime(struct tm* lt, char* sz_time, size_t len)
{
	// c.f., strftime(3) & https://stackoverflow.com/questions/7411301
	strftime(sz_time, len, "%Y-%m-%d @ %H:%M:%S", lt);

	return sz_time;
}

// Return the current time formatted as 2016-04-29 @ 20:44:13 (used for logging)
// NOTE: The use of static variables prevents this from being thread-safe,
//       but in the main thread, we use static storage for simplicity's sake.
char*
    get_current_time(void)
{
	static struct tm local_tm = { 0 };
	struct tm*       lt       = get_localtime(&local_tm);

	static char sz_time[22];

	return format_localtime(lt, sz_time, sizeof(sz_time));
}

// And now the same, but with user supplied storage, thus potentially thread-safe:
// f.g., we use the stack in reaper_thread().
char*
    get_current_time_r(struct tm* local_tm, char* sz_time, size_t len)
{
	struct tm* lt = get_localtime(local_tm);
	return format_localtime(lt, sz_time, len);
}

const char*
    get_log_prefix(int prio)
{
	// Reuse (part of) the syslog() priority constants
	switch (prio) {
		case LOG_CRIT:
			return "CRIT";
		case LOG_ERR:
			return "ERR!";
		case LOG_WARNING:
			return "WARN";
		case LOG_NOTICE:
			return "NOTE";
		case LOG_INFO:
			return "INFO";
		case LOG_DEBUG:
			return "DBG!";
		default:
			return "OOPS";
	}
}

// Check that our target mountpoint is indeed mounted...
static bool
    is_target_mounted(void)
{
	// c.f., http://program-nix.blogspot.com/2008/08/c-language-check-filesystem-is-mounted.html
	FILE*          mtab       = NULL;
	struct mntent* part       = NULL;
	bool           is_mounted = false;

	if ((mtab = setmntent("/proc/mounts", "r")) != NULL) {
		while ((part = getmntent(mtab)) != NULL) {
			DBGLOG("Checking fs %s mounted on %s", part->mnt_fsname, part->mnt_dir);
			if ((part->mnt_dir != NULL) && (strcmp(part->mnt_dir, KFMON_TARGET_MOUNTPOINT)) == 0) {
				is_mounted = true;
				break;
			}
		}
		endmntent(mtab);
	}

	return is_mounted;
}

// Monitor mountpoint activity...
static void
    wait_for_target_mountpoint(void)
{
	// c.f., https://stackoverflow.com/questions/5070801
	int           mfd = open("/proc/mounts", O_RDONLY, 0);
	struct pollfd pfd;

	uint8_t changes = 0;
	pfd.fd          = mfd;
	pfd.events      = POLLERR | POLLPRI;
	pfd.revents     = 0;
	while (poll(&pfd, 1, -1) >= 0) {
		if (pfd.revents & POLLERR) {
			LOG(LOG_INFO, "Mountpoints changed (iteration nr. %hhu)", (uint8_t) changes++);

			// Stop polling once we know our mountpoint is available...
			if (is_target_mounted()) {
				LOG(LOG_NOTICE, "Yay! Target mountpoint is available!");
				break;
			}
		}
		pfd.revents = 0;

		// If we can't find our mountpoint after that many changes, assume we're screwed...
		if (changes >= 5) {
			LOG(LOG_ERR, "Too many mountpoint changes without finding our target (shutdown?), aborting!");
			close(mfd);
			exit(EXIT_FAILURE);
		}
	}

	close(mfd);
}

// Sanitize user input for keys expecting an unsigned short integer
// NOTE: Inspired from git's strtoul_ui @ git-compat-util.h
static int
    strtoul_hu(const char* str, unsigned short int* result)
{
	// NOTE: We want to *reject* negative values (which strtoul does not)!
	if (strchr(str, '-')) {
		LOG(LOG_WARNING, "Assigned a negative value (%s) to a key expecting an unsigned short int.", str);
		return -EINVAL;
	}

	// Now that we know it's positive, we can go on with strtoul...
	char*             endptr;
	unsigned long int val;

	errno = 0;    // To distinguish success/failure after call
	val   = strtoul(str, &endptr, 10);

	if ((errno == ERANGE && val == ULONG_MAX) || (errno != 0 && val == 0)) {
		perror("[KFMon] [WARN] strtoul");
		return -EINVAL;
	}

	// NOTE: It fact, always clamp to SHRT_MAX, since some of these may end up cast to an int (f.g., db_timeout)
	if (val > SHRT_MAX) {
		LOG(LOG_WARNING,
		    "Encountered a value larger than SHRT_MAX assigned to a key, clamping it down to SHRT_MAX");
		val = SHRT_MAX;
	}

	if (endptr == str) {
		LOG(LOG_WARNING,
		    "No digits were found in value '%s' assigned to a key expecting an unsigned short int.",
		    str);
		return -EINVAL;
	}

	// If we got here, strtoul() successfully parsed at least part of a number.
	// But we do want to enforce the fact that the input really was *only* an integer value.
	if (*endptr != '\0') {
		LOG(LOG_WARNING,
		    "Found trailing characters (%s) behind value '%lu' assigned from string '%s' to a key expecting an unsigned short int.",
		    endptr,
		    val,
		    str);
		return -EINVAL;
	}

	// Make sure there isn't a loss of precision on this arch when casting explictly
	if ((unsigned short int) val != val) {
		LOG(LOG_WARNING, "Loss of precision when casting value '%lu' to an unsigned short int.", val);
		return -EINVAL;
	}

	*result = (unsigned short int) val;
	return EXIT_SUCCESS;
}

// Sanitize user input for keys expecting a boolean
// NOTE: Inspired from Linux's strtobool (tools/lib/string.c) as well as sudo's implementation of the same.
static int
    strtobool(const char* str, bool* result)
{
	if (!str) {
		LOG(LOG_WARNING, "Passed an empty value to a key expecting a boolean.");
		return -EINVAL;
	}

	switch (str[0]) {
		case 't':
		case 'T':
			if (strcasecmp(str, "true") == 0) {
				*result = true;
				return EXIT_SUCCESS;
			}
			break;
		case 'y':
		case 'Y':
			if (strcasecmp(str, "yes") == 0) {
				*result = true;
				return EXIT_SUCCESS;
			}
			break;
		case '1':
			if (str[1] == '\0') {
				*result = true;
				return EXIT_SUCCESS;
			}
			break;
		case 'f':
		case 'F':
			if (strcasecmp(str, "false") == 0) {
				*result = false;
				return EXIT_SUCCESS;
			}
			break;
		case 'n':
		case 'N':
			switch (str[1]) {
				case 'o':
				case 'O':
					if (str[2] == '\0') {
						*result = false;
						return EXIT_SUCCESS;
					}
					break;
				default:
					break;
			}
			break;
		case '0':
			if (str[1] == '\0') {
				*result = false;
				return EXIT_SUCCESS;
			}
			break;
		case 'o':
		case 'O':
			switch (str[1]) {
				case 'n':
				case 'N':
					if (str[2] == '\0') {
						*result = true;
						return EXIT_SUCCESS;
					}
					break;
				case 'f':
				case 'F':
					if (str[2] == '\0') {
						*result = false;
						return EXIT_SUCCESS;
					}
					break;
				default:
					break;
			}
			break;
		default:
			// NOTE: *result is zero-initialized, no need to explicitly set it to false
			break;
	}

	LOG(LOG_WARNING, "Assigned an invalid or malformed value (%s) to a key expecting a boolean.", str);
	return -EINVAL;
}

// Handle parsing the main KFMon config
static int
    daemon_handler(void* user, const char* section, const char* key, const char* value)
{
	DaemonConfig* pconfig = (DaemonConfig*) user;

#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(key, n) == 0
	if (MATCH("daemon", "db_timeout")) {
		if (strtoul_hu(value, &pconfig->db_timeout) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for db_timeout!");
			return 0;
		}
	} else if (MATCH("daemon", "use_syslog")) {
		if (strtobool(value, &pconfig->use_syslog) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for use_syslog!");
			return 0;
		}
	} else if (MATCH("daemon", "with_notifications")) {
		if (strtobool(value, &pconfig->with_notifications) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for with_notifications!");
			return 0;
		}
	} else {
		return 0;    // unknown section/name, error
	}
	return 1;
}

// Handle parsing a watch config
static int
    watch_handler(void* user, const char* section, const char* key, const char* value)
{
	WatchConfig* pconfig = (WatchConfig*) user;

#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(key, n) == 0
	// NOTE: Crappy strncpy() usage, but those char arrays are zeroed first
	//       (hence the MAX-1 len to ensure that we're NULL terminated)...
	if (MATCH("watch", "filename")) {
		strncpy(pconfig->filename, value, KFMON_PATH_MAX - 1);    // Flawfinder: ignore
	} else if (MATCH("watch", "action")) {
		strncpy(pconfig->action, value, KFMON_PATH_MAX - 1);    // Flawfinder: ignore
	} else if (MATCH("watch", "skip_db_checks")) {
		if (strtobool(value, &pconfig->skip_db_checks) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for skip_db_checks!");
			return 0;
		}
	} else if (MATCH("watch", "do_db_update")) {
		if (strtobool(value, &pconfig->do_db_update) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for do_db_update!");
			return 0;
		}
	} else if (MATCH("watch", "db_title")) {
		strncpy(pconfig->db_title, value, DB_SZ_MAX - 1);    // Flawfinder: ignore
	} else if (MATCH("watch", "db_author")) {
		strncpy(pconfig->db_author, value, DB_SZ_MAX - 1);    // Flawfinder: ignore
	} else if (MATCH("watch", "db_comment")) {
		strncpy(pconfig->db_comment, value, DB_SZ_MAX - 1);    // Flawfinder: ignore
	} else if (MATCH("watch", "block_spawns")) {
		if (strtobool(value, &pconfig->block_spawns) < 0) {
			LOG(LOG_CRIT, "Passed an invalid value for block_spawns!");
			return 0;
		}
	} else if (MATCH("watch", "reboot_on_exit")) {
		;
	} else {
		return 0;    // unknown section/name, error
	}
	return 1;
}

// Validate a watch config
static bool
    validate_watch_config(void* user)
{
	WatchConfig* pconfig = (WatchConfig*) user;

	bool sane = true;

	if (pconfig->filename[0] == '\0') {
		LOG(LOG_CRIT, "Mandatory key 'filename' is missing or blank!");
		sane = false;
	} else {
		// Make sure we're not trying to set multiple watches on the same file...
		// (because that would only actually register the first one parsed).
		uint8_t matches = 0;
		for (uint8_t watch_idx = 0; watch_idx < WATCH_MAX; watch_idx++) {
			if (strcmp(pconfig->filename, watch_config[watch_idx].filename) == 0) {
				matches++;
			}
		}
		// Since we'll necessarily loop over ourselves, only warn if we matched two or more times.
		if (matches >= 2) {
			LOG(LOG_WARNING, "Tried to setup multiple watches on file '%s'!", pconfig->filename);
			sane = false;
		}
	}
	if (pconfig->action[0] == '\0') {
		LOG(LOG_CRIT, "Mandatory key 'action' is missing or blank!");
		sane = false;
	}

	// If we asked for a database update, the next three keys become mandatory
	if (pconfig->do_db_update) {
		if (pconfig->db_title[0] == '\0') {
			LOG(LOG_CRIT, "Mandatory key 'db_title' is missing or blank!");
			sane = false;
		}
		if (pconfig->db_author[0] == '\0') {
			LOG(LOG_CRIT, "Mandatory key 'db_author' is missing or blank!");
			sane = false;
		}
		if (pconfig->db_comment[0] == '\0') {
			LOG(LOG_CRIT, "Mandatory key 'db_comment' is missing or blank!");
			sane = false;
		}
	}

	return sane;
}

// Load our config files...
static int
    load_config(void)
{
	// Our config files live in the target mountpoint...
	if (!is_target_mounted()) {
		LOG(LOG_NOTICE, "%s isn't mounted, waiting for it to be . . .", KFMON_TARGET_MOUNTPOINT);
		// If it's not, wait for it to be...
		wait_for_target_mountpoint();
	}

	// Walk the config directory to pickup our ini files... (c.f.,
	// https://keramida.wordpress.com/2009/07/05/fts3-or-avoiding-to-reinvent-the-wheel/)
	FTS*    ftsp;
	FTSENT* p;
	FTSENT* chp;
	// We only need to walk a single directory...
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types-discards-qualifiers"
	char* const cfg_path[] = { KFMON_CONFIGPATH, NULL };
#pragma GCC diagnostic pop
	int ret;
	int rval = 0;

	// Don't chdir (because that mountpoint can go buh-bye), and don't stat (because we don't need to).
	if ((ftsp = fts_open(cfg_path, FTS_COMFOLLOW | FTS_LOGICAL | FTS_NOCHDIR | FTS_NOSTAT | FTS_XDEV, NULL)) ==
	    NULL) {
		perror("[KFMon] [CRIT] fts_open");
		return -1;
	}
	// Initialize ftsp with as many toplevel entries as possible.
	chp = fts_children(ftsp, 0);
	if (chp == NULL) {
		// No files to traverse!
		LOG(LOG_CRIT, "Config directory '%s' appears to be empty, aborting!", KFMON_CONFIGPATH);
		fts_close(ftsp);
		return -1;
	}
	while ((p = fts_read(ftsp)) != NULL) {
		switch (p->fts_info) {
			case FTS_F:
				// Check if it's a .ini and not either an unix hidden file or a Mac resource fork...
				if (p->fts_namelen > 4 &&
				    strncasecmp(p->fts_name + (p->fts_namelen - 4), ".ini", 4) == 0 &&
				    strncasecmp(p->fts_name, ".", 1) != 0) {
					LOG(LOG_INFO, "Trying to load config file '%s' . . .", p->fts_path);
					// The main config has to be parsed slightly differently...
					if (strcasecmp(p->fts_name, "kfmon.ini") == 0) {
						// NOTE: Can technically return -1 on file open error,
						//       but that shouldn't really ever happen
						//       given the nature of the loop we're in ;).
						ret = ini_parse(p->fts_path, daemon_handler, &daemon_config);
						if (ret != 0) {
							LOG(LOG_CRIT,
							    "Failed to parse main config file '%s' (first error on line %d), will abort!",
							    p->fts_name,
							    ret);
							// Flag as a failure...
							rval = -1;
						} else {
							LOG(LOG_NOTICE,
							    "Daemon config loaded from '%s': db_timeout=%hu, use_syslog=%d, with_notifications=%d",
							    p->fts_name,
							    daemon_config.db_timeout,
							    daemon_config.use_syslog,
							    daemon_config.with_notifications);
						}
					} else {
						// NOTE: Don't blow up when trying to store more watches than we have
						//       space for...
						if (watch_count >= WATCH_MAX) {
							LOG(LOG_WARNING,
							    "We've already setup the maximum amount of watches we can handle (%d), discarding '%s'!",
							    WATCH_MAX,
							    p->fts_name);
							// Don't flag this as a hard failure, just warn and go on...
							break;
						}

						ret = ini_parse(p->fts_path, watch_handler, &watch_config[watch_count]);
						if (ret != 0) {
							LOG(LOG_CRIT,
							    "Failed to parse watch config file '%s' (first error on line %d), will abort!",
							    p->fts_name,
							    ret);
							// Flag as a failure...
							rval = -1;
						} else {
							if (validate_watch_config(&watch_config[watch_count])) {
								LOG(LOG_NOTICE,
								    "Watch config @ index %hhu loaded from '%s': filename=%s, action=%s, block_spawns=%d, do_db_update=%d, db_title=%s, db_author=%s, db_comment=%s",
								    watch_count,
								    p->fts_name,
								    watch_config[watch_count].filename,
								    watch_config[watch_count].action,
								    watch_config[watch_count].block_spawns,
								    watch_config[watch_count].do_db_update,
								    watch_config[watch_count].db_title,
								    watch_config[watch_count].db_author,
								    watch_config[watch_count].db_comment);
							} else {
								LOG(LOG_CRIT,
								    "Watch config file '%s' is not valid, will abort!",
								    p->fts_name);
								rval = -1;
							}
						}
						// No matter what, switch to the next slot:
						// we rely on zero-initialization (c.f., the comments around
						// our strncpy() usage in watch_handler), so we can't reuse a slot,
						// even in case of failure,
						// or we risk mixing values from different config files together,
						// which is why a broken watch config is flagged as a fatal failure.
						watch_count++;
					}
				}
				break;
			default:
				break;
		}
	}
	fts_close(ftsp);

#ifdef DEBUG
	// Let's recap (including failures)...
	DBGLOG("Daemon config recap: db_timeout=%hu, use_syslog=%d, with_notifications=%d",
	       daemon_config.db_timeout,
	       daemon_config.use_syslog,
	       daemon_config.with_notifications);
	for (uint8_t watch_idx = 0; watch_idx < watch_count; watch_idx++) {
		DBGLOG(
		    "Watch config @ index %hhu recap: filename=%s, action=%s, block_spawns=%d, skip_db_checks=%d, do_db_update=%d, db_title=%s, db_author=%s, db_comment=%s",
		    watch_idx,
		    watch_config[watch_idx].filename,
		    watch_config[watch_idx].action,
		    watch_config[watch_idx].block_spawns,
		    watch_config[watch_idx].skip_db_checks,
		    watch_config[watch_idx].do_db_update,
		    watch_config[watch_idx].db_title,
		    watch_config[watch_idx].db_author,
		    watch_config[watch_idx].db_comment);
	}
#endif

	return rval;
}

// Implementation of Qt4's QtHash (c.f., qhash @
// https://github.com/kovidgoyal/calibre/blob/master/src/calibre/devices/kobo/driver.py#L37)
static unsigned int
    qhash(const unsigned char* bytes, size_t length)
{
	unsigned int h = 0;

	for (unsigned int i = 0; i < length; i++) {
		h = (h << 4) + bytes[i];
		h ^= (h & 0xf0000000) >> 23;
		h &= 0x0fffffff;
	}

	return h;
}

// Check if our target file has been processed by Nickel...
static bool
    is_target_processed(uint8_t watch_idx, bool wait_for_db)
{
	sqlite3*      db;
	sqlite3_stmt* stmt;
	int           rc;
	int           idx;
	bool          is_processed = false;
	bool          needs_update = false;

#ifdef DEBUG
	// Bypass DB checks on demand for debugging purposes...
	if (watch_config[watch_idx].skip_db_checks)
		return true;
#endif

	// Did the user want to try to update the DB for this icon?
	bool update = watch_config[watch_idx].do_db_update;

	// NOTE: Open the db in multi-thread threading mode (we build w/ threadsafe and we don't use sqlite_config),
	//       and without a shared cache because we have no use for it, we only do SQL from the main thread.
	if (update) {
		CALL_SQLITE(open_v2(
		    KOBO_DB_PATH, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, NULL));
	} else {
		// Open the DB ro to be extra-safe...
		CALL_SQLITE(open_v2(
		    KOBO_DB_PATH, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, NULL));
	}

	// Wait at most for Nms on OPEN & N*2ms on CLOSE if we ever hit a locked database during any of our proceedings.
	// NOTE: The defaults timings (steps of 500ms) appear to work reasonably well on my H2O with a 50MB Nickel DB...
	//       (i.e., it trips on OPEN when Nickel is moderately busy, but if everything's quiet, we're good).
	//       Time will tell if that's a good middle-ground or not ;).
	//       This is user configurable in kfmon.ini (db_timeout key).
	// NOTE: On current FW versions, where the DB is now using WAL, we're exceedingly unlikely to ever hit a BUSY DB
	//       (c.f., https://www.sqlite.org/wal.html)
	sqlite3_busy_timeout(db, (int) daemon_config.db_timeout * (wait_for_db + 1));
	DBGLOG("SQLite busy timeout set to %dms", (int) daemon_config.db_timeout * (wait_for_db + 1));

	// NOTE: ContentType 6 should mean a book on pretty much anything since FW 1.9.17 (and why a book?
	//       Because Nickel currently identifies single PNGs as application/x-cbz, bless its cute little bytes).
	CALL_SQLITE(prepare_v2(
	    db, "SELECT EXISTS(SELECT 1 FROM content WHERE ContentID = @id AND ContentType = '6');", -1, &stmt, NULL));

	// Append the proper URI scheme to our icon path...
	char book_path[KFMON_PATH_MAX + 7];
	snprintf(book_path, KFMON_PATH_MAX + 7, "file://%s", watch_config[watch_idx].filename);

	idx = sqlite3_bind_parameter_index(stmt, "@id");
	CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		DBGLOG("SELECT SQL query returned: %d", sqlite3_column_int(stmt, 0));
		if (sqlite3_column_int(stmt, 0) == 1) {
			is_processed = true;
		}
	}

	sqlite3_finalize(stmt);

	// Now that we know the book exists, we also want to check if the thumbnails do,
	// to avoid getting triggered from the thumbnail creation...
	// NOTE: Again, this assumes FW >= 2.9.0
	if (is_processed) {
		// Assume they haven't been processed until we can confirm it...
		is_processed = false;

		// We'll need the ImageID first...
		CALL_SQLITE(prepare_v2(
		    db, "SELECT ImageID FROM content WHERE ContentID = @id AND ContentType = '6';", -1, &stmt, NULL));

		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			const unsigned char* image_id = sqlite3_column_text(stmt, 0);
			size_t               len      = (size_t) sqlite3_column_bytes(stmt, 0);
			DBGLOG("SELECT SQL query returned: %s", image_id);

			// Then we need the proper hashes Nickel devises...
			// c.f., images_path @
			// https://github.com/kovidgoyal/calibre/blob/master/src/calibre/devices/kobo/driver.py#L2489
			unsigned int hash = qhash(image_id, len);
			unsigned int dir1 = hash & (0xff * 1);
			unsigned int dir2 = (hash & (0xff00 * 1)) >> 8;

			char images_path[KFMON_PATH_MAX];
			snprintf(
			    images_path, KFMON_PATH_MAX, "%s/.kobo-images/%u/%u", KFMON_TARGET_MOUNTPOINT, dir1, dir2);
			DBGLOG("Checking for thumbnails in '%s' . . .", images_path);

			// Count the number of processed thumbnails we find...
			uint8_t thumbnails_count = 0;
			char    thumbnail_path[KFMON_PATH_MAX];

			// Start with the full-size screensaver...
			snprintf(thumbnail_path, KFMON_PATH_MAX, "%s/%s - N3_FULL.parsed", images_path, image_id);
			DBGLOG("Checking for full-size screensaver '%s' . . .", thumbnail_path);
			if (access(thumbnail_path, F_OK) == 0) {
				thumbnails_count++;
			} else {
				LOG(LOG_INFO, "Full-size screensaver hasn't been parsed yet!");
			}

			// Then the Homescreen tile...
			// NOTE: This one might be a tad confusing...
			//       If the icon has never been processed,
			//       this will only happen the first time we *close* the PNG's "book"...
			//       (i.e., the moment it pops up as the 'last opened' tile).
			//       And *that* processing triggers a set of OPEN & CLOSE,
			//       meaning we can quite possibly run on book *exit* that first time,
			//       (and only that first time), if database locking permits...
			snprintf(thumbnail_path, KFMON_PATH_MAX, "%s/%s - N3_LIBRARY_FULL.parsed", images_path, image_id);
			DBGLOG("Checking for homescreen tile '%s' . . .", thumbnail_path);
			if (access(thumbnail_path, F_OK) == 0) {
				thumbnails_count++;
			} else {
				LOG(LOG_INFO, "Homescreen tile hasn't been parsed yet!");
			}

			// And finally the Library thumbnail...
			snprintf(thumbnail_path, KFMON_PATH_MAX, "%s/%s - N3_LIBRARY_GRID.parsed", images_path, image_id);
			DBGLOG("Checking for library thumbnail '%s' . . .", thumbnail_path);
			if (access(thumbnail_path, F_OK) == 0) {
				thumbnails_count++;
			} else {
				LOG(LOG_INFO, "Library thumbnail hasn't been parsed yet!");
			}

			// Only give a greenlight if we got all three!
			if (thumbnails_count == 3) {
				is_processed = true;
			}
		}

		// NOTE: It's now safe to destroy the statement.
		//       (We can't do that early in the success branch,
		//       because we still hold a pointer to a result depending on the statement (image_id))
		sqlite3_finalize(stmt);
	}

	// NOTE: Here be dragons!
	//       This works in theory,
	//       but risks confusing Nickel's handling of the DB if we do that when nickel is running (which we are).
	//       Because doing it with Nickel running is a potentially terrible idea,
	//       for various reasons (c.f., https://www.sqlite.org/howtocorrupt.html for the gory details,
	//       some of which probably even apply here! :p).
	//       As such, we leave enabling this option to the user's responsibility.
	//       KOReader ships with it disabled.
	//       The idea is to, optionally, update the Title, Author & Comment fields to make them more useful...
	if (is_processed && update) {
		// Check if the DB has already been updated by checking the title...
		CALL_SQLITE(prepare_v2(
		    db, "SELECT Title FROM content WHERE ContentID = @id AND ContentType = '6';", -1, &stmt, NULL));

		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW) {
			DBGLOG("SELECT SQL query returned: %s", sqlite3_column_text(stmt, 0));
			if (strcmp((const char*) sqlite3_column_text(stmt, 0), watch_config[watch_idx].db_title) != 0) {
				needs_update = true;
			}
		}

		sqlite3_finalize(stmt);
	}
	if (needs_update) {
		CALL_SQLITE(prepare_v2(
		    db,
		    "UPDATE content SET Title = @title, Attribution = @author, Description = @comment WHERE ContentID = @id AND ContentType = '6';",
		    -1,
		    &stmt,
		    NULL));

		// NOTE: No sanity checks are done to confirm that those watch configs are sane,
		//       we only check that they are *present*...
		//       The example config ships with a strong warning not to forget them if wanted, but that's it.
		idx = sqlite3_bind_parameter_index(stmt, "@title");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_title, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@author");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_author, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@comment");
		CALL_SQLITE(bind_text(stmt, idx, watch_config[watch_idx].db_comment, -1, SQLITE_STATIC));
		idx = sqlite3_bind_parameter_index(stmt, "@id");
		CALL_SQLITE(bind_text(stmt, idx, book_path, -1, SQLITE_STATIC));

		rc = sqlite3_step(stmt);
		if (rc != SQLITE_DONE) {
			LOG(LOG_WARNING, "UPDATE SQL query failed: %s", sqlite3_errmsg(db));
		} else {
			LOG(LOG_NOTICE, "Successfully updated DB data for the target PNG");
		}

		sqlite3_finalize(stmt);
	}

	// A rather crappy check to wait for pending COMMITs...
	if (is_processed && wait_for_db) {
		// If there's a rollback journal for the DB, wait for it to go away...
		// NOTE: This assumes the DB was opened with the default journal_mode, DELETE
		//       This doesn't appear to be the case anymore, on FW >= 4.6.x (and possibly earlier),
		//       it's now using WAL (which makes sense, and our whole job safer ;)).
		const struct timespec zzz   = { 0L, 500000000L };
		uint8_t               count = 0;
		while (access(KOBO_DB_PATH "-journal", F_OK) == 0) {
			LOG(LOG_INFO,
			    "Found a SQLite rollback journal, waiting for it to go away (iteration nr. %hhu) . . .",
			    (uint8_t) count++);
			nanosleep(&zzz, NULL);
			// NOTE: Don't wait more than 10s
			if (count >= 20) {
				LOG(LOG_WARNING,
				    "Waited for the SQLite rollback journal to go away for far too long, going on anyway.");
				break;
			}
		}
	}

	sqlite3_close(db);

	return is_processed;
}

// Heavily inspired from https://stackoverflow.com/a/35235950
// Initializes the process table. -1 means the entry in the table is available.
static void
    init_process_table(void)
{
	for (uint8_t i = 0; i < WATCH_MAX; i++) {
		PT.spawn_pids[i]     = -1;
		PT.spawn_watchids[i] = -1;
	}
}

// Returns the index of the next available entry in the process table.
static int8_t
    get_next_available_pt_entry(void)
{
	for (uint8_t i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] == -1) {
			return (int8_t) i;
		}
	}
	return -1;
}

// Adds information about a new spawn to the process table.
static void
    add_process_to_table(uint8_t i, pid_t pid, uint8_t watch_idx)
{
	PT.spawn_pids[i]     = pid;
	PT.spawn_watchids[i] = (int8_t) watch_idx;
}

// Removes information about a spawn from the process table.
static void
    remove_process_from_table(uint8_t i)
{
	PT.spawn_pids[i]     = -1;
	PT.spawn_watchids[i] = -1;
}

// Initializes the FBInk config
static void
    init_fbink_config(void)
{
	// NOTE: The struct is zero-initialized, so we only tweak what's non-default
	//       (the defaults are explictly designed to always be 0 for this very purpose).
	fbink_config.row         = -5;
	fbink_config.is_centered = true;
	fbink_config.is_padded   = true;
	// NOTE: For now, we *want* fbink_init's status report logged, so we leave this disabled.
	// fbink_config.is_quiet = false;
}

// Wait for a specific child process to die, and reap it (runs in a dedicated thread per spawn).
void*
    reaper_thread(void* ptr)
{
	uint8_t i = *((uint8_t*) ptr);

	pid_t tid;
	tid = (pid_t) syscall(SYS_gettid);

	pid_t   cpid;
	uint8_t watch_idx;
	pthread_mutex_lock(&ptlock);
	cpid      = PT.spawn_pids[i];
	watch_idx = (uint8_t) PT.spawn_watchids[i];
	pthread_mutex_unlock(&ptlock);

	// Storage needed for get_current_time_r
	struct tm local_tm;
	char      sz_time[22];

	// Remember the current time for the execvp errno/exitcode heuristic...
	time_t then = time(NULL);

	MTLOG("[%s] [INFO] [TID: %ld] Waiting to reap process %ld (from watch idx %hhu) . . .",
	      get_current_time_r(&local_tm, sz_time, sizeof(sz_time)),
	      (long) tid,
	      (long) cpid,
	      watch_idx);
	pid_t ret;
	int   wstatus;
	// Wait for our child process to terminate, retrying on EINTR
	do {
		ret = waitpid(cpid, &wstatus, 0);
	} while (ret == -1 && errno == EINTR);
	// Recap what happened to it
	if (ret != cpid) {
		perror("[KFMon] [CRIT] waitpid");
		free(ptr);
		return (void*) NULL;
	} else {
		if (WIFEXITED(wstatus)) {
			int exitcode = WEXITSTATUS(wstatus);
			MTLOG(
			    "[%s] [NOTE] [TID: %ld] Reaped process %ld (from watch idx %hhu): It exited with status %d.",
			    get_current_time_r(&local_tm, sz_time, sizeof(sz_time)),
			    (long) tid,
			    (long) cpid,
			    watch_idx,
			    exitcode);
			// NOTE: Ugly hack to try to salvage execvp's potential error...
			//       If the process exited with a non-zero status code,
			//       within (roughly) a second of being launched,
			//       assume the exit code is actually inherited from execvp's errno...
			time_t now = time(NULL);
			// NOTE: We should be okay not using difftime on Linux (An Epoch is in UTC, time_t is int64_t).
			if (exitcode != 0 && (now - then) <= 1) {
				char buf[256];
				// NOTE: We *know* we'll be using the GNU, glibc >= 2.13 version of strerror_r
				// NOTE: Even if it's not entirely clear from the manpage, printf's %m *is* thread-safe,
				//       c.f., stdio-common/vfprintf.c:962 (it's using strerror_r).
				//       But since we're not checking errno but a custom variable, do it the hard way :)
				char* sz_error = strerror_r(exitcode, buf, sizeof(buf));
				MTLOG(
				    "[%s] [CRIT] [TID: %ld] If nothing was visibly launched, and/or especially if status > 1, this *may* actually be an execvp() error: %s.",
				    get_current_time_r(&local_tm, sz_time, sizeof(sz_time)),
				    (long) tid,
				    sz_error);
				fbink_printf(FBFD_AUTO,
					     NULL,
					     &fbink_config,
					     "[KFMon] PID %ld exited unexpectedly: %d!",
					     (long) cpid,
					     exitcode);
			}
		} else if (WIFSIGNALED(wstatus)) {
			// NOTE: strsignal is not thread safe... Use psignal instead.
			int  sigcode = WTERMSIG(wstatus);
			char buf[256];
			snprintf(
			    buf,
			    sizeof(buf),
			    "[KFMon] [%s] [WARN] [TID: %ld] Reaped process %ld (from watch idx %hhu): It was killed by signal %d",
			    get_current_time_r(&local_tm, sz_time, sizeof(sz_time)),
			    (long) tid,
			    (long) cpid,
			    watch_idx,
			    sigcode);
			fbink_printf(FBFD_AUTO,
				     NULL,
				     &fbink_config,
				     "[KFMon] PID %ld was killed by signal %d!",
				     (long) cpid,
				     sigcode);
			if (daemon_config.use_syslog) {
				// NOTE: No strsignal means no human-readable interpretation of the signal w/ syslog
				//       (the %m token only works for errno)...
				syslog(LOG_NOTICE, "%s", buf);
			} else {
				psignal(sigcode, buf);
			}
		}
	}

	// And now we can safely remove it from the process table
	pthread_mutex_lock(&ptlock);
	remove_process_from_table(i);
	pthread_mutex_unlock(&ptlock);

	free(ptr);

	return (void*) NULL;
}

// Spawn a process and return its pid...
// Initially inspired from popen2() implementations from https://stackoverflow.com/questions/548063
// As well as the glibc's system() call,
// With a bit of added tracking to handle reaping without a SIGCHLD handler.
static pid_t
    spawn(char* const* command, uint8_t watch_idx)
{
	pid_t pid;

	pid = fork();

	if (pid < 0) {
		// Fork failed?
		perror("[KFMon] [ERR!] Aborting: fork");
		fbink_print(FBFD_AUTO, "[KFMon] fork failed ?!", &fbink_config);
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		// Sweet child o' mine!
		// NOTE: We're multithreaded & forking, this means that from this point on until execve(),
		//       we can only use async-safe functions!
		//       See pthread_atfork(3) for details.
		// Do the whole stdin/stdout/stderr dance again,
		// to ensure that child process doesn't inherit our tweaked fds...
		dup2(orig_stdin, fileno(stdin));
		dup2(orig_stdout, fileno(stdout));
		dup2(orig_stderr, fileno(stderr));
		close(orig_stdin);
		close(orig_stdout);
		close(orig_stderr);
		// Restore signals
		signal(SIGHUP, SIG_DFL);
		// NOTE: We used to use execvpe when being launched from udev,
		//       in order to sanitize all the crap we inherited from udev's env ;).
		//       Now, we actually rely on the specific env we inherit from rcS/on-animator!
		execvp(*command, command);
		// NOTE: This will only ever be reached on error, hence the lack of actual return value check ;).
		//       Resort to an ugly hack by exiting with execvp()'s errno,
		//       which we can then try to salvage in the reaper thread.
		exit(errno);
	} else {
		// Parent
		// Keep track of the process
		int8_t i;
		pthread_mutex_lock(&ptlock);
		i = get_next_available_pt_entry();
		pthread_mutex_unlock(&ptlock);

		if (i < 0) {
			// NOTE: If we ever hit this error codepath,
			//       we don't have to worry about leaving that last spawn as a zombie:
			//       One of the benefits of the double-fork we do to daemonize is that, on our death,
			//       our children will get reparented to init, which, by design,
			//       will handle the reaping automatically.
			LOG(LOG_ERR,
			    "Failed to find an available entry in our process table for pid %ld, aborting!",
			    (long) pid);
			fbink_print(FBFD_AUTO, "[KFMon] Can't spawn any more processes!", &fbink_config);
			exit(EXIT_FAILURE);
		} else {
			pthread_mutex_lock(&ptlock);
			add_process_to_table((uint8_t) i, pid, watch_idx);
			pthread_mutex_unlock(&ptlock);

			DBGLOG("Assigned pid %ld (from watch idx %hhu) to process table entry idx %hhd",
			       (long) pid,
			       watch_idx,
			       i);
			// NOTE: We can't do that from the child proper, because it's not async-safe,
			//       so do it from here.
			LOG(LOG_NOTICE,
			    "Spawned process %ld (%s -> %s @ watch idx %hhu) . . .",
			    (long) pid,
			    watch_config[watch_idx].filename,
			    watch_config[watch_idx].action,
			    watch_idx);
			if (daemon_config.with_notifications) {
				fbink_printf(FBFD_AUTO,
					     NULL,
					     &fbink_config,
					     "[KFMon] Launched %s :)",
					     basename(watch_config[watch_idx].action));
			}
			// NOTE: We achieve reaping in a non-blocking way by doing the reaping from a dedicated thread
			//       for every spawn...
			//       See #2 for an history of the previous failed attempts...
			pthread_t rthread;
			uint8_t*  arg = malloc(sizeof(*arg));
			if (arg == NULL) {
				LOG(LOG_ERR, "Couldn't allocate memory for thread arg, aborting!");
				fbink_print(FBFD_AUTO, "[KFMon] OOM ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}
			*arg = (uint8_t) i;

			// NOTE: We will *never* wait for one of these threads to die from the main thread, so,
			//       start them in detached state
			//       to make sure their resources will be released when they terminate.
			pthread_attr_t attr;
			if (pthread_attr_init(&attr) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_attr_init");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_attr_init failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}
			if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_attr_setdetachstate");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_attr_setdetachstate failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}

			// NOTE: Use a smaller stack (ulimit -s is 8MB on the Kobos).
			//       Base it on pointer size, aiming for 1MB on x64 (meaning 512KB on x86/arm).
			//       Floor it at 512KB to be safe, though.
			//       In the grand scheme of things, this won't really change much ;).
			if (pthread_attr_setstacksize(
				&attr, MAX((1U * 1024U * 1024U) / 2U, (sizeof(void*) * 1024U * 1024U) / 8U)) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_attr_setstacksize");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_attr_setstacksize failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}
			if (pthread_create(&rthread, &attr, reaper_thread, arg) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_create");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_create failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}

			// Prettify the thread's name. Must be <= 15 characters long (i.e., 16 bytes, NULL included).
			char thname[16];
			snprintf(thname, sizeof(thname), "Reaper:%ld", (long) pid);
			if (pthread_setname_np(rthread, thname) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_setname_np");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_setname_np failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}

			if (pthread_attr_destroy(&attr) != 0) {
				perror("[KFMon] [ERR!] Aborting: pthread_attr_destroy");
				fbink_print(FBFD_AUTO, "[KFMon] pthread_attr_destroy failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}
		}
	}

	return pid;
}

// Check if a given inotify watch already has a spawn running
static bool
    is_watch_already_spawned(uint8_t watch_idx)
{
	// Walk our process table to see if the given watch currently has a registered running process
	for (uint8_t i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] == (int8_t) watch_idx) {
			return true;
			// NOTE: Assume everything's peachy,
			//       and we'll never end up with the same watch_idx assigned to multiple indices in the
			//       process table.
		}
	}

	return false;
}

// Check if a watch flagged as a spawn blocker (f.g., KOReader or Plato) is already running
// NOTE: This is mainly to prevent spurious spawns that might be unwittingly caused by their file manager
//       (be it through metadata reading, thumbnails creation, or whatever).
//       Another workaround is of course to kill KFMon as part of their startup process...
static bool
    is_blocker_running(void)
{
	// Walk our process table to identify watches with a currently running process
	for (uint8_t i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] != -1) {
			// Walk the registered watch list to match that currently running watch to its block_spawns flag
			for (uint8_t watch_idx = 0; watch_idx < watch_count; watch_idx++) {
				if (PT.spawn_watchids[i] == (int8_t) watch_idx) {
					if (watch_config[watch_idx].block_spawns) {
						return true;
					}
				}
			}
		}
	}

	// Nothing currently running is a spawn blocker, we're good to go!
	return false;
}

// Return the pid of the spawn of a given inotify watch
static pid_t
    get_spawn_pid_for_watch(uint8_t watch_idx)
{
	for (uint8_t i = 0; i < WATCH_MAX; i++) {
		if (PT.spawn_watchids[i] == (int8_t) watch_idx) {
			return PT.spawn_pids[i];
		}
	}

	return -1;
}

// Read all available inotify events from the file descriptor 'fd'.
static bool
    handle_events(int fd)
{
	// Some systems cannot read integer variables if they are not properly aligned.
	// On other systems, incorrect alignment may decrease performance.
	// Hence, the buffer used for reading from the inotify file descriptor
	// should have the same alignment as struct inotify_event.
	char                        buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event* event;
	bool                        destroyed_wd       = false;
	bool                        was_unmounted      = false;
	static bool                 pending_processing = false;

	// Loop while events can be read from inotify file descriptor.
	for (;;) {
		// Read some events.
		ssize_t len = read(fd, buf, sizeof(buf));    // Flawfinder: ignore
		if (len == -1 && errno != EAGAIN) {
			perror("[KFMon] [ERR!] Aborting: read");
			fbink_print(FBFD_AUTO, "[KFMon] read failed ?!", &fbink_config);
			exit(EXIT_FAILURE);
		}

		// If the nonblocking read() found no events to read,
		// then it returns -1 with errno set to EAGAIN.
		// In that case, we exit the loop.
		if (len <= 0) {
			break;
		}

		// Loop over all events in the buffer
		for (char* ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
			// NOTE: This trips -Wcast-align on ARM, but should be safe nonetheless ;).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
			event = (const struct inotify_event*) ptr;
#pragma GCC diagnostic pop
			// NOTE: This *may* be a viable alternative, but don't hold me to that.
			// memcpy(&event, &ptr, sizeof(struct inotify_event *));

			// Identify which of our target file we've caught an event for...
			uint8_t watch_idx       = 0;
			bool    found_watch_idx = false;
			for (watch_idx = 0; watch_idx < watch_count; watch_idx++) {
				if (watch_config[watch_idx].inotify_wd == event->wd) {
					found_watch_idx = true;
					break;
				}
			}
			if (!found_watch_idx) {
				// NOTE: Err, that should (hopefully) never happen!
				LOG(LOG_CRIT,
				    "!! Failed to match the current inotify event to any of our watched file! !!");
			}

			// NOTE: Now that, hopefully, we're pretty sure Nickel is up or on its way up,
			//       and has finished or will soon finish setting up the fb,
			//       we can reinit FBInk to have up to date information...
			//       Put everything behind our mutex to be super-safe,
			//       since we're playing with library globals...
			//       We do this the least amount of times possible,
			//       (i.e., once, if every watch has already been processed),
			//       to keep locking to a minimum.
			// NOTE: But, we may do it more than once, in fact, we'll re-init on each new event
			//       until we get a framebuffer state that is no longer quirky
			//       (i.e., once we're sure we got it from Nickel, and not pickel).
			//       This is needed because processing is done very early by Nickel for "new" icons when
			//       they end up on the Home screen straight away,
			//       (which is a given if you added at most 3 items, with the new Home screen).
			//       It's problematic for us, because it's early enough that pickel is still running,
			//       so we inherit its quirky fb setup and not Nickel's...
			pthread_mutex_lock(&ptlock);
			// NOTE: It went fine once, assume that'll still be the case and skip error checking...
			fbink_reinit(FBFD_AUTO, &fbink_config);
			pthread_mutex_unlock(&ptlock);

			// Print event type
			if (event->mask & IN_OPEN) {
				LOG(LOG_NOTICE, "Tripped IN_OPEN for %s", watch_config[watch_idx].filename);
				// Clunky detection of potential Nickel processing...
				bool is_watch_spawned;
				bool is_reader_spawned;
				pthread_mutex_lock(&ptlock);
				is_watch_spawned  = is_watch_already_spawned(watch_idx);
				is_reader_spawned = is_blocker_running();
				pthread_mutex_unlock(&ptlock);

				if (!is_watch_spawned && !is_reader_spawned) {
					// Only check if we're ready to spawn something...
					if (!is_target_processed(watch_idx, false)) {
						// It's not processed on OPEN, flag as pending...
						pending_processing = true;
						LOG(LOG_INFO,
						    "Flagged target icon '%s' as pending processing ...",
						    watch_config[watch_idx].filename);
					} else {
						// It's already processed, we're good!
						pending_processing = false;
					}
				}
			}
			if (event->mask & IN_CLOSE) {
				LOG(LOG_NOTICE, "Tripped IN_CLOSE for %s", watch_config[watch_idx].filename);
				// NOTE: Make sure we won't run a specific command multiple times
				//       while an earlier instance of it is still running...
				//       This is mostly of interest for KOReader/Plato:
				//       it means we can keep KFMon running while they're up,
				//       without risking trying to spawn multiple instances of them,
				//       in case they end up tripping their own inotify watch ;).
				bool is_watch_spawned;
				bool is_reader_spawned;
				pthread_mutex_lock(&ptlock);
				is_watch_spawned  = is_watch_already_spawned(watch_idx);
				is_reader_spawned = is_blocker_running();
				pthread_mutex_unlock(&ptlock);

				if (!is_watch_spawned && !is_reader_spawned) {
					// Check that our target file has already fully been processed by Nickel
					// before launching anything...
					if (!pending_processing && is_target_processed(watch_idx, true)) {
						LOG(LOG_INFO,
						    "Preparing to spawn %s for watch idx %hhu . . .",
						    watch_config[watch_idx].action,
						    watch_idx);
						if (watch_config[watch_idx].block_spawns) {
							LOG(LOG_NOTICE,
							    "%s is flagged as a spawn blocker, it will prevent *any* event from triggering a spawn while it is still running!",
							    watch_config[watch_idx].action);
						}
						// We're using execvp()...
						char* const cmd[] = { watch_config[watch_idx].action, NULL };
						spawn(cmd, watch_idx);
					} else {
						LOG(LOG_NOTICE,
						    "Target icon '%s' might not have been fully processed by Nickel yet, don't launch anything.",
						    watch_config[watch_idx].filename);
						fbink_printf(FBFD_AUTO,
							     NULL,
							     &fbink_config,
							     "[KFMon] Not spawning %s: still processing!",
							     basename(watch_config[watch_idx].action));
						// NOTE: That, or we hit a SQLITE_BUSY timeout on OPEN,
						//       which tripped our 'pending processing' check.
					}
				} else {
					if (is_watch_spawned) {
						pid_t spid;
						pthread_mutex_lock(&ptlock);
						spid = get_spawn_pid_for_watch(watch_idx);
						pthread_mutex_unlock(&ptlock);

						LOG(LOG_INFO,
						    "As watch idx %hhu (%s) still has a spawned process (%ld -> %s) running, we won't be spawning another instance of it!",
						    watch_idx,
						    watch_config[watch_idx].filename,
						    (long) spid,
						    watch_config[watch_idx].action);
						fbink_printf(FBFD_AUTO,
							     NULL,
							     &fbink_config,
							     "[KFMon] Not spawning %s: still running!",
							     basename(watch_config[watch_idx].action));
					} else if (is_reader_spawned) {
						LOG(LOG_INFO,
						    "As a spawn blocker process is currently running, we won't be spawning anything else to prevent unwanted behavior!");
						fbink_printf(FBFD_AUTO,
							     NULL,
							     &fbink_config,
							     "[KFMon] Not spawning %s: blocked!",
							     basename(watch_config[watch_idx].action));
					}
				}
			}
			if (event->mask & IN_UNMOUNT) {
				LOG(LOG_NOTICE, "Tripped IN_UNMOUNT for %s", watch_config[watch_idx].filename);
				// Remember that we encountered an unmount,
				// so we don't try to manually remove watches that are already gone...
				was_unmounted = true;
			}
			// NOTE: Something (badly coalesced/ordered events?) is a bit wonky on the Kobos
			//       when onboard gets unmounted:
			//       we actually never get an IN_UNMOUNT event, only IN_IGNORED...
			//       Another strange behavior is that we get them in a staggered mannered,
			//       and not in one batch, as I do on my sandbox when unmounting a tmpfs...
			//       That may explain why the explicit inotify_rm_watch() calls we do later
			//       on all our other watches don't seem to error out...
			//       In the end, we behave properly, but it's still strange enough to document ;).
			if (event->mask & IN_IGNORED) {
				LOG(LOG_NOTICE, "Tripped IN_IGNORED for %s", watch_config[watch_idx].filename);
				// Remember that the watch was automatically destroyed so we can break from the loop...
				destroyed_wd                             = true;
				watch_config[watch_idx].wd_was_destroyed = true;
			}
			if (event->mask & IN_Q_OVERFLOW) {
				if (event->len) {
					LOG(LOG_WARNING, "Huh oh... Tripped IN_Q_OVERFLOW for %s", event->name);
				} else {
					LOG(LOG_WARNING, "Huh oh... Tripped IN_Q_OVERFLOW for... something?");
				}
				// Try to remove the inotify watch we matched
				// (... hoping matching actually was successful), and break the loop.
				LOG(LOG_INFO,
				    "Trying to remove inotify watch for '%s' @ index %hhu.",
				    watch_config[watch_idx].filename,
				    watch_idx);
				if (inotify_rm_watch(fd, watch_config[watch_idx].inotify_wd) == -1) {
					// That's too bad, but may not be fatal, so warn only...
					perror("[KFMon] [WARN] inotify_rm_watch");
				} else {
					// Flag it as gone if rm was successful
					watch_config[watch_idx].inotify_wd = -1;
				}
				destroyed_wd                             = true;
				watch_config[watch_idx].wd_was_destroyed = true;
			}
		}

		// If we caught an unmount, explain why we don't explictly have to tear down our watches
		if (was_unmounted) {
			LOG(LOG_INFO, "Unmount detected, nothing to do, all watches will naturally get destroyed.");
		}
		// If we caught an event indicating that a watch was automatically destroyed, break the loop.
		if (destroyed_wd) {
			// But before we do that, make sure we've removed *all* our *other* watches first
			// (again, hoping matching was successful), since we'll be setting them up all again later...
			for (uint8_t watch_idx = 0; watch_idx < watch_count; watch_idx++) {
				if (!watch_config[watch_idx].wd_was_destroyed) {
					// Don't do anything if that was because of an unmount...
					// Because that assures us that everything is/will soon be gone
					// (since by design, all our target files live on the same mountpoint),
					// even if we didn't get to parse all the events in one go
					// to flag them as destroyed one by one.
					if (!was_unmounted) {
						// Check if that watch index is active to begin with,
						// as we might have just skipped it if its target file was missing...
						if (watch_config[watch_idx].inotify_wd == -1) {
							LOG(LOG_INFO,
							    "Inotify watch for '%s' @ index %hhu is already inactive!",
							    watch_config[watch_idx].filename,
							    watch_idx);
						} else {
							// Log what we're doing...
							LOG(LOG_INFO,
							    "Trying to remove inotify watch for '%s' @ index %hhu.",
							    watch_config[watch_idx].filename,
							    watch_idx);
							if (inotify_rm_watch(fd, watch_config[watch_idx].inotify_wd) ==
							    -1) {
								// That's too bad, but may not be fatal, so warn only...
								perror("[KFMon] [WARN] inotify_rm_watch");
							} else {
								// It's gone!
								watch_config[watch_idx].inotify_wd = -1;
							}
						}
					}
				} else {
					// Reset the flag to avoid false-positives on the next iteration of the loop,
					// since we re-use the array's content.
					watch_config[watch_idx].wd_was_destroyed = false;
				}
			}
			break;
		}
	}

	// And we have another outer loop to break, so pass that on...
	return destroyed_wd;
}

int
    main(int argc __attribute__((unused)), char* argv[] __attribute__((unused)))
{
	int           fd;
	int           poll_num;
	struct pollfd pfd;

	// Make sure we're running at a neutral niceness
	// (f.g., being launched via udev would leave us with a negative nice value).
	if (setpriority(PRIO_PROCESS, 0, 0) == -1) {
		perror("[KFMon] [ERR!] Aborting: setpriority");
		exit(EXIT_FAILURE);
	}

	// Fly, little daemon!
	if (daemonize() != 0) {
		fprintf(stderr, "Failed to daemonize, aborting!\n");
		exit(EXIT_FAILURE);
	}

	// Say hello :)
	LOG(LOG_INFO,
	    "[PID: %ld] Initializing KFMon %s | Built on %s @ %s | Using SQLite %s (built against version %s) | With FBInk %s",
	    (long) getpid(),
	    KFMON_VERSION,
	    __DATE__,
	    __TIME__,
	    sqlite3_libversion(),
	    SQLITE_VERSION,
	    fbink_version());

	// Load our configs
	if (load_config() == -1) {
		LOG(LOG_ERR, "Failed to load one or more config files, aborting!");
		exit(EXIT_FAILURE);
	}

	// Squish stderr if we want to log to the syslog...
	// (can't do that w/ the rest in daemonize, since we don't have our config yet at that point)
	if (daemon_config.use_syslog) {
		// Redirect stderr (which is now actually our log file) to /dev/null
		if ((fd = open("/dev/null", O_RDWR)) != -1) {
			dup2(fd, fileno(stderr));
			if (fd > 2 + 3) {
				close(fd);
			}
		} else {
			fprintf(stderr, "Failed to redirect stderr to /dev/null\n");
			return -1;
		}

		// And connect to the system logger...
		openlog("kfmon", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_DAEMON);
	}

	// Initialize the process table, to track our spawns
	init_process_table();

	// Initialize FBInk
	init_fbink_config();
	// Consider not being able to print on screen a hard pass...
	// (Mostly, it's to avoid blowing up later in fbink_print).
	if (fbink_init(FBFD_AUTO, &fbink_config) != EXIT_SUCCESS) {
		LOG(LOG_ERR, "Failed to initialize FBInk, aborting!");
		exit(EXIT_FAILURE);
	}

	// NOTE: Because of course we can't have nice things, at this point,
	//       Nickel hasn't finished setting up the fb to its liking. To be fair, it hasn't even started yet ;).
	//       On most devices, the fb is probably in a weird rotation and/or bitdepth at this point.
	//       This has two downsides:
	//       this message (as well as a few others in error paths that might trigger before our first inotify event)
	//       may be slightly broken (meaning badly rotated or positioned),
	//       although FBInk should now mitigate most, if not all, aspects of this particular issue.
	//       But more annoyingly: this means we need to later make sure we have up to date fb info,
	//       an issue we handle via fbink_reinit's heuristics ;).
	//       Thankfully, in most cases, stale info will mostly just mess with positioning,
	//       while completely broken info would only cause the MXCFB ioctl to fail, we wouldn't segfault.
	//       (Well, to be perfectly fair, it'd take an utterly broken finfo.smem_len to crash,
	//       and that should never happen).
	// NOTE: To get up to date info, we'll reinit on each new inotify event we catch,
	//       until we get something we can keep (i.e., Nickel's fb setup),
	//       at which point we'll stop doing those extra init calls,
	//       because we assume no-one will mess with it again (and no-one should).
	if (daemon_config.with_notifications) {
		fbink_print(FBFD_AUTO, "[KFMon] Successfully initialized. :)", &fbink_config);
	}

	// We pretty much want to loop forever...
	while (1) {
		LOG(LOG_INFO, "Beginning the main loop.");

		// Create the file descriptor for accessing the inotify API
		LOG(LOG_INFO, "Initializing inotify.");
		fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (fd == -1) {
			perror("[KFMon] [ERR!] Aborting: inotify_init1");
			fbink_print(FBFD_AUTO, "[KFMon] Failed to initialize inotify!", &fbink_config);
			exit(EXIT_FAILURE);
		}

		// Make sure our target file is available (i.e., the partition it resides in is mounted)
		if (!is_target_mounted()) {
			LOG(LOG_INFO, "%s isn't mounted, waiting for it to be . . .", KFMON_TARGET_MOUNTPOINT);
			// If it's not, wait for it to be...
			wait_for_target_mountpoint();
		}

		// Flag each of our target files for 'file was opened' and 'file was closed' events
		// NOTE: We don't check for:
		//       IN_MODIFY: Highly unlikely (and sandwiched between an OPEN and a CLOSE anyway)
		//       IN_CREATE: Only applies to directories
		//       IN_DELETE: Only applies to directories
		//       IN_DELETE_SELF: Will trigger an IN_IGNORED, which we already handle
		//       IN_MOVE_SELF: Highly unlikely on a Kobo, and somewhat annoying to handle with our design
		//           (we'd have to forget about it entirely and not try to re-watch for it
		//           on the next iteration of the loop).
		// NOTE: inotify tracks the file's inode, which means that it goes *through* bind mounts, for instance:
		//           When bind-mounting file 'a' to file 'b', and setting up a watch to the path of file 'b',
		//           you won't get *any* event on that watch when unmounting that bind mount, since the original
		//           file 'a' hasn't actually been touched, and, as it is the actual, real file,
		//           that is what inotify is actually tracking.
		//       Relative to the earlier IN_MOVE_SELF mention, that means it'll keep tracking the file with its
		//           new name (provided it was moved to the *same* fs,
		//           as crossing a fs boundary will delete the original).
		for (uint8_t watch_idx = 0; watch_idx < watch_count; watch_idx++) {
			watch_config[watch_idx].inotify_wd =
			    inotify_add_watch(fd, watch_config[watch_idx].filename, IN_OPEN | IN_CLOSE);
			if (watch_config[watch_idx].inotify_wd == -1) {
				perror("[KFMon] [WARN] inotify_add_watch");
				LOG(LOG_WARNING, "Cannot watch '%s', discarding it!", watch_config[watch_idx].filename);
				fbink_printf(FBFD_AUTO,
					     NULL,
					     &fbink_config,
					     "[KFMon] Failed to watch %s!",
					     basename(watch_config[watch_idx].filename));
				// NOTE: We used to abort entirely in case even one target file couldn't be watched,
				//       but that was a bit harsh ;).
				//       Since the inotify watch couldn't be setup,
				//       there's no way for this to cause trouble down the road,
				//       and this allows the user to fix it during an USBMS session instead of having to reboot,
				//       provided no config tweaks are needed (as we still only parse configs at boot)...
			} else {
				LOG(LOG_NOTICE,
				    "Setup an inotify watch for '%s' @ index %hhu.",
				    watch_config[watch_idx].filename,
				    watch_idx);
			}
		}

		// Inotify input
		pfd.fd     = fd;
		pfd.events = POLLIN;

		// Wait for events
		LOG(LOG_INFO, "Listening for events.");
		while (1) {
			poll_num = poll(&pfd, 1, -1);
			if (poll_num == -1) {
				if (errno == EINTR) {
					continue;
				}
				perror("[KFMon] [ERR!] Aborting: poll");
				fbink_print(FBFD_AUTO, "[KFMon] poll failed ?!", &fbink_config);
				exit(EXIT_FAILURE);
			}

			if (poll_num > 0) {
				if (pfd.revents & POLLIN) {
					// Inotify events are available
					if (handle_events(fd)) {
						// Go back to the main loop if we exited early (because a watch was
						// destroyed automatically after an unmount or an unlink, for instance)
						break;
					}
				}
			}
		}
		LOG(LOG_INFO, "Stopped listening for events.");

		// Close inotify file descriptor
		close(fd);
	}

	// Why, yes, this is unreachable! Good thing it's also optional ;).
	if (daemon_config.use_syslog) {
		closelog();
	}

	exit(EXIT_SUCCESS);
}
