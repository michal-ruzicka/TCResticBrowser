#pragma once
#include <windows.h>
#include <stddef.h>

/* Configuration file name and default binary name */
#define CONFIG_INI_FILENAME       L"restic_plugin.ini"
#define CONFIG_DEFAULT_RESTIC_EXE L"restic.exe"
#define CONFIG_MAX_PATH           1024

/**
 * Single repository mapping under [Repositories] in restic_plugin.ini.
 */
typedef struct {
    WCHAR alias[MAX_PATH];          /* Virtual folder name displayed at plugin root "\" */
    WCHAR target[CONFIG_MAX_PATH];  /* Restic repository URI or filesystem path */
} ResticRepoEntry;

/**
 * Complete parsed plugin configuration.
 */
typedef struct {
    WCHAR ini_path[CONFIG_MAX_PATH];     /* Full path to the active restic_plugin.ini */
    WCHAR restic_path[CONFIG_MAX_PATH];  /* Resolved path or command for restic.exe */
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
 * Sets the fallback configuration directory explicitly.
 */
void config_set_fallback_dir(const WCHAR* dir_path);

/**
 * Extracts and stores the fallback directory from Total Commander's DefaultIniName (ANSI).
 */
void config_set_fallback_from_default_params(const char* default_ini_name);

/**
 * Returns the currently configured fallback directory (empty string if unset).
 */
const WCHAR* config_get_fallback_dir(void);

/**
 * Resolves the path to restic_plugin.ini:
 * 1. Primary: <plugin_dir>\restic_plugin.ini (if existing).
 * 2. Fallback: <fallback_dir>\restic_plugin.ini (if existing).
 *
 * Returns TRUE if an existing INI file was found at out_path, or FALSE when neither file exists
 * (in which case out_path receives the preferred candidate path).
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
 * Loads configuration and repository list from a specific INI file path.
 * Caller must release allocated resources via config_free().
 */
BOOL config_load_from_file(const WCHAR* ini_path, ResticConfig* out_config);

/**
 * Resolves the active INI file path and loads it.
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
