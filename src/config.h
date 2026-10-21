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
#define CONFIG_ERROR_CHARS        512   /* ResticConfig.restic_path_error */
#define CONFIG_MAX_INI_BYTES      (4 * 1024 * 1024)  /* A larger TCResticBrowser.ini is not read */

/**
 * Single repository mapping under [Repositories] in TCResticBrowser.ini.
 */
typedef struct {
    WCHAR alias[MAX_PATH];          /* Virtual folder name displayed at plugin root "\" */
    WCHAR target[CONFIG_MAX_PATH];  /* Restic repository URI or filesystem path */
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
 * CONFIG_DEFAULT_SNAPSHOT_LIST_REFRESH_S when it is missing or not such a number.
 * Caller must release allocated resources via config_free().
 */
BOOL config_load_from_file(const WCHAR* ini_path, ResticConfig* out_config);

/**
 * Resolves the active INI file path and loads it, and applies its [Logging] LogPath to the
 * diagnostic log (log_configure), so logging follows the INI without a restart.
 * Caller must release allocated resources via config_free().
 */
BOOL config_load(ResticConfig* out_config);

/**
 * Finds a repository entry by its alias (case-insensitive).
 * Returns NULL if not found.
 */
const ResticRepoEntry* config_find_repo(const ResticConfig* config, const WCHAR* alias);

/**
 * Frees dynamically allocated repository entries in config and zeroes the structure.
 */
void config_free(ResticConfig* config);
