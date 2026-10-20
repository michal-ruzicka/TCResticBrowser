#pragma once
#include <windows.h>
#include <stddef.h>

/* Configuration file name and default binary name */
#define CONFIG_INI_FILENAME       L"TCResticBrowser.ini"
#define CONFIG_USER_SUBDIR        L"GHISLER"   /* Total Commander's settings folder under %APPDATA% */
#define CONFIG_DEFAULT_RESTIC_EXE L"restic.exe"
#define CONFIG_DEFAULT_ROOT_NAME  L"TCResticBrowser"  /* Network Neighborhood name without [Settings] RootName */
#define CONFIG_DEFAULT_PASSWORD_TIMEOUT_S 1800u  /* [Settings] PasswordTimeoutSeconds default: 30 min */
#define CONFIG_MAX_PATH           1024
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
    WCHAR restic_path[CONFIG_MAX_PATH];  /* Resolved path or command for restic.exe */
    WCHAR root_name[MAX_PATH];           /* [Settings] RootName, or CONFIG_DEFAULT_ROOT_NAME */
    WCHAR log_path[CONFIG_MAX_PATH];     /* [Logging] LogPath as written (unexpanded); empty = off */
    DWORD password_timeout_s;            /* [Settings] PasswordTimeoutSeconds; 0 = keep until exit */
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
 * Resolves the Restic CLI executable path in priority order:
 * 1. [Settings] ResticPath in ini_path (if set to an existing file or custom command).
 * 2. <plugin_dir>\restic.exe (if present in the plugin directory).
 * 3. Binary located in the system PATH environment variable (fallback to "restic.exe").
 *
 * Returns TRUE if the binary was verified on disk or in PATH, FALSE if falling back to bare "restic.exe".
 */
BOOL config_resolve_restic_path(const WCHAR* ini_path, WCHAR* out_path, size_t out_capacity);

/**
 * Loads configuration and repository list from a specific INI file path. root_name is always
 * filled: with a valid [Settings] RootName, otherwise (also when the file is missing) with
 * CONFIG_DEFAULT_ROOT_NAME. password_timeout_s is [Settings] PasswordTimeoutSeconds (a decimal
 * number of seconds, 0 = no timeout), or CONFIG_DEFAULT_PASSWORD_TIMEOUT_S when it is missing or
 * not such a number.
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
