#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include "config.h"
#include "log.h"
#include "fsplugin.h"

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[FAIL] %s:%d: %s\n", __FUNCTION__, __LINE__, (msg)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

#define EXPECT_WSTR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (_wcsicmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
} while (0)

/* Helper: writes an ASCII/UTF-8 text file to disk */
static BOOL write_text_file(const WCHAR* path, const char* content) {
    HANDLE hFile = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    DWORD len = (DWORD)strlen(content);
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content, len, &written, NULL);
    CloseHandle(hFile);
    return ok && (written == len);
}

/* Helper: creates a unique temporary directory for isolated tests */
static BOOL make_temp_dir(const WCHAR* prefix, WCHAR* out_dir, size_t capacity) {
    WCHAR base_temp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, base_temp);
    swprintf_s(out_dir, capacity, L"%ls%ls_%lu", base_temp, prefix, GetCurrentProcessId());
    CreateDirectoryW(out_dir, NULL);
    return TRUE;
}

/* Points %APPDATA% at `appdata_dir` and returns its GHISLER subdirectory in `out_user_dir` */
static void use_temp_appdata(const WCHAR* appdata_dir, WCHAR* out_user_dir, size_t capacity) {
    SetEnvironmentVariableW(L"APPDATA", appdata_dir);
    swprintf_s(out_user_dir, capacity, L"%ls\\%ls", appdata_dir, CONFIG_USER_SUBDIR);
    CreateDirectoryW(out_user_dir, NULL);
}

/* Test 1: Directory normalization; FsSetDefaultParams does not influence the INI lookup */
static void test_plugin_dir_and_default_params(void) {
    config_set_plugin_dir(L"  \"C:\\TotalCmd\\plugins\\wfx\\restic\\\"  ");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\TotalCmd\\plugins\\wfx\\restic", "Trim and strip trailing backslash");

    config_set_plugin_dir(L"C:\\");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\", "Preserve drive root backslash");

    WCHAR saved_appdata[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"APPDATA", saved_appdata, MAX_PATH);
    SetEnvironmentVariableW(L"APPDATA", L"C:\\Users\\TestUser\\AppData\\Roaming\\");
    WCHAR user_ini[MAX_PATH] = {0};
    EXPECT_TRUE(config_get_user_ini_path(user_ini, MAX_PATH), "Per-user INI path is built from %APPDATA%");
    EXPECT_WSTR_EQ(user_ini, L"C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER\\TCResticBrowser.ini",
                   "Per-user INI lives in Total Commander's settings folder");

    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = sizeof(dps);
    strncpy_s(dps.DefaultIniName, sizeof(dps.DefaultIniName), "D:\\Elsewhere\\fsplugin.ini", _TRUNCATE);
    FsSetDefaultParams(&dps);
    EXPECT_TRUE(config_get_user_ini_path(user_ini, MAX_PATH), "Per-user INI path still available");
    EXPECT_WSTR_EQ(user_ini, L"C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER\\TCResticBrowser.ini",
                   "DefaultIniName does not move the per-user INI");

    SetEnvironmentVariableW(L"APPDATA", NULL);
    EXPECT_TRUE(!config_get_user_ini_path(user_ini, MAX_PATH) && user_ini[0] == L'\0',
                "No per-user INI path without %APPDATA%");
    SetEnvironmentVariableW(L"APPDATA", saved_appdata);
}

/* Test 2: INI path resolution (per-user %APPDATA%\GHISLER over the plugin directory) */
static void test_ini_path_resolution_priority(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, appdata_dir[MAX_PATH] = {0}, user_dir[MAX_PATH] = {0};
    WCHAR saved_appdata[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"APPDATA", saved_appdata, MAX_PATH);
    make_temp_dir(L"restic_test_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_test_appdata", appdata_dir, MAX_PATH);
    use_temp_appdata(appdata_dir, user_dir, MAX_PATH);

    WCHAR plugin_ini[MAX_PATH] = {0};
    WCHAR user_ini[MAX_PATH]   = {0};
    swprintf_s(plugin_ini, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(user_ini, MAX_PATH, L"%ls\\%ls", user_dir, CONFIG_INI_FILENAME);

    DeleteFileW(plugin_ini);
    DeleteFileW(user_ini);

    config_set_plugin_dir(plugin_dir);

    /* Case A: Neither exists -> returns FALSE and does not create any file on disk */
    WCHAR resolved[MAX_PATH] = {0};
    BOOL ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(!ok, "Should return FALSE when no INI file exists");
    EXPECT_WSTR_EQ(resolved, plugin_ini, "Candidate path points to <plugin_dir>\\TCResticBrowser.ini");
    EXPECT_TRUE(GetFileAttributesW(plugin_ini) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(user_ini) == INVALID_FILE_ATTRIBUTES,
                "No INI file is auto-created when missing");

    /* Case B: Only the shipped INI in the plugin directory exists -> it is used */
    write_text_file(plugin_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve the plugin-directory INI when there is no per-user INI");
    EXPECT_WSTR_EQ(resolved, plugin_ini, "Resolved path is the plugin-directory INI");

    /* Case C: Both exist -> the per-user INI takes precedence */
    write_text_file(user_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve the per-user INI when present");
    EXPECT_WSTR_EQ(resolved, user_ini, "Per-user INI must take precedence over the plugin directory");

    /* Case D: Only the per-user INI exists -> it is used */
    DeleteFileW(plugin_ini);
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve the per-user INI alone");
    EXPECT_WSTR_EQ(resolved, user_ini, "Resolved path is the per-user INI");

    /* Case E: %APPDATA% not set -> only the plugin directory is consulted */
    write_text_file(plugin_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    SetEnvironmentVariableW(L"APPDATA", NULL);
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve the plugin-directory INI without %APPDATA%");
    EXPECT_WSTR_EQ(resolved, plugin_ini, "Resolved path is the plugin-directory INI without %APPDATA%");

    SetEnvironmentVariableW(L"APPDATA", saved_appdata);
    DeleteFileW(plugin_ini);
    DeleteFileW(user_ini);
    RemoveDirectoryW(user_dir);
    RemoveDirectoryW(appdata_dir);
    RemoveDirectoryW(plugin_dir);
}

/* Test 3: Restic executable resolution priority */
static void test_restic_binary_resolution(void) {
    WCHAR plugin_dir[MAX_PATH] = {0};
    WCHAR custom_dir[MAX_PATH] = {0};
    make_temp_dir(L"restic_bin_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_bin_cust", custom_dir, MAX_PATH);

    WCHAR ini_path[MAX_PATH]       = {0};
    WCHAR local_restic[MAX_PATH]   = {0};
    WCHAR custom_restic[MAX_PATH]  = {0};
    swprintf_s(ini_path, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(local_restic, MAX_PATH, L"%ls\\restic.exe", plugin_dir);
    swprintf_s(custom_restic, MAX_PATH, L"%ls\\my_restic.exe", custom_dir);

    /* Create dummy binaries */
    write_text_file(local_restic, "MZ_DUMMY_LOCAL");
    write_text_file(custom_restic, "MZ_DUMMY_CUSTOM");

    config_set_plugin_dir(plugin_dir);

    /* Priority 1: Explicit custom path in [Settings] ResticPath */
    char ini_content[2048];
    snprintf(ini_content, sizeof(ini_content),
             "[Settings]\r\nResticPath=\"%ls\"\r\n", custom_restic);
    write_text_file(ini_path, ini_content);

    WCHAR resolved_bin[MAX_PATH] = {0};
    BOOL ok = config_resolve_restic_path(ini_path, resolved_bin, MAX_PATH);
    EXPECT_TRUE(ok, "Custom ResticPath should resolve");
    EXPECT_WSTR_EQ(resolved_bin, custom_restic, "Explicit ResticPath takes highest priority");

    /* Priority 2: ResticPath is default/empty -> picks <plugin_dir>\restic.exe */
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_restic_path(ini_path, resolved_bin, MAX_PATH);
    EXPECT_TRUE(ok, "Local plugin_dir restic.exe should resolve");
    EXPECT_WSTR_EQ(resolved_bin, local_restic, "Should pick <plugin_dir>\\restic.exe");

    /* Priority 3: Remove <plugin_dir>\restic.exe -> falls back to PATH or bare restic.exe */
    DeleteFileW(local_restic);
    config_resolve_restic_path(ini_path, resolved_bin, MAX_PATH);
    EXPECT_TRUE(resolved_bin[0] != L'\0', "Fallback restic binary string must not be empty");

    DeleteFileW(custom_restic);
    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
    RemoveDirectoryW(custom_dir);
}

/* Test 4: [Repositories] section parsing, sanitization, deduplication, and WFX root enumeration */
static void test_repositories_parsing_and_wfx_root(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_repo_plug", plugin_dir, CONFIG_MAX_PATH);

    WCHAR ini_path[CONFIG_MAX_PATH] = {0};
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);

    const char* sample_ini =
        "[Settings]\r\n"
        "ResticPath=restic.exe\r\n"
        "\r\n"
        "[Repositories]\r\n"
        "; Comment line with semicolon\r\n"
        "# Comment line with hash\r\n"
        "BackupLocale=C:\\Backups\\OldPath\r\n"
        "ServerLinux = rest:https://backup.lan:8000/\r\n"
        "CloudS3 = \"s3:s3.us-east-1.amazonaws.com/company-backup/restic\"\r\n"
        "NAS_SMB=\\\\nas\\share\\restic_repo\r\n"
        "Invalid/Alias=C:\\ShouldBeIgnored\r\n"
        "..=C:\\ShouldBeIgnoredToo\r\n"
        "EmptyTarget=\r\n"
        "backuplocale = D:\\Backups\\ResticRepo\r\n";

    write_text_file(ini_path, sample_ini);
    config_set_plugin_dir(plugin_dir);

    ResticConfig cfg;
    BOOL ok = config_load(&cfg);
    EXPECT_TRUE(ok, "config_load should succeed");
    EXPECT_TRUE(cfg.repo_count == 4, "Should parse 4 valid unique repositories");

    const ResticRepoEntry* r0 = config_find_repo(&cfg, L"BackupLocale");
    EXPECT_TRUE(r0 != NULL, "Find BackupLocale");
    EXPECT_WSTR_EQ(r0->target, L"D:\\Backups\\ResticRepo", "Case-insensitive duplicate should update target");

    const ResticRepoEntry* r1 = config_find_repo(&cfg, L"serverlinux");
    EXPECT_TRUE(r1 != NULL, "Case-insensitive lookup for ServerLinux");
    EXPECT_WSTR_EQ(r1->target, L"rest:https://backup.lan:8000/", "ServerLinux target match");

    const ResticRepoEntry* r2 = config_find_repo(&cfg, L"CloudS3");
    EXPECT_TRUE(r2 != NULL, "Find CloudS3");
    EXPECT_WSTR_EQ(r2->target, L"s3:s3.us-east-1.amazonaws.com/company-backup/restic", "Quotes stripped from target");

    const ResticRepoEntry* r3 = config_find_repo(&cfg, L"NAS_SMB");
    EXPECT_TRUE(r3 != NULL, "Find NAS_SMB");
    EXPECT_WSTR_EQ(r3->target, L"\\\\nas\\share\\restic_repo", "UNC path preserved");

    EXPECT_TRUE(config_find_repo(&cfg, L"NonExistent") == NULL, "Unknown alias returns NULL");
    config_free(&cfg);

    /* Verify WFX Level 0 Root ("\") enumeration through FsFindFirstW / FsFindNextW / FsFindClose */
    WIN32_FIND_DATAW find_data;
    HANDLE hFind = FsFindFirstW(L"\\", &find_data);
    EXPECT_TRUE(hFind != INVALID_HANDLE_VALUE, "FsFindFirstW on root '\\' should succeed");
    EXPECT_TRUE((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "Root item 0 must be directory");
    EXPECT_WSTR_EQ(find_data.cFileName, L"BackupLocale", "Root item 0 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 1");
    EXPECT_WSTR_EQ(find_data.cFileName, L"ServerLinux", "Root item 1 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 2");
    EXPECT_WSTR_EQ(find_data.cFileName, L"CloudS3", "Root item 2 name");

    EXPECT_TRUE(FsFindNextW(hFind, &find_data), "FsFindNextW item 3");
    EXPECT_WSTR_EQ(find_data.cFileName, L"NAS_SMB", "Root item 3 name");

    EXPECT_TRUE(!FsFindNextW(hFind, &find_data), "FsFindNextW after last item must return FALSE");
    FsFindClose(hFind);

    /* Empty [Repositories] -> ERROR_NO_MORE_FILES */
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n[Repositories]\r\n");
    hFind = FsFindFirstW(L"\\", &find_data);
    EXPECT_TRUE(hFind == INVALID_HANDLE_VALUE, "Empty repositories should return INVALID_HANDLE_VALUE");
    EXPECT_TRUE(GetLastError() == ERROR_NO_MORE_FILES, "Last error must be ERROR_NO_MORE_FILES");

    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
}

/* Helper: writes raw bytes (may contain NULs) to disk */
static BOOL write_bytes(const WCHAR* path, const void* data, DWORD len) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, NULL) && written == len;
    CloseHandle(h);
    return ok;
}

/* Helper: writes `text` as UTF-8 (optionally with BOM) or UTF-16 LE/BE with BOM */
typedef enum { ENC_UTF8, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE } TestEncoding;

static BOOL write_encoded(const WCHAR* path, const WCHAR* text, TestEncoding enc) {
    static unsigned char buf[16384];
    size_t len = 0, n = wcslen(text);
    if (enc == ENC_UTF8 || enc == ENC_UTF8_BOM) {
        if (enc == ENC_UTF8_BOM) { buf[0] = 0xEF; buf[1] = 0xBB; buf[2] = 0xBF; len = 3; }
        int m = WideCharToMultiByte(CP_UTF8, 0, text, (int)n, (char*)buf + len, (int)(sizeof(buf) - len), NULL, NULL);
        if (m <= 0) return FALSE;
        len += (size_t)m;
    } else {
        if (2 * (n + 1) > sizeof(buf)) return FALSE;
        BOOL be = (enc == ENC_UTF16BE);
        buf[0] = be ? 0xFE : 0xFF; buf[1] = be ? 0xFF : 0xFE; len = 2;
        for (size_t i = 0; i < n; ++i) {
            unsigned char hi = (unsigned char)(text[i] >> 8), lo = (unsigned char)(text[i] & 0xFF);
            buf[len++] = be ? hi : lo;
            buf[len++] = be ? lo : hi;
        }
    }
    return write_bytes(path, buf, (DWORD)len);
}

/* Test 5: INI encodings (UTF-8 default, BOMs, ANSI fallback) and the line syntax of the own reader */
static void test_ini_encodings_and_syntax(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, tools_dir[MAX_PATH] = {0};
    WCHAR ini_path[MAX_PATH] = {0}, custom_restic[MAX_PATH] = {0};
    make_temp_dir(L"restic_enc_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_enc_N\u00e1stroje_\u65e5\u672c", tools_dir, MAX_PATH);
    swprintf_s(ini_path, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(custom_restic, MAX_PATH, L"%ls\\restic.exe", tools_dir);
    write_text_file(custom_restic, "MZ_DUMMY");
    config_set_plugin_dir(plugin_dir);

    /* Non-ASCII alias, target and ResticPath; LF, CRLF and CR line endings; foreign section ignored */
    static WCHAR body[4096];
    swprintf_s(body, _countof(body),
               L"; \u010de\u0161tina \u65e5\u672c\u8a9e\n"
               L"[settings]\r\n"
               L"  ResticPath = \"%ls\"\n"
               L"ResticPath=C:\\second\\is\\ignored.exe\r"
               L"[Other]\n"
               L"Ignored=D:\\other\n"
               L"[REPOSITORIES]\r\n"
               L"Z\u00e1lohy=D:\\Z\u00e1lohy\\repo\r\n"
               L"\u65e5\u672c = rest:https://\u65e5\u672c.example/repo\n"
               L"NoNewlineAtEnd=E:\\last",
               custom_restic);

    ResticConfig cfg; /* One instance per test: /analyze (C6262) caps a frame at 16 KB */
    const TestEncoding encodings[] = {ENC_UTF8, ENC_UTF8_BOM, ENC_UTF16LE, ENC_UTF16BE};
    for (size_t i = 0; i < _countof(encodings); ++i) {
        EXPECT_TRUE(write_encoded(ini_path, body, encodings[i]), "Write encoded INI");
        EXPECT_TRUE(config_load_from_file(ini_path, &cfg), "Encoded INI loads");
        EXPECT_TRUE(cfg.repo_count == 3, "Three repositories, the [Other] section is not mixed in");
        const ResticRepoEntry* r = config_find_repo(&cfg, L"Z\u00e1lohy");
        EXPECT_TRUE(r != NULL, "Non-ASCII alias found");
        EXPECT_WSTR_EQ(r->target, L"D:\\Z\u00e1lohy\\repo", "Non-ASCII target decoded");
        r = config_find_repo(&cfg, L"\u65e5\u672c");
        EXPECT_TRUE(r != NULL, "CJK alias found");
        EXPECT_WSTR_EQ(r->target, L"rest:https://\u65e5\u672c.example/repo", "CJK target decoded");
        r = config_find_repo(&cfg, L"NoNewlineAtEnd");
        EXPECT_TRUE(r != NULL, "Last line without a line break is read");
        EXPECT_WSTR_EQ(r->target, L"E:\\last", "Last line target");
        EXPECT_WSTR_EQ(cfg.restic_path, custom_restic, "First non-ASCII ResticPath in [Settings] is used");
        config_free(&cfg);
    }

    /* A BOM-less file that is not valid UTF-8 is an ANSI file from the 1.x plugin */
    const char ansi_ini[] = "[Repositories]\r\nOld=D:\\Z\xE1lohy\r\n";
    WCHAR expected[64] = {0};
    MultiByteToWideChar(CP_ACP, 0, "D:\\Z\xE1lohy", -1, expected, _countof(expected));
    EXPECT_TRUE(write_bytes(ini_path, ansi_ini, (DWORD)strlen(ansi_ini)), "Write ANSI INI");
    EXPECT_TRUE(config_load_from_file(ini_path, &cfg) && cfg.repo_count == 1, "ANSI INI loads");
    EXPECT_WSTR_EQ(cfg.repos[0].target, expected, "Invalid UTF-8 falls back to the ANSI code page");
    config_free(&cfg);

    /* Over-long entries are skipped, never truncated into a different repository */
    static char long_ini[4096];
    int pos = snprintf(long_ini, sizeof(long_ini), "[Repositories]\r\nTooLong=D:\\");
    for (int i = 0; i < CONFIG_MAX_PATH && pos >= 0 && pos < (int)sizeof(long_ini) - 64; ++i) {
        long_ini[pos++] = 'x';
    }
    snprintf(long_ini + pos, sizeof(long_ini) - (size_t)pos, "\r\nShort=D:\\ok\r\n");
    EXPECT_TRUE(write_text_file(ini_path, long_ini), "Write INI with an over-long target");
    EXPECT_TRUE(config_load_from_file(ini_path, &cfg) && cfg.repo_count == 1 &&
                config_find_repo(&cfg, L"Short") != NULL && config_find_repo(&cfg, L"TooLong") == NULL,
                "Over-long target is skipped");
    config_free(&cfg);

    /* A file above CONFIG_MAX_INI_BYTES is not read at all */
    char* huge = (char*)malloc(CONFIG_MAX_INI_BYTES + 1);
    EXPECT_TRUE(huge != NULL, "Allocate oversized INI");
    memset(huge, ';', CONFIG_MAX_INI_BYTES + 1);
    memcpy(huge, "[Repositories]\nA=D:\\a\n", 22);
    BOOL written = write_bytes(ini_path, huge, CONFIG_MAX_INI_BYTES + 1);
    free(huge);
    EXPECT_TRUE(written, "Write oversized INI");
    EXPECT_TRUE(!config_load_from_file(ini_path, &cfg) && cfg.repo_count == 0, "Oversized INI is refused");
    config_free(&cfg);

    DeleteFileW(ini_path);
    DeleteFileW(custom_restic);
    RemoveDirectoryW(tools_dir);
    RemoveDirectoryW(plugin_dir);
}

/* Test 6: [Settings] RootName and the name FsGetDefRootName reports */
static void test_root_name(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0}, ini_path[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_root_plug", plugin_dir, CONFIG_MAX_PATH);
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    DeleteFileW(ini_path);
    config_set_plugin_dir(plugin_dir);

    ResticConfig cfg;
    char ansi[MAX_PATH];
    EXPECT_TRUE(!config_load(&cfg), "No INI");
    EXPECT_WSTR_EQ(cfg.root_name, CONFIG_DEFAULT_ROOT_NAME, "Default root name without an INI");
    config_free(&cfg);
    FsGetDefRootName(ansi, (int)sizeof(ansi));
    EXPECT_TRUE(strcmp(ansi, "TCResticBrowser") == 0, "FsGetDefRootName default without an INI");

    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n");
    EXPECT_TRUE(config_load(&cfg), "INI without RootName loads");
    EXPECT_WSTR_EQ(cfg.root_name, CONFIG_DEFAULT_ROOT_NAME, "Default root name without RootName");
    config_free(&cfg);

    /* First RootName in [Settings] wins; a RootName line elsewhere is not the setting */
    write_text_file(ini_path,
                    "[Repositories]\r\nRootName=D:\\repo\r\n"
                    "[settings]\r\n  rootname = \"My Backups\" \r\nRootName=Second\r\n");
    EXPECT_TRUE(config_load(&cfg), "INI with RootName loads");
    EXPECT_WSTR_EQ(cfg.root_name, L"My Backups", "Configured root name (trimmed, unquoted)");
    EXPECT_TRUE(cfg.repo_count == 1 && config_find_repo(&cfg, L"RootName") != NULL,
                "RootName under [Repositories] is an ordinary alias");
    config_free(&cfg);
    FsGetDefRootName(ansi, (int)sizeof(ansi));
    EXPECT_TRUE(strcmp(ansi, "My Backups") == 0, "FsGetDefRootName reports the configured name");
    char tiny[4];
    FsGetDefRootName(tiny, (int)sizeof(tiny));
    EXPECT_TRUE(strcmp(tiny, "TCR") == 0, "A name longer than maxlen falls back to the (truncated) default");

    /* Invalid or empty names keep the default; so does a first invalid one followed by a valid one */
    const char* invalid[] = {
        "[Settings]\r\nRootName=a\\b\r\n",
        "[Settings]\r\nRootName=a:b\r\n",
        "[Settings]\r\nRootName=..\r\n",
        "[Settings]\r\nRootName=\r\n",
        "[Settings]\r\nRootName=bad|name\r\nRootName=Good\r\n",
    };
    for (size_t i = 0; i < _countof(invalid); ++i) {
        write_text_file(ini_path, invalid[i]);
        EXPECT_TRUE(config_load(&cfg), "INI with an invalid RootName loads");
        EXPECT_WSTR_EQ(cfg.root_name, CONFIG_DEFAULT_ROOT_NAME, "Invalid RootName keeps the default");
        config_free(&cfg);
    }

    /* A non-ASCII name: reported as is when the ANSI code page has it, otherwise the default */
    EXPECT_TRUE(write_encoded(ini_path, L"[Settings]\nRootName=Z\u00e1lohy \u65e5\u672c\n", ENC_UTF8), "Write UTF-8 INI");
    EXPECT_TRUE(config_load(&cfg), "UTF-8 INI with RootName loads");
    EXPECT_WSTR_EQ(cfg.root_name, L"Z\u00e1lohy \u65e5\u672c", "Non-ASCII root name decoded");
    config_free(&cfg);
    char expected[MAX_PATH];
    BOOL lossy = FALSE;
    BOOL utf8_acp = (GetACP() == CP_UTF8);
    WideCharToMultiByte(CP_ACP, utf8_acp ? 0 : WC_NO_BEST_FIT_CHARS, L"Z\u00e1lohy \u65e5\u672c", -1,
                        expected, (int)sizeof(expected), NULL, utf8_acp ? NULL : &lossy);
    FsGetDefRootName(ansi, (int)sizeof(ansi));
    EXPECT_TRUE(strcmp(ansi, lossy ? "TCResticBrowser" : expected) == 0,
                "Name not representable in the ANSI code page falls back to the default");

    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
}

/* Helper: reads the current value of an environment variable into a heap string (or NULL) */
static WCHAR* dup_env(const WCHAR* name) {
    DWORD need = GetEnvironmentVariableW(name, NULL, 0);
    WCHAR* buf = (need > 0) ? (WCHAR*)malloc(need * sizeof(WCHAR)) : NULL;
    if (buf && GetEnvironmentVariableW(name, buf, need) >= need) {
        free(buf);
        buf = NULL;
    }
    return buf;
}

/* Test 5: restic.exe is searched in %PATH% only - never in the current directory (planting) */
static void test_restic_lookup_ignores_current_directory(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, path_dir[MAX_PATH] = {0};
    WCHAR cwd_dir[MAX_PATH] = {0}, ini_path[MAX_PATH] = {0};
    WCHAR expected[MAX_PATH] = {0}, expected_custom[MAX_PATH] = {0};
    make_temp_dir(L"restic_lookup_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_lookup_path", path_dir, MAX_PATH);
    make_temp_dir(L"restic_lookup_cwd", cwd_dir, MAX_PATH);
    swprintf_s(ini_path, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(expected, MAX_PATH, L"%ls\\restic.exe", path_dir);
    swprintf_s(expected_custom, MAX_PATH, L"%ls\\custom_tool.exe", path_dir);

    WCHAR cwd_restic[MAX_PATH] = {0};
    swprintf_s(cwd_restic, MAX_PATH, L"%ls\\restic.exe", cwd_dir);
    write_text_file(expected, "MZ_PATH");
    write_text_file(expected_custom, "MZ_CUSTOM");
    write_text_file(cwd_restic, "MZ_PLANTED");
    config_set_plugin_dir(plugin_dir); /* No restic.exe here: forces the PATH fallback */

    WCHAR* old_path = dup_env(L"PATH");
    WCHAR old_cwd[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, old_cwd);
    SetCurrentDirectoryW(cwd_dir);

    /* Case A: PATH with empty entries, a quoted entry with spaces and a missing directory */
    WCHAR path_value[MAX_PATH * 2] = {0};
    swprintf_s(path_value, _countof(path_value), L";;C:\\no_such_dir_xyz; \"%ls\" ;", path_dir);
    SetEnvironmentVariableW(L"PATH", path_value);
    WCHAR resolved_a[MAX_PATH] = {0};
    BOOL ok_a = config_resolve_restic_path(NULL, resolved_a, MAX_PATH);

    /* Case B: PATH without restic.exe while the current directory holds one */
    SetEnvironmentVariableW(L"PATH", L"C:\\no_such_dir_xyz");
    WCHAR resolved_b[MAX_PATH] = {0};
    BOOL ok_b = config_resolve_restic_path(NULL, resolved_b, MAX_PATH);

    /* Case C: [Settings] ResticPath is a bare name without extension, found through PATH */
    SetEnvironmentVariableW(L"PATH", path_dir);
    write_text_file(ini_path, "[Settings]\r\nResticPath=custom_tool\r\n");
    WCHAR resolved_c[MAX_PATH] = {0};
    BOOL ok_c = config_resolve_restic_path(ini_path, resolved_c, MAX_PATH);

    SetCurrentDirectoryW(old_cwd);
    SetEnvironmentVariableW(L"PATH", old_path);
    free(old_path);

    EXPECT_TRUE(ok_a, "restic.exe found through PATH");
    EXPECT_WSTR_EQ(resolved_a, expected, "PATH lookup skips empty/invalid entries and strips quotes");
    EXPECT_TRUE(!ok_b, "restic.exe in the current directory must NOT be used");
    EXPECT_WSTR_EQ(resolved_b, L"restic.exe", "Bare name returned when nothing is found");
    EXPECT_TRUE(ok_c, "Custom bare name found through PATH");
    EXPECT_WSTR_EQ(resolved_c, expected_custom, ".exe extension appended to extension-less ResticPath");

    DeleteFileW(expected);
    DeleteFileW(expected_custom);
    DeleteFileW(cwd_restic);
    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
    RemoveDirectoryW(path_dir);
    RemoveDirectoryW(cwd_dir);
}

/* Test 6: hundreds of repositories (array and INI-section buffer growth) + root listing details */
static void test_large_repository_list(void) {
    enum { REPO_COUNT = 300 };
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0}, ini_path[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_large_plug", plugin_dir, CONFIG_MAX_PATH);
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);

    size_t cap = 256 + (size_t)REPO_COUNT * 160;
    char* content = (char*)malloc(cap);
    EXPECT_TRUE(content != NULL, "Allocate INI content");
    size_t len = (size_t)snprintf(content, cap, "[Repositories]\r\n  Spaced Alias  =  D:\\path with spaces\\repo  \r\n");
    for (int i = 0; i < REPO_COUNT; ++i) {
        len += (size_t)snprintf(content + len, cap - len,
            "Repo_%03d=D:\\Backups\\A\\Rather\\Long\\Directory\\Structure\\For\\Repository\\Number\\%03d\r\n", i, i);
    }
    BOOL written = write_text_file(ini_path, content);
    free(content);
    EXPECT_TRUE(written, "Write large INI");
    config_set_plugin_dir(plugin_dir);

    ResticConfig cfg;
    EXPECT_TRUE(config_load(&cfg), "Load large configuration");
    EXPECT_TRUE(cfg.repo_count == REPO_COUNT + 1, "All 301 repositories parsed (section buffer grew past 16K chars)");
    const ResticRepoEntry* spaced = config_find_repo(&cfg, L"Spaced Alias");
    EXPECT_TRUE(spaced != NULL, "Alias with inner space, trimmed");
    EXPECT_WSTR_EQ(spaced->target, L"D:\\path with spaces\\repo", "Target trimmed");
    const ResticRepoEntry* last = config_find_repo(&cfg, L"Repo_299");
    EXPECT_TRUE(last != NULL, "Last repository present");
    config_free(&cfg);
    EXPECT_TRUE(cfg.repos == NULL && cfg.repo_count == 0, "config_free resets the structure");

    WIN32_FIND_DATAW fd;
    HANDLE h = FsFindFirstW(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Root enumeration of 301 aliases");
    size_t seen = 1;
    EXPECT_TRUE(fd.ftLastWriteTime.dwHighDateTime == 0xFFFFFFFF && fd.ftLastWriteTime.dwLowDateTime == 0xFFFFFFFE,
                "Aliases carry the 'no date' marker");
    EXPECT_TRUE((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "Aliases are directories");
    while (FsFindNextW(h, &fd)) seen++;
    FsFindClose(h);
    EXPECT_TRUE(seen == REPO_COUNT + 1, "Every alias is enumerated exactly once");
    EXPECT_TRUE(FsFindFirstW(L"\\NoSuchRepo", &fd) == INVALID_HANDLE_VALUE, "Unrouted path is refused");

    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
}

/* Mock callback state for test_wfx_routing_and_callbacks */
static int   g_mock_pwd_calls   = 0;
static int   g_mock_msgok_calls = 0;
static int   g_mock_log_calls   = 0;
static BOOL  g_mock_pwd_accept  = FALSE;
static WCHAR g_mock_last_log[256] = {0};

static void __stdcall mock_log_proc(int PluginNr, int MsgType, WCHAR* LogString) {
    (void)PluginNr;
    if (MsgType == MSGTYPE_IMPORTANTERROR && LogString) {
        g_mock_log_calls++;
        wcsncpy_s(g_mock_last_log, _countof(g_mock_last_log), LogString, _TRUNCATE);
    }
}

static BOOL __stdcall mock_request_proc(
    int PluginNr,
    int RequestType,
    WCHAR* CustomTitle,
    WCHAR* CustomText,
    WCHAR* ReturnedText,
    int maxlen
) {
    (void)PluginNr;
    (void)CustomTitle;
    (void)CustomText;
    if (RequestType == RT_Password) {
        g_mock_pwd_calls++;
        if (!g_mock_pwd_accept) {
            return FALSE;
        }
        wcsncpy_s(ReturnedText, (size_t)maxlen, L"secret", _TRUNCATE);
        return TRUE;
    }
    if (RequestType == RT_MsgOK) {
        g_mock_msgok_calls++;
        return TRUE;
    }
    return FALSE;
}

/* Test 8: with no TCResticBrowser.ini anywhere the plugin stays inert and never writes to disk */
static void test_missing_ini_is_harmless(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, appdata_dir[MAX_PATH] = {0}, user_dir[MAX_PATH] = {0};
    WCHAR local[MAX_PATH] = {0}, saved_appdata[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"APPDATA", saved_appdata, MAX_PATH);
    make_temp_dir(L"restic_noini_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_noini_appdata", appdata_dir, MAX_PATH);
    use_temp_appdata(appdata_dir, user_dir, MAX_PATH);
    swprintf_s(local, MAX_PATH, L"%ls\\extracted.txt", plugin_dir);
    config_set_plugin_dir(plugin_dir);

    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = (int)sizeof(dps);
    WideCharToMultiByte(CP_ACP, 0, user_dir, -1, dps.DefaultIniName, (int)sizeof(dps.DefaultIniName), NULL, NULL);
    strcat_s(dps.DefaultIniName, sizeof(dps.DefaultIniName), "\\wincmd.ini");

    g_mock_pwd_calls = g_mock_msgok_calls = g_mock_log_calls = 0;
    EXPECT_TRUE(FsInitW(9, NULL, mock_log_proc, mock_request_proc) == 0, "FsInitW succeeds without INI");
    FsSetDefaultParams(&dps);

    ResticConfig cfg;
    EXPECT_TRUE(!config_load(&cfg) && cfg.repos == NULL && cfg.repo_count == 0,
                "config_load reports failure and no repositories when no INI exists");
    EXPECT_TRUE(cfg.restic_path[0] != L'\0', "restic.exe path is still resolved (bare name when not found)");
    config_free(&cfg);

    WIN32_FIND_DATAW fd;
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\", &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES,
                "Root is an empty folder without INI");
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\MyRepo", &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
                "Repository level -> ERROR_PATH_NOT_FOUND without INI");
    RemoteInfoStruct ri = {5, 0, {0, 0}, 0};
    WCHAR remote[] = L"\\MyRepo\\2026-10-18_12-00-00_[abcd1234]\\C\\x.txt";
    EXPECT_TRUE(FsGetFileW(remote, local, 0, &ri) == FS_FILE_NOTFOUND &&
                GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES,
                "FsGetFileW -> FS_FILE_NOTFOUND without INI and no local file is created");
    EXPECT_TRUE(g_mock_pwd_calls == 0 && g_mock_msgok_calls == 0, "No password prompt or popup without INI");

    /* RemoveDirectoryW only succeeds on empty directories: nothing was written anywhere */
    EXPECT_TRUE(RemoveDirectoryW(plugin_dir), "Plugin directory is still empty (no INI auto-created)");
    EXPECT_TRUE(RemoveDirectoryW(user_dir), "Per-user settings directory is still empty (no INI auto-created)");
    RemoveDirectoryW(appdata_dir);
    SetEnvironmentVariableW(L"APPDATA", saved_appdata);

    /* Verify that the sample TCResticBrowser.ini the build stages next to this test is valid and all-commented */
    WCHAR sample_ini[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, sample_ini, MAX_PATH);
    WCHAR* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(sample_ini, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Directory of the test executable is known");
    *slash = L'\0';
    wcscat_s(sample_ini, MAX_PATH, L"\\TCResticBrowser.ini");
    EXPECT_TRUE(config_load_from_file(sample_ini, &cfg) && cfg.repo_count == 0,
                "Staged TCResticBrowser.ini template exists and has all repositories commented out");
    EXPECT_WSTR_EQ(cfg.root_name, CONFIG_DEFAULT_ROOT_NAME, "Sample INI leaves RootName commented out");
    config_free(&cfg);

    FsInitW(0, NULL, NULL, NULL);
}

/* Test 7: WFX virtual path validation, missing restic.exe diagnostic, and user password cancel */
static void test_wfx_routing_and_callbacks(void) {
    WCHAR plugin_dir[MAX_PATH] = {0};
    WCHAR ini_path[MAX_PATH]   = {0};
    WCHAR dummy_exe[MAX_PATH]  = {0};
    make_temp_dir(L"restic_wfx_route", plugin_dir, MAX_PATH);
    swprintf_s(ini_path, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(dummy_exe, MAX_PATH, L"%ls\\restic.exe", plugin_dir);

    write_text_file(ini_path, "[Repositories]\r\nMyRepo=D:\\SomeRepo\r\n");
    config_set_plugin_dir(plugin_dir);

    DWORD need = GetEnvironmentVariableW(L"PATH", NULL, 0);
    WCHAR* saved_path = (WCHAR*)calloc((size_t)need + 1, sizeof(WCHAR));
    if (need > 0 && saved_path) {
        GetEnvironmentVariableW(L"PATH", saved_path, need);
    }
    SetEnvironmentVariableW(L"PATH", L"C:\\no_such_dir_restic_test");

    FsInitW(7, NULL, mock_log_proc, mock_request_proc);

    WIN32_FIND_DATAW fd;
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(NULL, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_INVALID_PARAMETER, "NULL Path rejected");
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\", NULL) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_INVALID_PARAMETER, "NULL FindData rejected");

    /* Unknown alias or malformed snapshot folder fails with ERROR_PATH_NOT_FOUND before prompting */
    g_mock_pwd_calls = g_mock_msgok_calls = g_mock_log_calls = 0;
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\UnknownAlias", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Unknown repository alias rejected");
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\MyRepo\\not_a_snapshot_folder", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Invalid snapshot folder name rejected");
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\MyRepo\\2026-10-18_12-00-00_[not_hex!]\\sub", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Non-hex snapshot ID rejected");
    EXPECT_TRUE(g_mock_pwd_calls == 0 && g_mock_msgok_calls == 0,
                "Invalid paths must not prompt for a password or show error dialogs");

    /* Missing restic.exe reports ERROR_FILE_NOT_FOUND + LogProc + RT_MsgOK without asking password */
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\MyRepo", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_FILE_NOT_FOUND, "Missing restic.exe returns ERROR_FILE_NOT_FOUND");
    EXPECT_TRUE(g_mock_pwd_calls == 0, "Missing restic.exe does not prompt for password");
    EXPECT_TRUE(g_mock_msgok_calls == 1 && g_mock_log_calls == 1, "Missing restic.exe notifies user and log");
    EXPECT_TRUE(wcsstr(g_mock_last_log, L"restic.exe not found") != NULL, "Log mentions restic.exe not found");

    /* Place a dummy restic.exe and verify that cancelling the password prompt returns ERROR_CANCELLED */
    write_text_file(dummy_exe, "MZ");
    g_mock_pwd_accept  = FALSE;
    g_mock_pwd_calls   = 0;
    g_mock_msgok_calls = 0;
    g_mock_log_calls   = 0;
    SetLastError(0);
    EXPECT_TRUE(FsFindFirstW(L"\\MyRepo", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_CANCELLED, "Cancelled password prompt returns ERROR_CANCELLED");
    EXPECT_TRUE(g_mock_pwd_calls == 1 && g_mock_msgok_calls == 0,
                "Cancelled password prompt asks once and shows no error dialog");

    FsInitW(0, NULL, NULL, NULL);
    if (saved_path) {
        SetEnvironmentVariableW(L"PATH", saved_path);
        free(saved_path);
    }
    DeleteFileW(dummy_exe);
    DeleteFileW(ini_path);
    RemoveDirectoryW(plugin_dir);
}

/* Reads a whole file as a NUL-terminated byte string (caller frees); NULL when missing */
static char* read_all(const WCHAR* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL), got = 0;
    char* buf = (char*)malloc((size_t)size + 1);
    if (buf && ReadFile(h, buf, size, &got, NULL)) {
        buf[got] = '\0';
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

/* Optional diagnostic log: [Logging] LogPath parsing, expansion, file creation, redaction */
static void test_logging(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, ini_path[MAX_PATH] = {0};
    WCHAR log_root[MAX_PATH] = {0}, log_file[MAX_PATH] = {0}, got[MAX_PATH];
    make_temp_dir(L"restic_log_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_log_out", log_root, MAX_PATH);
    swprintf_s(ini_path, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(log_file, MAX_PATH, L"%ls\\new dir\\sub\\TCResticBrowser.log", log_root);
    config_set_plugin_dir(plugin_dir);
    SetEnvironmentVariableW(L"TCRB_TEST_LOG_ROOT", log_root);

    /* Off by default: no [Logging] section */
    ResticConfig cfg;
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n");
    EXPECT_TRUE(config_load(&cfg), "INI without [Logging] loads");
    EXPECT_TRUE(cfg.log_path[0] == L'\0' && !log_enabled(), "Logging is off without LogPath");
    config_free(&cfg);

    /* First LogPath in [Logging] wins, environment variables are expanded; LogPath elsewhere is ignored */
    write_text_file(ini_path,
                    "[Settings]\r\nLogPath=C:\\settings.log\r\n"
                    "[logging]\r\n logpath = %TCRB_TEST_LOG_ROOT%\\new dir\\sub\\TCResticBrowser.log \r\n"
                    "LogPath=C:\\second.log\r\n");
    EXPECT_TRUE(config_load(&cfg), "INI with [Logging] loads");
    EXPECT_WSTR_EQ(cfg.log_path, L"%TCRB_TEST_LOG_ROOT%\\new dir\\sub\\TCResticBrowser.log",
                   "LogPath kept as written");
    config_free(&cfg);
    EXPECT_TRUE(log_enabled(), "config_load turns logging on");
    log_get_path(got, _countof(got));
    EXPECT_WSTR_EQ(got, log_file, "LogPath expanded");

    log_write(L"first line %d", 1);
    log_write(L"non-ASCII \u017dlu\u0165ou\u010dk\u00fd k\u016f\u0148 \u65e5\u672c");
    char* text = read_all(log_file);
    EXPECT_TRUE(text != NULL, "Log file and its missing parent directories created");
    BOOL has_first = strstr(text, "first line 1\r\n") != NULL;
    BOOL has_utf8  = strstr(text, "non-ASCII \xC5\xBDlu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88 \xE6\x97\xA5\xE6\x9C\xAC\r\n") != NULL;
    BOOL has_pid   = strstr(text, "[pid ") != NULL;
    BOOL no_bom    = (unsigned char)text[0] != 0xEF;
    free(text);
    EXPECT_TRUE(has_first && has_pid, "Lines are appended with a time/pid/tid prefix and CRLF");
    EXPECT_TRUE(has_utf8 && no_bom, "Lines are UTF-8 without a byte-order mark");

    /* The file is not kept open: it can be deleted, and the next line recreates it */
    EXPECT_TRUE(DeleteFileW(log_file), "Log file can be deleted while logging is on");
    log_write(L"after delete");
    text = read_all(log_file);
    EXPECT_TRUE(text != NULL && strstr(text, "after delete") != NULL, "Next line recreates the log file");
    free(text);

    /* A relative path (it would depend on Total Commander's current directory) or an empty value: off */
    write_text_file(ini_path, "[Logging]\r\nLogPath=relative\\TCResticBrowser.log\r\n");
    config_load(&cfg);
    config_free(&cfg);
    EXPECT_TRUE(!log_enabled(), "Relative LogPath leaves logging off");
    write_text_file(ini_path, "[Logging]\r\nLogPath=\r\n");
    config_load(&cfg);
    config_free(&cfg);
    EXPECT_TRUE(!log_enabled(), "Empty LogPath turns logging off");
    log_write(L"must not appear");
    text = read_all(log_file);
    EXPECT_TRUE(text != NULL && strstr(text, "must not appear") == NULL, "Nothing is written while logging is off");
    free(text);

    /* Removing the INI turns logging off as well */
    DeleteFileW(ini_path);
    log_configure(L"%TCRB_TEST_LOG_ROOT%\\x.log");
    EXPECT_TRUE(log_enabled(), "log_configure turns logging on directly");
    config_load(&cfg);
    config_free(&cfg);
    EXPECT_TRUE(!log_enabled(), "No INI: logging off");

    /* Credentials in repository URLs are masked; other targets are kept */
    WCHAR red[256];
    log_redact_target(L"rest:https://user:secret@host:8000/repo", red, _countof(red));
    EXPECT_WSTR_EQ(red, L"rest:https://***@host:8000/repo", "rest: URL credentials masked");
    log_redact_target(L"s3:https://key:p@ss@minio.local/bucket", red, _countof(red));
    EXPECT_WSTR_EQ(red, L"s3:https://***@minio.local/bucket", "Last '@' ends the userinfo");
    log_redact_target(L"rest:http://host/repo?x=a@b", red, _countof(red));
    EXPECT_WSTR_EQ(red, L"rest:http://host/repo?x=a@b", "'@' after the authority is not userinfo");
    log_redact_target(L"sftp:user@host:/srv/repo", red, _countof(red));
    EXPECT_WSTR_EQ(red, L"sftp:user@host:/srv/repo", "sftp: target without a password kept");
    log_redact_target(L"C:\\Backups\\repo", red, _countof(red));
    EXPECT_WSTR_EQ(red, L"C:\\Backups\\repo", "Local path kept");

    WCHAR dir1[MAX_PATH], dir2[MAX_PATH];
    swprintf_s(dir2, MAX_PATH, L"%ls\\new dir\\sub", log_root);
    swprintf_s(dir1, MAX_PATH, L"%ls\\new dir", log_root);
    DeleteFileW(log_file);
    RemoveDirectoryW(dir2);
    RemoveDirectoryW(dir1);
    RemoveDirectoryW(log_root);
    RemoveDirectoryW(plugin_dir);
    SetEnvironmentVariableW(L"TCRB_TEST_LOG_ROOT", NULL);
}

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic WFX Configuration Unit Tests...\n");

    /* Never pick up the developer's own %APPDATA%\GHISLER\TCResticBrowser.ini */
    WCHAR appdata_dir[MAX_PATH] = {0}, user_dir[MAX_PATH] = {0};
    make_temp_dir(L"restic_test_noappdata", appdata_dir, MAX_PATH);
    use_temp_appdata(appdata_dir, user_dir, MAX_PATH);

    test_plugin_dir_and_default_params();
    test_ini_path_resolution_priority();
    test_restic_binary_resolution();
    test_repositories_parsing_and_wfx_root();
    test_ini_encodings_and_syntax();
    test_root_name();
    test_restic_lookup_ignores_current_directory();
    test_large_repository_list();
    test_wfx_routing_and_callbacks();
    test_missing_ini_is_harmless();
    test_logging();

    RemoveDirectoryW(user_dir);
    RemoveDirectoryW(appdata_dir);

#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    EXPECT_TRUE(_CrtCheckMemory(), "CRT heap integrity check must pass");
#endif

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    } else {
        printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
        return 1;
    }
}
