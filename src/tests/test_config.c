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

/* Test 1: Directory normalization and FsSetDefaultParams extraction */
static void test_plugin_and_fallback_dirs(void) {
    config_set_plugin_dir(L"  \"C:\\TotalCmd\\plugins\\wfx\\restic\\\"  ");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\TotalCmd\\plugins\\wfx\\restic", "Trim and strip trailing backslash");

    config_set_plugin_dir(L"C:\\");
    EXPECT_WSTR_EQ(config_get_plugin_dir(), L"C:\\", "Preserve drive root backslash");

    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = sizeof(dps);
    strncpy_s(dps.DefaultIniName, sizeof(dps.DefaultIniName),
              "C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER\\fsplugin.ini", _TRUNCATE);

    FsSetDefaultParams(&dps);
    EXPECT_WSTR_EQ(
        config_get_fallback_dir(),
        L"C:\\Users\\TestUser\\AppData\\Roaming\\GHISLER",
        "Extract parent directory from DefaultIniName"
    );
}

/* Test 2: INI path resolution (primary plugin dir vs fallback dir) */
static void test_ini_path_resolution_priority(void) {
    WCHAR plugin_dir[MAX_PATH]   = {0};
    WCHAR fallback_dir[MAX_PATH] = {0};
    make_temp_dir(L"restic_test_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_test_fb", fallback_dir, MAX_PATH);

    WCHAR primary_ini[MAX_PATH]  = {0};
    WCHAR fallback_ini[MAX_PATH] = {0};
    swprintf_s(primary_ini, MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(fallback_ini, MAX_PATH, L"%ls\\%ls", fallback_dir, CONFIG_INI_FILENAME);

    DeleteFileW(primary_ini);
    DeleteFileW(fallback_ini);

    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(fallback_dir);

    /* Case A: Neither exists -> returns FALSE and does not create any file on disk */
    WCHAR resolved[MAX_PATH] = {0};
    BOOL ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(!ok, "Should return FALSE when no INI file exists");
    EXPECT_WSTR_EQ(resolved, primary_ini, "Preferred candidate path points to <plugin_dir>\\restic_plugin.ini");
    EXPECT_TRUE(GetFileAttributesW(primary_ini) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(fallback_ini) == INVALID_FILE_ATTRIBUTES,
                "No INI file is auto-created when missing");

    /* Case B: Only fallback INI exists -> resolves to fallback INI */
    write_text_file(fallback_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve fallback INI when primary is missing");
    EXPECT_WSTR_EQ(resolved, fallback_ini, "Resolved path is fallback INI");

    /* Case C: Both exist -> primary INI in plugin_dir takes priority over fallback INI */
    write_text_file(primary_ini, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_ini_path(resolved, MAX_PATH);
    EXPECT_TRUE(ok, "Should resolve primary INI when present");
    EXPECT_WSTR_EQ(resolved, primary_ini, "Primary INI in plugin_dir must take precedence over fallback_dir");

    DeleteFileW(primary_ini);
    DeleteFileW(fallback_ini);
    RemoveDirectoryW(plugin_dir);
    RemoveDirectoryW(fallback_dir);
}

/* Test 3: Restic executable resolution priority */
static void test_restic_binary_resolution(void) {
    WCHAR plugin_dir[CONFIG_MAX_PATH] = {0};
    WCHAR custom_dir[CONFIG_MAX_PATH] = {0};
    make_temp_dir(L"restic_bin_plug", plugin_dir, CONFIG_MAX_PATH);
    make_temp_dir(L"restic_bin_cust", custom_dir, CONFIG_MAX_PATH);

    WCHAR ini_path[CONFIG_MAX_PATH]       = {0};
    WCHAR local_restic[CONFIG_MAX_PATH]   = {0};
    WCHAR custom_restic[CONFIG_MAX_PATH]  = {0};
    swprintf_s(ini_path, CONFIG_MAX_PATH, L"%ls\\%ls", plugin_dir, CONFIG_INI_FILENAME);
    swprintf_s(local_restic, CONFIG_MAX_PATH, L"%ls\\restic.exe", plugin_dir);
    swprintf_s(custom_restic, CONFIG_MAX_PATH, L"%ls\\my_restic.exe", custom_dir);

    /* Create dummy binaries */
    write_text_file(local_restic, "MZ_DUMMY_LOCAL");
    write_text_file(custom_restic, "MZ_DUMMY_CUSTOM");

    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(L"");

    /* Priority 1: Explicit custom path in [Settings] ResticPath */
    char ini_content[2048];
    snprintf(ini_content, sizeof(ini_content),
             "[Settings]\r\nResticPath=\"%ls\"\r\n", custom_restic);
    write_text_file(ini_path, ini_content);

    WCHAR resolved_bin[CONFIG_MAX_PATH] = {0};
    BOOL ok = config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
    EXPECT_TRUE(ok, "Custom ResticPath should resolve");
    EXPECT_WSTR_EQ(resolved_bin, custom_restic, "Explicit ResticPath takes highest priority");

    /* Priority 2: ResticPath is default/empty -> picks <plugin_dir>\restic.exe */
    write_text_file(ini_path, "[Settings]\r\nResticPath=restic.exe\r\n");
    ok = config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
    EXPECT_TRUE(ok, "Local plugin_dir restic.exe should resolve");
    EXPECT_WSTR_EQ(resolved_bin, local_restic, "Should pick <plugin_dir>\\restic.exe");

    /* Priority 3: Remove <plugin_dir>\restic.exe -> falls back to PATH or bare restic.exe */
    DeleteFileW(local_restic);
    config_resolve_restic_path(ini_path, resolved_bin, CONFIG_MAX_PATH);
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
    config_set_fallback_dir(L"");

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
    config_set_fallback_dir(L"");

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
    config_set_fallback_dir(L"");

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

/* Test 8: with no restic_plugin.ini anywhere the plugin stays inert and never writes to disk */
static void test_missing_ini_is_harmless(void) {
    WCHAR plugin_dir[MAX_PATH] = {0}, fallback_dir[MAX_PATH] = {0}, local[MAX_PATH] = {0};
    make_temp_dir(L"restic_noini_plug", plugin_dir, MAX_PATH);
    make_temp_dir(L"restic_noini_fb", fallback_dir, MAX_PATH);
    swprintf_s(local, MAX_PATH, L"%ls\\extracted.txt", plugin_dir);
    config_set_plugin_dir(plugin_dir);
    config_set_fallback_dir(fallback_dir);

    FsDefaultParamStruct dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = (int)sizeof(dps);
    WideCharToMultiByte(CP_ACP, 0, fallback_dir, -1, dps.DefaultIniName, (int)sizeof(dps.DefaultIniName), NULL, NULL);
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
    EXPECT_TRUE(RemoveDirectoryW(fallback_dir), "Fallback directory is still empty (no INI auto-created)");

    /* Verify that the sample restic_plugin.ini the build stages next to this test is valid and all-commented */
    WCHAR sample_ini[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, sample_ini, MAX_PATH);
    WCHAR* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(sample_ini, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Directory of the test executable is known");
    *slash = L'\0';
    wcscat_s(sample_ini, MAX_PATH, L"\\restic_plugin.ini");
    ResticConfig sample_cfg;
    EXPECT_TRUE(config_load_from_file(sample_ini, &sample_cfg) && sample_cfg.repo_count == 0,
                "Staged restic_plugin.ini template exists and has all repositories commented out");
    config_free(&sample_cfg);

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
    config_set_fallback_dir(L"");

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

int main(void) {
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic WFX Configuration Unit Tests...\n");

    test_plugin_and_fallback_dirs();
    test_ini_path_resolution_priority();
    test_restic_binary_resolution();
    test_repositories_parsing_and_wfx_root();
    test_restic_lookup_ignores_current_directory();
    test_large_repository_list();
    test_wfx_routing_and_callbacks();
    test_missing_ini_is_harmless();

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
