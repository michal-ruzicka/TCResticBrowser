#pragma once
#include <windows.h>
#include "snapshot_tree.h"
#include <stddef.h>

/* Configuration file name and default binary name */
#define CONFIG_INI_FILENAME       L"TCResticBrowser.ini"
#define CONFIG_USER_SUBDIR        L"GHISLER"   /* Total Commander's settings folder under %APPDATA% */
#define CONFIG_DEFAULT_RESTIC_EXE L"restic.exe"
#define CONFIG_DEFAULT_ROOT_NAME  L"TCResticBrowser"  /* Network Neighborhood name without [Settings] RootName */
#define CONFIG_DEFAULT_PASSWORD_TIMEOUT_S 1800u  /* [Settings] PasswordTimeoutSeconds default: 30 min */
#define CONFIG_DEFAULT_SNAPSHOT_CACHE_MB  512u   /* [Settings] SnapshotCacheMB default */
#define CONFIG_DEFAULT_SNAPSHOT_LIST_REFRESH_S 60u /* [Settings] SnapshotListRefreshSeconds: restic mount's interval */
#define CONFIG_MAX_PATH           1024
#define CONFIG_MAX_REPO_ENV       4096  /* ResticRepoEntry.environment, in characters */
#define CONFIG_MAX_REPO_OPTIONS   2048  /* ResticRepoEntry.options, in characters */
#define CONFIG_ERROR_CHARS        512   /* ResticConfig.restic_path_error */
#define CONFIG_MAX_INI_BYTES      (4 * 1024 * 1024)  /* A larger TCResticBrowser.ini is not read */

/* The section with a repository's own settings: "[Repository: <alias>]" */
#define CONFIG_REPOSITORY_SECTION_PREFIX L"Repository:"

/**
 * Single repository mapping under [Repositories] in TCResticBrowser.ini, with the settings of its
 * [Repository: <alias>] section. Those are empty when not set, otherwise ready to be passed to
 * restic (ResticRepo, restic_cli.h): paths resolved and checked, PasswordCommand as restic splits it.
 */
typedef struct {
    WCHAR alias[MAX_PATH];          /* Virtual folder name displayed at plugin root "\" */
    WCHAR target[CONFIG_MAX_PATH];  /* Restic repository URI or filesystem path */
    WCHAR password_file[CONFIG_MAX_PATH];     /* PasswordFile: RESTIC_PASSWORD_FILE, full path */
    WCHAR password_command[CONFIG_MAX_PATH];  /* PasswordCommand: RESTIC_PASSWORD_COMMAND */
    /* The S3 environment variables of the section, "NAME=value\0...\0\0" (empty: "\0") */
    WCHAR environment[CONFIG_MAX_REPO_ENV];
    /* The section's restic options (-o), "backend.option=value\0...\0\0" (empty: "\0") */
    WCHAR options[CONFIG_MAX_REPO_OPTIONS];
    WCHAR settings_error[CONFIG_ERROR_CHARS]; /* Why a setting of the section cannot be used; the
                                                 repository is then not opened. Empty when fine */
} ResticRepoEntry;

/**
 * Complete parsed plugin configuration.
 */
typedef struct {
    WCHAR ini_path[CONFIG_MAX_PATH];     /* Full path to the active TCResticBrowser.ini */
    WCHAR restic_path[CONFIG_MAX_PATH];  /* Resolved full path of restic.exe; bare name if not found */
    WCHAR restic_path_error[CONFIG_ERROR_CHARS]; /* Why restic.exe was not found; empty if it was */
    WCHAR root_name[MAX_PATH];           /* [Settings] RootName, or CONFIG_DEFAULT_ROOT_NAME */
    WCHAR log_path[CONFIG_MAX_PATH];     /* [Logging] LogPath as written (unexpanded); empty = off */
    DWORD password_timeout_s;            /* [Settings] PasswordTimeoutSeconds; 0 = keep until exit */
    BOOL password_timeout_reset_on_use;  /* [Settings] PasswordTimeoutResetOnUse (default FALSE) */
    DWORD snapshot_cache_mb;             /* [Settings] SnapshotCacheMB: whole-snapshot listings; 0 = off */
    DWORD snapshot_list_refresh_s;       /* [Settings] SnapshotListRefreshSeconds: age that triggers a re-read */
    BOOL connection_log;                 /* [Settings] ConnectionLog: Total Commander's connections toolbar (default TRUE) */
    char csv_separator;                  /* [Settings] CsvSeparator: , ; | or tab; 0 = the Windows list separator */
    char snapshot_time_format[SNAPSHOT_TREE_TIME_FORMAT_MAX + 1]; /* [Settings] SnapshotTimeFormat, UTF-8, always valid */
    BOOL snapshot_time_format_invalid;   /* The INI had an unusable SnapshotTimeFormat (default used) */
    ResticRepoEntry* repos;              /* Heap-allocated array of repository entries */
    size_t repo_count;                   /* Number of repositories in repos */
} ResticConfig;

/**
 * Sets the plugin directory explicitly (used by DllMain or unit tests).
 */
void config_set_plugin_dir(const WCHAR* dir_path);

/**
 * Resolves and stores the directory containing the given DLL module handle.
 */
void config_set_plugin_dir_from_module(HMODULE hModule);

/**
 * Returns the currently configured plugin directory (empty string if unset).
 */
const WCHAR* config_get_plugin_dir(void);

/**
 * Builds the per-user INI path %APPDATA%\GHISLER\TCResticBrowser.ini (whether or not it exists).
 * Returns FALSE when %APPDATA% is not set.
 */
BOOL config_get_user_ini_path(WCHAR* out_path, size_t out_capacity);

/**
 * Resolves the active TCResticBrowser.ini. The first existing file is used in full (no merging):
 * 1. %APPDATA%\GHISLER\TCResticBrowser.ini - per-user override.
 * 2. <plugin_dir>\TCResticBrowser.ini - system-wide file shipped with the plugin.
 *
 * Returns TRUE if an existing INI file was found at out_path, or FALSE when neither file exists
 * (in which case out_path receives the plugin-directory path, if known).
 */
BOOL config_resolve_ini_path(WCHAR* out_path, size_t out_capacity);

/**
 * Resolves the full path of the Restic CLI executable.
 *
 * A non-empty [Settings] ResticPath in ini_path (first occurrence) is used or nothing is: a full
 * path must name an existing file, a relative one ("tools\restic.exe") is taken from the plugin
 * directory, and a bare name ("restic-custom", ".exe" added when there is no extension) is looked up
 * in the plugin directory, then in PATH. Drive- or root-relative values ("C:restic.exe",
 * "\tools\restic.exe") and over-long ones are refused. Nothing else is tried when the configured
 * binary is missing, so the user never runs a different restic.exe without knowing it.
 *
 * Without ResticPath: <plugin_dir>\restic.exe, then restic.exe in the absolute PATH entries. The
 * current directory is never searched.
 *
 * Returns TRUE with the full path in out_path. Otherwise out_path receives the bare "restic.exe"
 * (which restic_exec_command refuses to run) and `error` (optional) a message for the user that
 * names the setting, the INI file and the path that was tried.
 */
BOOL config_resolve_restic_path(
    const WCHAR* ini_path,
    WCHAR* out_path,
    size_t out_capacity,
    WCHAR* error,
    size_t error_capacity
);

/*
 * TRUE for "X:\..." and "\\server\..." (also "\\?\..."): a path that does not depend on the
 * current drive or directory. Relative, drive-relative ("C:x") and root-relative ("\x") paths
 * would follow the Total Commander panel and are never used for executables or files.
 */
BOOL config_is_absolute_path(const WCHAR* p);

/*
 * TRUE when Windows stores a file under exactly the last component of `path` (both separators
 * count). Names from non-Windows backups may be legal there but not here: "a:b" would write the
 * alternate data stream "b" of the file "a", "CON", "NUL.txt" or "COM1" a device, "a." or "a "
 * the file "a", and <>"|?* or control characters make Win32 fail. All of them are refused.
 */
BOOL config_is_plain_file_name(const WCHAR* path);

/**
 * Loads configuration and repository list from a specific INI file path. root_name is always
 * filled: with a valid [Settings] RootName, otherwise (also when the file is missing) with
 * CONFIG_DEFAULT_ROOT_NAME. password_timeout_s is [Settings] PasswordTimeoutSeconds (a decimal
 * number of seconds, 0 = no timeout), or CONFIG_DEFAULT_PASSWORD_TIMEOUT_S when it is missing or
 * not such a number. password_timeout_reset_on_use is [Settings] PasswordTimeoutResetOnUse
 * (yes/no, true/false, on/off, 1/0; FALSE when missing or anything else). snapshot_cache_mb is
 * [Settings] SnapshotCacheMB (a decimal number of MiB, 0 = no whole-snapshot listings), or
 * CONFIG_DEFAULT_SNAPSHOT_CACHE_MB when it is missing or not such a number. snapshot_time_format
 * is [Settings] SnapshotTimeFormat when snapshot_tree_time_format_valid() accepts it, otherwise
 * SNAPSHOT_TREE_DEFAULT_TIME_FORMAT (and snapshot_time_format_invalid is set if one was given).
 * snapshot_list_refresh_s is [Settings] SnapshotListRefreshSeconds (decimal seconds), or
 * CONFIG_DEFAULT_SNAPSHOT_LIST_REFRESH_S when it is missing or not such a number. connection_log is
 * [Settings] ConnectionLog: FALSE for no/false/off/0 (any case), otherwise TRUE, also when missing.
 * csv_separator is [Settings] CsvSeparator: ",", ";", "|" or "tab" (any case); 0 when missing or
 * anything else, which means the list separator of the user's Windows regional settings.
 * Caller must release allocated resources via config_free().
 */
BOOL config_load_from_file(const WCHAR* ini_path, ResticConfig* out_config);

/**
 * Resolves the active INI file path and loads it, and applies its [Logging] LogPath to the
 * diagnostic log (log_configure), so logging follows the INI without a restart.
 * Caller must release allocated resources via config_free().
 */
BOOL config_load(ResticConfig* out_config);

/*
 * [Repository: <alias>] sections (CONFIG_REPOSITORY_SECTION_PREFIX, the alias as in [Repositories],
 * compared ignoring case) hold settings of one repository; config_load_from_file() applies them to
 * the repository of that alias, the first occurrence of each key winning, also across repeated
 * sections. Every key is named as restic names it. Keys:
 * - PasswordFile: a file restic reads the password from (RESTIC_PASSWORD_FILE); like ResticPath a
 *   full path or one relative to the plugin folder, unquoted, and it must exist.
 * - PasswordCommand: a program restic runs to get the password (RESTIC_PASSWORD_COMMAND). Without
 *   quote characters the whole value is the program's path, resolved like ResticPath (a bare name
 *   in the plugin folder, then in PATH) and passed in double quotes. A value with quotes is a
 *   command line as restic splits it (backend.SplitShellStrings): arguments separated by spaces,
 *   "..." or '...' grouping, and a backslash outside quotes also separating, which would cut a
 *   Windows path apart; such a value is refused, as are unbalanced quotes and a program that is
 *   neither a full path nor a bare name.
 * - The S3 environment variables restic documents (doc/075_scripting.rst) except the secret ones:
 *   AWS_ACCESS_KEY_ID, AWS_DEFAULT_REGION, AWS_PROFILE, AWS_SHARED_CREDENTIALS_FILE (a file,
 *   resolved like PasswordFile) and RESTIC_AWS_ASSUME_ROLE_ARN, _SESSION_NAME, _EXTERNAL_ID,
 *   _POLICY, _REGION, _STS_ENDPOINT, kept in `environment`. With AWS_ACCESS_KEY_ID the plugin asks
 *   for AWS_SECRET_ACCESS_KEY; secrets (it and AWS_SESSION_TOKEN) are never read from the INI.
 * - Any "<backend>.<option>" key (letters, digits and '-' around one dot, e.g. s3.region,
 *   s3.bucket-lookup, sftp.command): an extended option, passed as restic -o <key>=<value> and
 *   checked by restic itself. The key is lowercased, as restic does; quotes around the value are
 *   removed unless the value has more of them inside. The four options restic splits into a
 *   command line itself follow the PasswordCommand rules instead: rclone.program and sftp.command
 *   (a plain path, or a command line with quotes, kept as written), rclone.args and sftp.args
 *   (arguments only, kept as written, refused with a backslash outside quotes).
 * PasswordFile and PasswordCommand may both be set: restic gets both and decides (it refuses the
 * combination). An unusable value sets the entry's settings_error; unknown keys, empty values and
 * sections for an alias that is not configured are ignored.
 */

/* The value of variable `name` in a repository's `environment`, or NULL when it is not set */
const WCHAR* config_repo_env_value(const ResticRepoEntry* repo, const WCHAR* name);

/**
 * Finds a repository entry by its alias (case-insensitive).
 * Returns NULL if not found.
 */
const ResticRepoEntry* config_find_repo(const ResticConfig* config, const WCHAR* alias);

/**
 * Frees dynamically allocated repository entries in config and zeroes the structure.
 */
void config_free(ResticConfig* config);
