#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "config.h"
#include "log.h"
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

/* Module-level directory state initialized at plugin load time */
static WCHAR g_plugin_dir[CONFIG_MAX_PATH] = {0};

static BOOL is_space_char(WCHAR c) {
    return (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
}

/* Trims leading and trailing whitespace in-place */
static void trim_whitespace(WCHAR* str) {
    if (!str || str[0] == L'\0') return;

    WCHAR* start = str;
    while (*start && is_space_char(*start)) {
        start++;
    }

    size_t len = wcslen(start);
    while (len > 0 && is_space_char(start[len - 1])) {
        len--;
    }

    if (start != str && len > 0) {
        wmemmove(str, start, len);
    }
    str[len] = L'\0';
}

/* Removes surrounding double or single quotes in-place */
static void strip_quotes(WCHAR* str) {
    if (!str) return;
    size_t len = wcslen(str);
    if (len >= 2) {
        WCHAR first = str[0];
        WCHAR last  = str[len - 1];
        if ((first == L'"' && last == L'"') || (first == L'\'' && last == L'\'')) {
            wmemmove(str, str + 1, len - 2);
            str[len - 2] = L'\0';
        }
    }
}

/* Removes trailing path separators while preserving drive roots (e.g. "C:\") */
static void strip_trailing_separators(WCHAR* path) {
    if (!path) return;
    size_t len = wcslen(path);
    while (len > 1 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        if (len == 3 && path[1] == L':') {
            break; /* Keep "X:\" */
        }
        path[--len] = L'\0';
    }
}

/* Extracts the parent directory of a file path */
static void extract_parent_dir(const WCHAR* file_path, WCHAR* out_dir, size_t out_capacity) {
    if (!out_dir || out_capacity == 0) return;
    out_dir[0] = L'\0';
    if (!file_path || file_path[0] == L'\0') return;

    wcsncpy_s(out_dir, out_capacity, file_path, _TRUNCATE);

    WCHAR* last_sep = NULL;
    for (WCHAR* p = out_dir; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            last_sep = p;
        }
    }

    if (!last_sep) {
        out_dir[0] = L'\0';
        return;
    }

    if (last_sep == out_dir + 2 && out_dir[1] == L':') {
        last_sep[1] = L'\0'; /* Keep root "C:\" */
    } else {
        *last_sep = L'\0';
    }
}

/* Joins a directory and leaf name with a single backslash separator */
static BOOL join_path(const WCHAR* dir, const WCHAR* leaf, WCHAR* out_path, size_t out_capacity) {
    if (!out_path || out_capacity == 0 || !leaf) return FALSE;
    out_path[0] = L'\0';

    if (!dir || dir[0] == L'\0') {
        return wcsncpy_s(out_path, out_capacity, leaf, _TRUNCATE) == 0;
    }

    size_t dir_len = wcslen(dir);
    BOOL has_sep   = (dir[dir_len - 1] == L'\\' || dir[dir_len - 1] == L'/');
    /* Checked first: wcscat_s on a too-small buffer would terminate Total Commander */
    if (dir_len + (has_sep ? 0 : 1) + wcslen(leaf) >= out_capacity) return FALSE;

    wcscpy_s(out_path, out_capacity, dir);
    if (!has_sep) wcscat_s(out_path, out_capacity, L"\\");
    wcscat_s(out_path, out_capacity, leaf);
    return TRUE;
}

BOOL config_is_absolute_path(const WCHAR* p) {
    if (!p) return FALSE;
    if (((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' &&
        (p[2] == L'\\' || p[2] == L'/')) {
        return TRUE;
    }
    return (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}

/* TRUE when `base` (the name up to its first dot, trailing spaces ignored) is a DOS device name */
static BOOL is_dos_device_name(const WCHAR* base, size_t len) {
    static const WCHAR* const devices[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    while (len > 0 && base[len - 1] == L' ') len--;
    for (size_t i = 0; i < _countof(devices); ++i) {
        if (len == wcslen(devices[i]) && _wcsnicmp(base, devices[i], len) == 0) return TRUE;
    }
    /* COM0-COM9, LPT0-LPT9 and the superscript digits 1-3 that Windows treats the same way */
    if (len == 4 && (_wcsnicmp(base, L"COM", 3) == 0 || _wcsnicmp(base, L"LPT", 3) == 0)) {
        WCHAR d = base[3];
        return (d >= L'0' && d <= L'9') || d == L'\u00B9' || d == L'\u00B2' || d == L'\u00B3';
    }
    return FALSE;
}

BOOL config_is_plain_file_name(const WCHAR* path) {
    if (!path) return FALSE;
    const WCHAR* name = path;
    for (const WCHAR* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') name = p + 1;
    }
    size_t len = wcslen(name);
    if (len == 0 || name[len - 1] == L'.' || name[len - 1] == L' ') return FALSE;
    for (const WCHAR* p = name; *p != L'\0'; ++p) {
        if (*p < 0x20 || wcschr(L"<>:\"|?*", *p) != NULL) return FALSE;
    }
    const WCHAR* dot = wcschr(name, L'.');
    return !is_dos_device_name(name, dot ? (size_t)(dot - name) : len);
}

/* Checks whether path points to an existing regular file */
static BOOL file_exists(const WCHAR* path) {
    if (!path || path[0] == L'\0') return FALSE;
    DWORD attr = GetFileAttributesW(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

/* Checks whether a string contains directory separators or a drive colon */
static BOOL has_path_component(const WCHAR* path) {
    if (!path) return FALSE;
    for (const WCHAR* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') return TRUE;
    }
    return FALSE;
}

/* Checks whether a path is relative (not rooted with "\" or "X:") */
static BOOL is_relative_path(const WCHAR* path) {
    if (!path || path[0] == L'\0') return TRUE;
    if (path[0] == L'\\' || path[0] == L'/') return FALSE;
    if (wcslen(path) >= 2 && path[1] == L':') return FALSE;
    return TRUE;
}

/* Validates that an alias is a legal single-level Windows directory name */
static BOOL is_valid_alias(const WCHAR* alias) {
    if (!alias || alias[0] == L'\0') return FALSE;
    if (wcscmp(alias, L".") == 0 || wcscmp(alias, L"..") == 0) return FALSE;

    const WCHAR* invalid_chars = L"\\/:*?\"<>|";
    for (const WCHAR* p = alias; *p != L'\0'; ++p) {
        if (*p < 32 || wcschr(invalid_chars, *p) != NULL) {
            return FALSE;
        }
    }
    return TRUE;
}

void config_set_plugin_dir(const WCHAR* dir_path) {
    if (!dir_path) {
        g_plugin_dir[0] = L'\0';
        return;
    }
    wcsncpy_s(g_plugin_dir, CONFIG_MAX_PATH, dir_path, _TRUNCATE);
    trim_whitespace(g_plugin_dir);
    strip_quotes(g_plugin_dir);
    strip_trailing_separators(g_plugin_dir);
}

void config_set_plugin_dir_from_module(HMODULE hModule) {
    WCHAR module_path[CONFIG_MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(hModule, module_path, CONFIG_MAX_PATH);
    if (len == 0 || len >= CONFIG_MAX_PATH) {
        return;
    }
    WCHAR parent_dir[CONFIG_MAX_PATH] = {0};
    extract_parent_dir(module_path, parent_dir, CONFIG_MAX_PATH);
    config_set_plugin_dir(parent_dir);
}

const WCHAR* config_get_plugin_dir(void) {
    return g_plugin_dir;
}

BOOL config_get_user_ini_path(WCHAR* out_path, size_t out_capacity) {
    if (!out_path || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';

    WCHAR appdata[CONFIG_MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, CONFIG_MAX_PATH);
    if (n == 0 || n >= CONFIG_MAX_PATH) return FALSE;
    trim_whitespace(appdata);
    strip_trailing_separators(appdata);
    if (!config_is_absolute_path(appdata)) return FALSE; /* Never relative to the current directory */

    WCHAR ghisler_dir[CONFIG_MAX_PATH] = {0};
    if (!join_path(appdata, CONFIG_USER_SUBDIR, ghisler_dir, CONFIG_MAX_PATH) ||
        !join_path(ghisler_dir, CONFIG_INI_FILENAME, out_path, out_capacity)) {
        out_path[0] = L'\0';
        return FALSE;
    }
    return TRUE;
}

BOOL config_resolve_ini_path(WCHAR* out_path, size_t out_capacity) {
    if (!out_path || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';

    /* 1. Per-user override: %APPDATA%\GHISLER\TCResticBrowser.ini */
    WCHAR user_candidate[CONFIG_MAX_PATH] = {0};
    if (config_get_user_ini_path(user_candidate, CONFIG_MAX_PATH) && file_exists(user_candidate)) {
        wcsncpy_s(out_path, out_capacity, user_candidate, _TRUNCATE);
        return TRUE;
    }

    /* 2. System-wide file shipped with the plugin: <plugin_dir>\TCResticBrowser.ini */
    if (g_plugin_dir[0] != L'\0') {
        join_path(g_plugin_dir, CONFIG_INI_FILENAME, out_path, out_capacity);
        return file_exists(out_path);
    }
    return FALSE;
}

/*
 * Reads a whole INI file into a heap-allocated, NUL-terminated wide string (caller frees).
 * Encoding: a UTF-16 LE/BE or UTF-8 byte-order mark is honoured; without one the file is UTF-8,
 * the default of today's editors. The Win32 GetPrivateProfile* APIs are not used because they
 * read a BOM-less file in the ANSI code page and garble non-ASCII paths saved as UTF-8. A
 * BOM-less file that is not valid UTF-8 is decoded in the ANSI code page instead, so an INI
 * written for the 1.x plugin (which required ANSI) keeps working.
 * Returns NULL when the file cannot be read or exceeds CONFIG_MAX_INI_BYTES.
 */
static WCHAR* read_ini_text(const WCHAR* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    LARGE_INTEGER size = {0};
    unsigned char* bytes = NULL;
    DWORD got = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= CONFIG_MAX_INI_BYTES) {
        bytes = (unsigned char*)malloc((size_t)size.QuadPart + 1);
        if (bytes && !ReadFile(h, bytes, (DWORD)size.QuadPart, &got, NULL)) {
            free(bytes);
            bytes = NULL;
        }
    }
    CloseHandle(h);
    if (!bytes) return NULL;

    WCHAR* text = NULL;
    if (got >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF))) {
        BOOL big_endian = (bytes[0] == 0xFE);
        size_t count = (got - 2) / 2;
        text = (WCHAR*)malloc((count + 1) * sizeof(WCHAR));
        if (text) {
            for (size_t i = 0; i < count; ++i) {
                unsigned char b0 = bytes[2 + 2 * i], b1 = bytes[3 + 2 * i];
                text[i] = (WCHAR)(big_endian ? ((b0 << 8) | b1) : ((b1 << 8) | b0));
            }
            text[count] = L'\0';
        }
    } else {
        BOOL has_bom = (got >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF);
        const char* src = (const char*)bytes + (has_bom ? 3 : 0);
        int len = (int)got - (has_bom ? 3 : 0);
        UINT code_page = CP_UTF8;
        int wlen = (len > 0) ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, NULL, 0) : 0;
        if (len > 0 && wlen == 0) {
            code_page = has_bom ? CP_UTF8 : CP_ACP;
            wlen = MultiByteToWideChar(code_page, 0, src, len, NULL, 0);
        }
        text = (WCHAR*)malloc(((size_t)wlen + 1) * sizeof(WCHAR));
        if (text) {
            if (wlen > 0 && MultiByteToWideChar(code_page, 0, src, len, text, wlen) != wlen) {
                wlen = 0;
            }
            text[wlen] = L'\0';
        }
    }
    free(bytes);
    return text;
}

/*
 * Walks "key=value" lines of INI text in place (the text is modified). Lines starting with ';'
 * or '#' and lines without '=' are skipped; "[name]" lines change the current section. Key and
 * value are trimmed, the value is also unquoted. Section names compare case-insensitively.
 */
typedef struct {
    WCHAR*       cursor;
    const WCHAR* section;
} IniReader;

static BOOL ini_next_entry(IniReader* r, WCHAR** key, WCHAR** value) {
    while (r->cursor && *r->cursor != L'\0') {
        WCHAR* line = r->cursor;
        WCHAR* eol = line + wcscspn(line, L"\r\n");
        r->cursor = (*eol != L'\0') ? eol + 1 : eol;
        *eol = L'\0';

        while (is_space_char(*line)) line++;
        if (*line == L'\0' || *line == L';' || *line == L'#') continue;

        if (*line == L'[') {
            WCHAR* close = wcschr(line, L']');
            if (close) {
                *close = L'\0';
                trim_whitespace(line + 1);
                r->section = line + 1;
            }
            continue;
        }

        WCHAR* eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = L'\0';
        trim_whitespace(line);
        WCHAR* val = eq + 1;
        trim_whitespace(val);
        strip_quotes(val);
        trim_whitespace(val);
        *key   = line;
        *value = val;
        return TRUE;
    }
    return FALSE;
}

static BOOL ini_in_section(const IniReader* r, const WCHAR* section) {
    return r->section && _wcsicmp(r->section, section) == 0;
}

/*
 * Looks for an executable only inside the directories listed in %PATH%. Unlike SearchPathW, the
 * current directory (which follows the Total Commander panel) is never searched, so a planted
 * "restic.exe" in a browsed folder cannot hijack the lookup. Entries that are not absolute (".",
 * "bin", "C:tools", an unexpanded "%SystemRoot%\system32") would resolve against the current
 * directory and are skipped. Result is an absolute path.
 */
static BOOL find_in_path_env(const WCHAR* name, WCHAR* out_path, size_t out_capacity) {
    WCHAR file_name[MAX_PATH];
    BOOL add_ext = wcschr(name, L'.') == NULL;
    if (wcslen(name) + (add_ext ? 4 : 0) >= _countof(file_name)) return FALSE;
    wcscpy_s(file_name, _countof(file_name), name);
    if (add_ext) wcscat_s(file_name, _countof(file_name), L".exe");

    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
    WCHAR* path_env = (need > 0) ? (WCHAR*)malloc(need * sizeof(WCHAR)) : NULL;
    if (!path_env) return FALSE;
    if (GetEnvironmentVariableW(L"PATH", path_env, need) >= need) {
        free(path_env);
        return FALSE;
    }

    BOOL found = FALSE;
    WCHAR* ctx = NULL;
    for (WCHAR* dir = wcstok_s(path_env, L";", &ctx); dir && !found; dir = wcstok_s(NULL, L";", &ctx)) {
        WCHAR candidate[CONFIG_MAX_PATH];
        trim_whitespace(dir);
        strip_quotes(dir);
        if (config_is_absolute_path(dir) && join_path(dir, file_name, candidate, CONFIG_MAX_PATH) &&
            file_exists(candidate)) {
            DWORD len = GetFullPathNameW(candidate, (DWORD)out_capacity, out_path, NULL);
            found = (len > 0 && len < out_capacity);
        }
    }
    free(path_env);
    return found;
}

/* Fills `error` (if given) with a printf-style message; always NUL-terminated */
static void set_error(WCHAR* error, size_t error_capacity, const WCHAR* fmt, ...) {
    if (!error || error_capacity == 0) return;
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(error, error_capacity, _TRUNCATE, fmt, args);
    va_end(args);
}

/* Looks for `name` (".exe" appended when it has no extension) in the plugin folder, then in PATH */
static BOOL find_in_plugin_dir_or_path(const WCHAR* name, WCHAR* out_path, size_t out_capacity) {
    WCHAR file_name[MAX_PATH];
    BOOL add_ext = wcschr(name, L'.') == NULL;
    if (wcslen(name) + (add_ext ? 4 : 0) >= _countof(file_name)) return FALSE;
    wcscpy_s(file_name, _countof(file_name), name);
    if (add_ext) wcscat_s(file_name, _countof(file_name), L".exe");

    WCHAR candidate[CONFIG_MAX_PATH];
    if (g_plugin_dir[0] != L'\0' && join_path(g_plugin_dir, file_name, candidate, CONFIG_MAX_PATH) &&
        file_exists(candidate)) {
        wcsncpy_s(out_path, out_capacity, candidate, _TRUNCATE);
        return TRUE;
    }
    return find_in_path_env(file_name, out_path, out_capacity);
}

BOOL config_resolve_restic_path(
    const WCHAR* ini_path,
    WCHAR* out_path,
    size_t out_capacity,
    WCHAR* error,
    size_t error_capacity
) {
    if (error && error_capacity > 0) error[0] = L'\0';
    if (!out_path || out_capacity == 0) return FALSE;
    out_path[0] = L'\0';

    /* Read [Settings] ResticPath (first occurrence) if an existing INI file is provided */
    WCHAR configured_val[CONFIG_MAX_PATH] = {0};
    BOOL configured = FALSE, too_long = FALSE;
    WCHAR* ini_text = (ini_path && file_exists(ini_path)) ? read_ini_text(ini_path) : NULL;
    if (ini_text) {
        IniReader reader = {ini_text, NULL};
        WCHAR *key = NULL, *value = NULL;
        while (ini_next_entry(&reader, &key, &value)) {
            if (ini_in_section(&reader, L"Settings") && _wcsicmp(key, L"ResticPath") == 0) {
                /* An over-long value is refused rather than truncated into a different path */
                configured = value[0] != L'\0';
                too_long   = wcslen(value) >= CONFIG_MAX_PATH;
                if (!too_long) wcsncpy_s(configured_val, CONFIG_MAX_PATH, value, _TRUNCATE);
                break;
            }
        }
        free(ini_text);
    }

    /* Not found: the bare name stays in out_path (restic_exec_command refuses it, never searching
       the cwd) and `error` says why */
    wcsncpy_s(out_path, out_capacity, CONFIG_DEFAULT_RESTIC_EXE, _TRUNCATE);

    /* 1. An explicit [Settings] ResticPath is used as written or not at all: falling back to
          another restic.exe would hide that the configured one is missing */
    if (configured) {
        WCHAR candidate[CONFIG_MAX_PATH] = {0};
        if (too_long) {
            set_error(error, error_capacity, L"ResticPath in %ls is too long.", ini_path);
            return FALSE;
        }
        if (!has_path_component(configured_val)) {
            /* Bare name such as "restic-custom.exe" or "restic-custom": plugin folder, then PATH */
            if (find_in_plugin_dir_or_path(configured_val, candidate, CONFIG_MAX_PATH)) {
                wcsncpy_s(out_path, out_capacity, candidate, _TRUNCATE);
                return TRUE;
            }
            set_error(error, error_capacity,
                      L"ResticPath=%ls (set in %ls) was found neither in the plugin folder nor in PATH.",
                      configured_val, ini_path);
            return FALSE;
        }
        if (config_is_absolute_path(configured_val)) {
            wcsncpy_s(candidate, CONFIG_MAX_PATH, configured_val, _TRUNCATE);
        } else if (!is_relative_path(configured_val)) {
            /* "C:restic.exe" and "\\tools\\restic.exe" depend on the current drive or directory */
            set_error(error, error_capacity,
                      L"ResticPath=%ls (set in %ls) depends on the current drive or folder. Use a full path "
                      L"such as C:\\Tools\\restic.exe or a path relative to the plugin folder.",
                      configured_val, ini_path);
            return FALSE;
        } else if (g_plugin_dir[0] == L'\0' || !join_path(g_plugin_dir, configured_val, candidate, CONFIG_MAX_PATH)) {
            set_error(error, error_capacity,
                      L"ResticPath=%ls (set in %ls) cannot be resolved against the plugin folder.",
                      configured_val, ini_path);
            return FALSE;
        }
        if (!file_exists(candidate)) {
            set_error(error, error_capacity, L"ResticPath=%ls (set in %ls) does not exist or is not a file: %ls",
                      configured_val, ini_path, candidate);
            return FALSE;
        }
        wcsncpy_s(out_path, out_capacity, candidate, _TRUNCATE);
        return TRUE;
    }

    /* 2. <plugin_dir>\restic.exe, 3. the directories listed in PATH */
    WCHAR found[CONFIG_MAX_PATH] = {0};
    if (find_in_plugin_dir_or_path(CONFIG_DEFAULT_RESTIC_EXE, found, CONFIG_MAX_PATH)) {
        wcsncpy_s(out_path, out_capacity, found, _TRUNCATE);
        return TRUE;
    }
    set_error(error, error_capacity,
              L"restic.exe not found. Set ResticPath in TCResticBrowser.ini or place restic.exe in the "
              L"plugin folder or in a folder listed in PATH.");
    return FALSE;
}

/* Appends or updates a repository entry in out_config */
static BOOL upsert_repo_entry(
    ResticConfig* config,
    size_t* capacity,
    const WCHAR* alias,
    const WCHAR* target
) {
    /* Update in-place if alias already exists (case-insensitive) */
    for (size_t i = 0; i < config->repo_count; ++i) {
        if (_wcsicmp(config->repos[i].alias, alias) == 0) {
            wcsncpy_s(config->repos[i].target, CONFIG_MAX_PATH, target, _TRUNCATE);
            return TRUE;
        }
    }

    if (config->repo_count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        ResticRepoEntry* new_repos = (ResticRepoEntry*)realloc(
            config->repos,
            new_cap * sizeof(ResticRepoEntry)
        );
        if (!new_repos) return FALSE;
        config->repos = new_repos;
        *capacity = new_cap;
    }

    ResticRepoEntry* entry = &config->repos[config->repo_count];
    memset(entry, 0, sizeof(ResticRepoEntry));
    wcsncpy_s(entry->alias, MAX_PATH, alias, _TRUNCATE);
    wcsncpy_s(entry->target, CONFIG_MAX_PATH, target, _TRUNCATE);
    config->repo_count++;
    return TRUE;
}

/* Parses a plain decimal number (digits only, at most 4294967295) */
static BOOL parse_decimal(const WCHAR* text, DWORD* out) {
    if (!text || text[0] == L'\0') return FALSE;
    unsigned long long value = 0;
    for (const WCHAR* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9') return FALSE;
        value = value * 10 + (unsigned long long)(*p - L'0');
        if (value > 0xFFFFFFFFull) return FALSE;
    }
    *out = (DWORD)value;
    return TRUE;
}

/* TRUE for yes/true/on/1 (case-insensitive); FALSE for no/false/off/0 and anything else */
static BOOL parse_yes(const WCHAR* text) {
    return text && (_wcsicmp(text, L"yes") == 0 || _wcsicmp(text, L"true") == 0 || _wcsicmp(text, L"on") == 0 ||
                    wcscmp(text, L"1") == 0);
}

BOOL config_load_from_file(const WCHAR* ini_path, ResticConfig* out_config) {
    if (!out_config) return FALSE;
    memset(out_config, 0, sizeof(ResticConfig));
    wcsncpy_s(out_config->root_name, MAX_PATH, CONFIG_DEFAULT_ROOT_NAME, _TRUNCATE);
    out_config->password_timeout_s = CONFIG_DEFAULT_PASSWORD_TIMEOUT_S;
    out_config->snapshot_cache_mb  = CONFIG_DEFAULT_SNAPSHOT_CACHE_MB;
    out_config->snapshot_list_refresh_s = CONFIG_DEFAULT_SNAPSHOT_LIST_REFRESH_S;
    strcpy_s(out_config->snapshot_time_format, sizeof(out_config->snapshot_time_format),
             SNAPSHOT_TREE_DEFAULT_TIME_FORMAT);

    if (!ini_path || ini_path[0] == L'\0') {
        config_resolve_restic_path(NULL, out_config->restic_path, CONFIG_MAX_PATH, out_config->restic_path_error,
                                   CONFIG_ERROR_CHARS);
        return FALSE;
    }

    /* Normalize the INI path (a relative one is resolved against the current directory) */
    if (GetFullPathNameW(ini_path, CONFIG_MAX_PATH, out_config->ini_path, NULL) == 0) {
        wcsncpy_s(out_config->ini_path, CONFIG_MAX_PATH, ini_path, _TRUNCATE);
    }

    config_resolve_restic_path(out_config->ini_path, out_config->restic_path, CONFIG_MAX_PATH,
                               out_config->restic_path_error, CONFIG_ERROR_CHARS);

    if (!file_exists(out_config->ini_path)) {
        return FALSE;
    }
    WCHAR* ini_text = read_ini_text(out_config->ini_path);
    if (!ini_text) {
        return FALSE;
    }

    /* Every [Repositories] line is "<Alias>=<Target>"; a repeated alias updates the earlier entry */
    size_t repo_capacity = 0;
    BOOL root_name_seen = FALSE;
    BOOL log_path_seen = FALSE;
    BOOL timeout_seen = FALSE;
    BOOL reset_seen = FALSE;
    BOOL cache_seen = FALSE;
    BOOL format_seen = FALSE;
    BOOL refresh_seen = FALSE;
    BOOL connection_log_seen = FALSE;
    BOOL csv_separator_seen = FALSE;
    IniReader reader = {ini_text, NULL};
    WCHAR *alias = NULL, *target = NULL;
    while (ini_next_entry(&reader, &alias, &target)) {
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"RootName") == 0) {
            /* First occurrence wins; an empty, over-long or invalid name keeps the default */
            if (!root_name_seen && is_valid_alias(target) && wcslen(target) < MAX_PATH) {
                wcsncpy_s(out_config->root_name, MAX_PATH, target, _TRUNCATE);
            }
            root_name_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"PasswordTimeoutSeconds") == 0) {
            /* First occurrence wins; anything but a plain decimal number keeps the default */
            DWORD seconds = 0;
            if (!timeout_seen && parse_decimal(target, &seconds)) {
                out_config->password_timeout_s = seconds;
            }
            timeout_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"SnapshotTimeFormat") == 0) {
            /* First occurrence wins; an unusable format keeps the default and is reported */
            char format[SNAPSHOT_TREE_TIME_FORMAT_MAX * 4 + 1];
            if (!format_seen) {
                int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, target, -1, format, (int)sizeof(format), NULL, NULL);
                if (n > 0 && snapshot_tree_time_format_valid(format)) {
                    strcpy_s(out_config->snapshot_time_format, sizeof(out_config->snapshot_time_format), format);
                } else {
                    out_config->snapshot_time_format_invalid = TRUE;
                }
            }
            format_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"CsvSeparator") == 0) {
            /* First occurrence wins; anything else keeps the default (the Windows list separator) */
            if (!csv_separator_seen) {
                if (wcscmp(target, L",") == 0 || wcscmp(target, L";") == 0 || wcscmp(target, L"|") == 0) {
                    out_config->csv_separator = (char)target[0];
                } else if (_wcsicmp(target, L"tab") == 0) {
                    out_config->csv_separator = '\t';
                }
            }
            csv_separator_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"ConnectionLog") == 0) {
            /* First occurrence wins; anything but a recognised "yes" keeps the default (no) */
            if (!connection_log_seen) out_config->connection_log = parse_yes(target);
            connection_log_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"SnapshotListRefreshSeconds") == 0) {
            /* First occurrence wins; anything but a plain decimal number keeps the default */
            DWORD seconds = 0;
            if (!refresh_seen && parse_decimal(target, &seconds)) out_config->snapshot_list_refresh_s = seconds;
            refresh_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"SnapshotCacheMB") == 0) {
            /* First occurrence wins; anything but a plain decimal number keeps the default */
            DWORD mb = 0;
            if (!cache_seen && parse_decimal(target, &mb)) out_config->snapshot_cache_mb = mb;
            cache_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Settings") && _wcsicmp(alias, L"PasswordTimeoutResetOnUse") == 0) {
            /* First occurrence wins; anything but a recognised "yes" keeps the default (no) */
            if (!reset_seen) out_config->password_timeout_reset_on_use = parse_yes(target);
            reset_seen = TRUE;
            continue;
        }
        if (ini_in_section(&reader, L"Logging") && _wcsicmp(alias, L"LogPath") == 0) {
            /* First occurrence wins; an over-long path leaves logging off */
            if (!log_path_seen && wcslen(target) < CONFIG_MAX_PATH) {
                wcsncpy_s(out_config->log_path, CONFIG_MAX_PATH, target, _TRUNCATE);
            }
            log_path_seen = TRUE;
            continue;
        }
        if (!ini_in_section(&reader, L"Repositories")) {
            continue;
        }
        strip_quotes(alias);
        trim_whitespace(alias);

        /* Over-long entries are skipped: truncating a target could name a different repository */
        if (!is_valid_alias(alias) || target[0] == L'\0' ||
            wcslen(alias) >= MAX_PATH || wcslen(target) >= CONFIG_MAX_PATH) {
            continue;
        }

        if (!upsert_repo_entry(out_config, &repo_capacity, alias, target)) {
            free(ini_text);
            config_free(out_config);
            return FALSE;
        }
    }

    free(ini_text);
    return TRUE;
}

BOOL config_load(ResticConfig* out_config) {
    if (!out_config) return FALSE;
    memset(out_config, 0, sizeof(ResticConfig));

    WCHAR resolved_ini[CONFIG_MAX_PATH] = {0};
    config_resolve_ini_path(resolved_ini, CONFIG_MAX_PATH);
    BOOL ok = config_load_from_file(resolved_ini, out_config);
    log_configure(out_config->log_path);
    return ok;
}

const ResticRepoEntry* config_find_repo(const ResticConfig* config, const WCHAR* alias) {
    if (!config || !config->repos || !alias || alias[0] == L'\0') {
        return NULL;
    }
    for (size_t i = 0; i < config->repo_count; ++i) {
        if (_wcsicmp(config->repos[i].alias, alias) == 0) {
            return &config->repos[i];
        }
    }
    return NULL;
}

void config_free(ResticConfig* config) {
    if (!config) return;
    free(config->repos);
    memset(config, 0, sizeof(ResticConfig));
}
