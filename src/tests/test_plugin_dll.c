/*
 * Black-box smoke test of the built plugin DLL: it is loaded the way Total Commander loads it
 * (LoadLibrary + GetProcAddress) and driven through the exported WFX entry points only.
 * Expects the plugin (TCResticBrowser.wfx64 or TCResticBrowser.wfx, matching this test's bitness) and the staged
 * pluginst.inf in the directory of this executable, where the build puts them; skipped when the
 * plugin is absent, e.g. in AddressSanitizer test runs. The live tests read tmp\ relative to the
 * current directory, which the build sets to the repository root.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#include <crtdbg.h>
#endif
#include <string.h>
#include <wchar.h>
#include "version.h"

/* Full paths of the built plugin and the staged pluginst.inf next to this executable */
static WCHAR g_plugin_dll[MAX_PATH];
static WCHAR g_pluginst_inf[MAX_PATH];

static int g_tests_run    = 0;
static int g_tests_failed = 0;

/* The last check that finished, for the watchdog (see watchdog_thread) */
static volatile LONG g_checks_done = 0;
static const char* volatile g_last_check_function = "main";
static volatile LONG g_last_check_line = 0;
static const char* volatile g_last_check = "(none yet)";
#define CHECK_DONE(msg) do { \
    g_last_check_function = __FUNCTION__; \
    g_last_check_line     = __LINE__; \
    g_last_check          = (msg); \
    InterlockedIncrement(&g_checks_done); \
} while (0)

#define EXPECT_TRUE(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("[FAIL] %s:%d: %s\n", __FUNCTION__, __LINE__, (msg)); \
        g_tests_failed++; \
        CHECK_DONE(msg); \
        return; \
    } \
    CHECK_DONE(msg); \
} while (0)

#define EXPECT_WSTR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (wcscmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        CHECK_DONE(msg); \
        return; \
    } \
    CHECK_DONE(msg); \
} while (0)

/* Local copies of the WFX constants and prototypes (the plugin is only reachable through GetProcAddress) */
#define RT_Password            2
#define RT_MsgOK               8
#define MSGTYPE_IMPORTANTERROR 6

#define FS_FILE_OK             0
#define FS_FILE_EXISTS         1
#define FS_FILE_NOTFOUND       2
#define FS_FILE_READERROR      3
#define FS_FILE_WRITEERROR     4
#define FS_FILE_USERABORT      5
#define FS_FILE_NOTSUPPORTED   6
#define FS_EXEC_OK             0
#define FS_EXEC_ERROR          1
#define FS_EXEC_YOURSELF       (-1)
#define FS_EXEC_SYMLINK        (-2)

#define FS_COPYFLAGS_OVERWRITE 1
#define FS_COPYFLAGS_RESUME    2
#define FS_COPYFLAGS_MOVE      4

typedef struct {
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} DefaultParams;

typedef struct {
    DWORD SizeLow;
    DWORD SizeHigh;
    FILETIME LastWriteTime;
    int Attr;
} RemoteInfo;

typedef int    (__stdcall *PFN_InitW)(int, void*, void*, void*);
typedef void   (__stdcall *PFN_GetDefRootName)(char*, int);
typedef void   (__stdcall *PFN_SetDefaultParams)(DefaultParams*);
typedef HANDLE (__stdcall *PFN_FindFirstW)(WCHAR*, WIN32_FIND_DATAW*);
typedef HANDLE (__stdcall *PFN_FindFirst)(char*, WIN32_FIND_DATAA*);
typedef BOOL   (__stdcall *PFN_FindNextW)(HANDLE, WIN32_FIND_DATAW*);
typedef int    (__stdcall *PFN_FindClose)(HANDLE);
typedef int    (__stdcall *PFN_GetFile)(char*, char*, int, RemoteInfo*);
typedef int    (__stdcall *PFN_GetFileW)(WCHAR*, WCHAR*, int, RemoteInfo*);
typedef int    (__stdcall *PFN_ExecuteFileW)(HWND, WCHAR*, WCHAR*);
typedef BOOL   (__stdcall *PFN_DisconnectW)(WCHAR*);

typedef struct {
    HMODULE module;
    PFN_InitW init_w;
    PFN_GetDefRootName get_root_name;
    PFN_SetDefaultParams set_default_params;
    PFN_FindFirstW find_first_w;
    PFN_FindFirst find_first;
    PFN_FindNextW find_next_w;
    PFN_FindClose find_close;
    PFN_GetFile get_file;
    PFN_GetFileW get_file_w;
    PFN_ExecuteFileW execute_w;
    PFN_DisconnectW disconnect_w;
} Plugin;

static BOOL make_unique_dir(const WCHAR* tag, WCHAR* out, size_t cap) {
    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(out, cap, L"%ls%ls_%lu", tmp, tag, GetCurrentProcessId());
    return CreateDirectoryW(out, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL write_file(const WCHAR* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0, len = (DWORD)strlen(text);
    BOOL ok = (h != INVALID_HANDLE_VALUE) && WriteFile(h, text, len, &written, NULL) && written == len;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

/* Reads a whole file as a NUL-terminated byte string (caller frees); NULL when missing */
static char* read_text_file(const WCHAR* path) {
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

/* Copies the DLL into dir (so DllMain resolves that directory as the plugin directory) and loads it */
static BOOL load_plugin_copy(const WCHAR* dir, Plugin* p) {
    WCHAR dest[MAX_PATH];
    memset(p, 0, sizeof(*p));
    swprintf_s(dest, MAX_PATH, L"%ls\\" RESTIC_WFX_FILE_NAME, dir);
    if (!CopyFileW(g_plugin_dll, dest, FALSE)) return FALSE;
    p->module = LoadLibraryW(dest);
    if (!p->module) return FALSE;
    p->init_w            = (PFN_InitW)(void*)GetProcAddress(p->module, "FsInitW");
    p->get_root_name     = (PFN_GetDefRootName)(void*)GetProcAddress(p->module, "FsGetDefRootName");
    p->set_default_params = (PFN_SetDefaultParams)(void*)GetProcAddress(p->module, "FsSetDefaultParams");
    p->find_first_w      = (PFN_FindFirstW)(void*)GetProcAddress(p->module, "FsFindFirstW");
    p->find_first        = (PFN_FindFirst)(void*)GetProcAddress(p->module, "FsFindFirst");
    p->find_next_w       = (PFN_FindNextW)(void*)GetProcAddress(p->module, "FsFindNextW");
    p->find_close        = (PFN_FindClose)(void*)GetProcAddress(p->module, "FsFindClose");
    p->get_file          = (PFN_GetFile)(void*)GetProcAddress(p->module, "FsGetFile");
    p->get_file_w        = (PFN_GetFileW)(void*)GetProcAddress(p->module, "FsGetFileW");
    p->execute_w         = (PFN_ExecuteFileW)(void*)GetProcAddress(p->module, "FsExecuteFileW");
    p->disconnect_w      = (PFN_DisconnectW)(void*)GetProcAddress(p->module, "FsDisconnectW");
    return TRUE;
}

static void unload_plugin_copy(Plugin* p, const WCHAR* dir) {
    WCHAR dest[MAX_PATH];
    if (p->module) FreeLibrary(p->module);
    p->module = NULL;
    swprintf_s(dest, MAX_PATH, L"%ls\\" RESTIC_WFX_FILE_NAME, dir);
    DeleteFileW(dest);
}

/* Exact export surface: read-only + file extraction entry points present, write/delete/move absent */
static void test_export_surface(void) {
    HMODULE m = LoadLibraryW(g_plugin_dll);
    EXPECT_TRUE(m != NULL, "Plugin DLL loads");

    const char* required[] = {"FsInit", "FsInitW", "FsFindFirst", "FsFindFirstW", "FsFindNext",
                              "FsFindNextW", "FsFindClose", "FsGetDefRootName", "FsSetDefaultParams",
                              "FsGetFile", "FsGetFileW", "FsExecuteFile", "FsExecuteFileW",
                              "FsDisconnect", "FsDisconnectW"};
    for (size_t i = 0; i < _countof(required); ++i) {
        FARPROC fn = GetProcAddress(m, required[i]);
        g_tests_run++;
        if (!fn) {
            printf("[FAIL] test_export_surface: missing export %s\n", required[i]);
            g_tests_failed++;
        }
    }

    /* Modification/deletion/move functions must NOT be exported: Total Commander would call them */
    const char* forbidden[] = {"FsPutFile", "FsPutFileW", "FsDeleteFile", "FsDeleteFileW",
                               "FsMkDir", "FsMkDirW", "FsRemoveDir", "FsRemoveDirW",
                               "FsRenMovFile", "FsRenMovFileW", "FsSetAttr", "FsSetTime",
                               "json_parse", "json_free", "DllMain"};
    for (size_t i = 0; i < _countof(forbidden); ++i) {
        FARPROC fn = GetProcAddress(m, forbidden[i]);
        g_tests_run++;
        if (fn) {
            printf("[FAIL] test_export_surface: unexpected export %s\n", forbidden[i]);
            g_tests_failed++;
        }
    }

    /* Verify embedded plugin icon resource (ID 1, 5 frames: 16x16, 24x24, 32x32, 48x48, 256x256) */
    HRSRC hGroup = FindResourceW(m, MAKEINTRESOURCEW(1), (LPCWSTR)RT_GROUP_ICON);
    EXPECT_TRUE(hGroup != NULL && SizeofResource(m, hGroup) >= 6 + 5 * 14,
                "Embedded RT_GROUP_ICON #1 present with 5 icon frames");
    for (WORD id = 1; id <= 5; ++id) {
        HRSRC hIcon = FindResourceW(m, MAKEINTRESOURCEW(id), (LPCWSTR)RT_ICON);
        EXPECT_TRUE(hIcon != NULL && SizeofResource(m, hIcon) > 0, "Embedded RT_ICON frame present");
    }

    /* Verify embedded VS_VERSION_INFO resource (FileVersion, ProductName, VS_FIXEDFILEINFO signature) */
    HRSRC hVer = FindResourceW(m, MAKEINTRESOURCEW(VS_VERSION_INFO), (LPCWSTR)RT_VERSION);
    DWORD verSize = hVer ? SizeofResource(m, hVer) : 0;
    HGLOBAL hVerMem = hVer ? LoadResource(m, hVer) : NULL;
    const BYTE* verBytes = hVerMem ? (const BYTE*)LockResource(hVerMem) : NULL;
    EXPECT_TRUE(verBytes != NULL && verSize >= 92, "Embedded RT_VERSION (VS_VERSION_INFO) resource present");
    BOOL foundFixed = FALSE;
    DWORD expectedMS = ((DWORD)RESTIC_WFX_VER_MAJOR << 16) | (DWORD)RESTIC_WFX_VER_MINOR;
    DWORD expectedLS = ((DWORD)RESTIC_WFX_VER_PATCH << 16) | (DWORD)RESTIC_WFX_VER_BUILD;
    for (DWORD off = 0; verBytes && off + sizeof(VS_FIXEDFILEINFO) <= verSize; off += 4) {
        const VS_FIXEDFILEINFO* ffi = (const VS_FIXEDFILEINFO*)(verBytes + off);
        if (ffi->dwSignature == 0xFEEF04BDu) {
            foundFixed = (ffi->dwFileVersionMS == expectedMS && ffi->dwFileVersionLS == expectedLS &&
                          ffi->dwProductVersionMS == expectedMS && ffi->dwProductVersionLS == expectedLS &&
                          ffi->dwFileType == VFT_DLL);
            break;
        }
    }
    EXPECT_TRUE(foundFixed, "VS_FIXEDFILEINFO matches RESTIC_WFX_VERSION_CSV and VFT_DLL");

    /* Verify the staged pluginst.inf manifest matches Total Commander auto-install specification */
    WCHAR infType[32] = {0}, infFile[64] = {0}, infVer[32] = {0};
    GetPrivateProfileStringW(L"plugininstall", L"type", L"", infType, _countof(infType), g_pluginst_inf);
    GetPrivateProfileStringW(L"plugininstall", L"file", L"", infFile, _countof(infFile), g_pluginst_inf);
    GetPrivateProfileStringW(L"plugininstall", L"version", L"", infVer, _countof(infVer), g_pluginst_inf);
    /* Checked one by one so a failure shows the value actually read (empty: no [plugininstall]) */
    EXPECT_WSTR_EQ(infType, L"wfx", "Staged pluginst.inf: type=");
    EXPECT_WSTR_EQ(infFile, L"TCResticBrowser.wfx", "Staged pluginst.inf: file= names the 32-bit plugin DLL");
    EXPECT_WSTR_EQ(infVer, L"" RESTIC_WFX_PRODUCT_VER, "Staged pluginst.inf: version= matches version.h");

    FreeLibrary(m);
}

/* Scenario A: TCResticBrowser.ini sits next to the DLL (no per-user INI) */
static void test_primary_ini_next_to_dll(void) {
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_primary", dir, MAX_PATH), "Create plugin directory");
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);
    EXPECT_TRUE(write_file(ini, "[Repositories]\r\nAlpha=D:\\r1\r\nBeta=D:\\r2\r\n"), "Write INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy");
    EXPECT_TRUE(p.init_w && p.get_root_name && p.set_default_params && p.find_first_w && p.find_next_w &&
                p.find_close && p.find_first, "Entry points resolved");

    char root_name[64] = {0};
    p.get_root_name(root_name, (int)sizeof(root_name));
    EXPECT_TRUE(strcmp(root_name, "TCResticBrowser") == 0, "Default root name");
    char tiny[5] = {'x', 'x', 'x', 'x', 'x'};
    p.get_root_name(tiny, (int)sizeof(tiny));
    EXPECT_TRUE(strcmp(tiny, "TCRe") == 0, "Root name truncated and terminated for small buffers");

    EXPECT_TRUE(p.init_w(1, NULL, NULL, NULL) == 0, "FsInitW returns 0");

    WIN32_FIND_DATAW fd;
    HANDLE h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Root enumeration starts");
    EXPECT_TRUE(wcsncmp(fd.cFileName, L"\U0001F5D1", 2) != 0, "No forget entry while no password is cached");
    EXPECT_WSTR_EQ(fd.cFileName, L"Alpha", "First alias");
    EXPECT_TRUE((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, "Alias is a directory");
    EXPECT_TRUE(p.find_next_w(h, &fd), "Second alias available");
    EXPECT_WSTR_EQ(fd.cFileName, L"Beta", "Second alias");
    EXPECT_TRUE(!p.find_next_w(h, &fd), "End of list");
    EXPECT_TRUE(p.find_close(h) == 0, "FsFindClose returns 0");

    /* Two simultaneous enumerations (both panels) keep independent state */
    WIN32_FIND_DATAW fd2;
    HANDLE h1 = p.find_first_w(L"\\", &fd);
    HANDLE h2 = p.find_first_w(L"\\", &fd2);
    EXPECT_TRUE(h1 != INVALID_HANDLE_VALUE && h2 != INVALID_HANDLE_VALUE && h1 != h2, "Two live handles");
    EXPECT_TRUE(p.find_next_w(h1, &fd), "Advance first handle");
    EXPECT_WSTR_EQ(fd.cFileName, L"Beta", "First handle advanced");
    EXPECT_TRUE(p.find_next_w(h2, &fd2), "Advance second handle");
    EXPECT_WSTR_EQ(fd2.cFileName, L"Beta", "Second handle independent");
    p.find_close(h1);
    p.find_close(h2);

    /* Unknown alias and the ANSI stub fail cleanly instead of crashing */
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\NoSuchAlias", &fd) == INVALID_HANDLE_VALUE, "Unknown alias refused");
    EXPECT_TRUE(GetLastError() == ERROR_PATH_NOT_FOUND, "Unknown alias sets ERROR_PATH_NOT_FOUND");
    WIN32_FIND_DATAA fda;
    char root_a[] = "\\";
    EXPECT_TRUE(p.find_first(root_a, &fda) == INVALID_HANDLE_VALUE, "ANSI stub refuses to enumerate");
    EXPECT_TRUE(p.find_close(INVALID_HANDLE_VALUE) == 0 && p.find_close(NULL) == 0, "FsFindClose tolerates invalid handles");

    unload_plugin_copy(&p, dir);
    DeleteFileW(ini);
    RemoveDirectoryW(dir);
}

/* %APPDATA%\GHISLER of this test run (set up in main, so the developer's own INI is never read) */
static WCHAR g_user_dir[MAX_PATH];

/*
 * Scenario B: the per-user %APPDATA%\GHISLER\TCResticBrowser.ini wins over the INI next to the DLL;
 * DefaultIniName from FsSetDefaultParams is ignored and no INI is ever auto-created.
 */
static void test_user_ini_overrides_plugin_ini(void) {
    WCHAR dir[MAX_PATH], plug_ini[MAX_PATH], user_ini[MAX_PATH], tc_dir[MAX_PATH], tc_ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_userini", dir, MAX_PATH) &&
                make_unique_dir(L"restic_dll_tcconfig", tc_dir, MAX_PATH), "Create directories");
    swprintf_s(plug_ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);
    swprintf_s(user_ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", g_user_dir);
    swprintf_s(tc_ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", tc_dir);
    DeleteFileW(plug_ini);
    DeleteFileW(user_ini);

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy");
    EXPECT_TRUE(p.init_w && p.set_default_params && p.find_first_w && p.find_next_w && p.find_close,
                "Entry points resolved");

    /* Total Commander's settings folder pointing elsewhere (e.g. a portable install) is not consulted */
    EXPECT_TRUE(write_file(tc_ini, "[Repositories]\r\nFromDefaultIniName=D:\\repo\r\n"), "Write INI next to wincmd.ini");
    DefaultParams dps;
    memset(&dps, 0, sizeof(dps));
    dps.size = (int)sizeof(dps);
    WideCharToMultiByte(CP_ACP, 0, tc_dir, -1, dps.DefaultIniName, (int)sizeof(dps.DefaultIniName), NULL, NULL);
    strcat_s(dps.DefaultIniName, sizeof(dps.DefaultIniName), "\\wincmd.ini");
    p.init_w(1, NULL, NULL, NULL);
    p.set_default_params(&dps);

    WIN32_FIND_DATAW fd;
    SetLastError(0);
    HANDLE h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES,
                "No INI in %APPDATA%\\GHISLER or the plugin directory -> empty root");
    EXPECT_TRUE(GetFileAttributesW(plug_ini) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(user_ini) == INVALID_FILE_ATTRIBUTES,
                "No INI file is auto-created on disk");

    /* The shipped INI next to the DLL is used while there is no per-user one */
    EXPECT_TRUE(write_file(plug_ini, "[Repositories]\r\nShipped=D:\\repo\r\n"), "Write plugin-directory INI");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Plugin-directory INI is used");
    EXPECT_WSTR_EQ(fd.cFileName, L"Shipped", "Alias from the plugin-directory INI");
    p.find_close(h);

    /* A per-user INI appearing later takes over in full on the next listing, without a reload */
    EXPECT_TRUE(write_file(user_ini, "[Repositories]\r\nPersonal=D:\\repo\r\n"), "Write per-user INI");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Per-user INI is picked up without reloading the plugin");
    EXPECT_WSTR_EQ(fd.cFileName, L"Personal", "Alias from the per-user INI");
    EXPECT_TRUE(!p.find_next_w(h, &fd), "Plugin-directory INI is not merged in");
    p.find_close(h);

    /* RootName from the active INI is what the installer gets from FsGetDefRootName */
    char root_name[64] = {0};
    p.get_root_name(root_name, (int)sizeof(root_name));
    EXPECT_TRUE(strcmp(root_name, "TCResticBrowser") == 0, "Default root name without RootName");
    EXPECT_TRUE(write_file(user_ini, "[Settings]\r\nRootName=Personal Backups\r\n[Repositories]\r\nPersonal=D:\\repo\r\n"),
                "Write per-user INI with RootName");
    p.get_root_name(root_name, (int)sizeof(root_name));
    EXPECT_TRUE(strcmp(root_name, "Personal Backups") == 0, "Configured RootName reported by FsGetDefRootName");

    unload_plugin_copy(&p, dir);
    DeleteFileW(user_ini);
    DeleteFileW(plug_ini);
    DeleteFileW(tc_ini);
    RemoveDirectoryW(dir);
    RemoveDirectoryW(tc_dir);
}

/* Mock Total Commander callback state for Scenario C (end-to-end live navigation) */
#define FIXTURE_PASSWORD L"p@ss \"w0rd\" \u00fc %PATH% \u65e5\u672c"

static int   g_e2e_pwd_prompts      = 0;
static int   g_e2e_msg_dialogs      = 0;
static int   g_e2e_error_logs       = 0;
static int   g_e2e_progress_calls   = 0;
static BOOL  g_e2e_force_abort      = FALSE;
static BOOL  g_e2e_abort_after_start = FALSE; /* abort once the transfer reports progress above 0 % */
static int   g_e2e_pwd_mode         = 0; /* 0 = wrong once then correct, 1 = always correct, 2 = always wrong,
                                           3 = user submits an empty password, 4 = user cancels the dialog,
                                           5 = correct password with a trailing space once, then correct */
static WCHAR g_e2e_last_prompt[256] = {0};
static WCHAR g_e2e_last_title[512] = {0};
static WCHAR g_e2e_last_msg[1024]   = {0}; /* report_plugin_error() writes up to 1024 */
static WCHAR g_e2e_progress_target[512] = {0};
static int   g_e2e_progress_percent = -1;
static volatile LONG g_e2e_mt_failures = 0;

static int __stdcall e2e_progress_proc(int PluginNr, WCHAR* SourceName, WCHAR* TargetName, int PercentDone) {
    (void)PluginNr;
    (void)SourceName;
    if (TargetName) {
        wcsncpy_s(g_e2e_progress_target, _countof(g_e2e_progress_target), TargetName, _TRUNCATE);
    }
    g_e2e_progress_percent = PercentDone;
    g_e2e_progress_calls++;
    if (g_e2e_abort_after_start && PercentDone > 0) return 1;
    return g_e2e_force_abort ? 1 : 0;
}

/*
 * Everything the plugin logged through LogProc other than important errors, one "<MsgType>:<text>"
 * line each. LogProc is also called from the plugin's background threads, hence the lock.
 */
static WCHAR   g_e2e_conn_log[1 << 16];
static SRWLOCK g_e2e_conn_log_lock = SRWLOCK_INIT;

static void __stdcall e2e_log_proc(int PluginNr, int MsgType, WCHAR* LogString) {
    (void)PluginNr;
    if (MsgType == MSGTYPE_IMPORTANTERROR && LogString) {
        g_e2e_error_logs++;
    } else if (LogString) {
        WCHAR line[1100];
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"%d:%ls\n", MsgType, LogString);
        AcquireSRWLockExclusive(&g_e2e_conn_log_lock);
        if (wcslen(g_e2e_conn_log) + wcslen(line) < _countof(g_e2e_conn_log)) {
            wcscat_s(g_e2e_conn_log, _countof(g_e2e_conn_log), line);
        }
        ReleaseSRWLockExclusive(&g_e2e_conn_log_lock);
    }
}

/* Number of times `text` occurs in what LogProc received */
static int conn_log_count(const WCHAR* text) {
    int n = 0;
    AcquireSRWLockShared(&g_e2e_conn_log_lock);
    for (const WCHAR* p = g_e2e_conn_log; (p = wcsstr(p, text)) != NULL; p++) n++;
    ReleaseSRWLockShared(&g_e2e_conn_log_lock);
    return n;
}

/* Answers to the S3 secret key dialog, one per prompt (NULL or past the end: FIXTURE_PASSWORD) */
static const WCHAR* g_e2e_s3_answers[4];
static int          g_e2e_s3_prompts = 0;

static BOOL __stdcall e2e_request_proc(
    int PluginNr,
    int RequestType,
    WCHAR* CustomTitle,
    WCHAR* CustomText,
    WCHAR* ReturnedText,
    int maxlen
) {
    (void)PluginNr;
    if (RequestType == RT_Password) {
        g_e2e_pwd_prompts++;
        wcsncpy_s(g_e2e_last_title, _countof(g_e2e_last_title), CustomTitle ? CustomTitle : L"", _TRUNCATE);
        if (CustomText) {
            wcsncpy_s(g_e2e_last_prompt, _countof(g_e2e_last_prompt), CustomText, _TRUNCATE);
        }
        if (CustomTitle && wcsstr(CustomTitle, L"S3 secret access key")) {
            int i = g_e2e_s3_prompts++;
            const WCHAR* answer = i < (int)_countof(g_e2e_s3_answers) && g_e2e_s3_answers[i] ? g_e2e_s3_answers[i]
                                                                                            : FIXTURE_PASSWORD;
            wcsncpy_s(ReturnedText, (size_t)maxlen, answer, _TRUNCATE);
            return TRUE;
        }
        if (g_e2e_pwd_mode == 3) {
            ReturnedText[0] = L'\0';
            return TRUE;
        }
        if (g_e2e_pwd_mode == 4) {
            return FALSE;
        }
        if (g_e2e_pwd_mode == 5 && g_e2e_pwd_prompts == 1) {
            _snwprintf_s(ReturnedText, (size_t)maxlen, _TRUNCATE, L"%ls ", FIXTURE_PASSWORD);
            return TRUE;
        }
        if (g_e2e_pwd_mode == 0 && g_e2e_pwd_prompts == 1) {
            wcsncpy_s(ReturnedText, (size_t)maxlen, L"wrong_first_attempt", _TRUNCATE);
            return TRUE;
        }
        if (g_e2e_pwd_mode == 2) {
            wcsncpy_s(ReturnedText, (size_t)maxlen, L"always_wrong", _TRUNCATE);
            return TRUE;
        }
        wcsncpy_s(ReturnedText, (size_t)maxlen, FIXTURE_PASSWORD, _TRUNCATE);
        return TRUE;
    }
    if (RequestType == RT_MsgOK) {
        g_e2e_msg_dialogs++;
        if (CustomText) {
            wcsncpy_s(g_e2e_last_msg, _countof(g_e2e_last_msg), CustomText, _TRUNCATE);
        }
        return TRUE;
    }
    return FALSE;
}

/* Worker for the concurrency scenario: lists one repository a few times and counts failures */
typedef struct {
    const Plugin* plugin;
    const WCHAR*  path;
} MtJob;

static DWORD WINAPI mt_list_worker(LPVOID param) {
    const MtJob* job = (const MtJob*)param;
    for (int i = 0; i < 3; ++i) {
        WIN32_FIND_DATAW fd;
        HANDLE h = job->plugin->find_first_w((WCHAR*)job->path, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            InterlockedIncrement(&g_e2e_mt_failures);
            continue;
        }
        int entries = 1;
        while (job->plugin->find_next_w(h, &fd)) entries++;
        job->plugin->find_close(h);
        if (entries != 5) InterlockedIncrement(&g_e2e_mt_failures); /* Two snapshots, latest and its two aliases */
    }
    return 0;
}

/*
 * Simulates Total Commander's native F5 copy of a directory: walks the remote tree via
 * FsFindFirstW/FsFindNextW/FsFindClose, creates local directories, and calls FsGetFileW for files.
 */
static BOOL tc_copy_folder_recursive(
    const Plugin* p,
    const WCHAR* remote_dir,
    const WCHAR* local_dir,
    int* out_dirs,
    int* out_files
) {
    if (!CreateDirectoryW(local_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return FALSE;
    }
    (*out_dirs)++;

    WIN32_FIND_DATAW fd;
    HANDLE h = p->find_first_w((WCHAR*)remote_dir, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_NO_MORE_FILES; /* Empty directory is valid */
    }

    BOOL ok = TRUE;
    do {
        WCHAR r_child[MAX_PATH * 2], l_child[MAX_PATH * 2];
        swprintf_s(r_child, _countof(r_child), L"%ls\\%ls", remote_dir, fd.cFileName);
        swprintf_s(l_child, _countof(l_child), L"%ls\\%ls", local_dir, fd.cFileName);

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (!tc_copy_folder_recursive(p, r_child, l_child, out_dirs, out_files)) {
                ok = FALSE;
                break;
            }
        } else {
            RemoteInfo ri;
            ri.SizeLow       = fd.nFileSizeLow;
            ri.SizeHigh      = fd.nFileSizeHigh;
            ri.LastWriteTime = fd.ftLastWriteTime;
            ri.Attr          = (int)fd.dwFileAttributes;
            if (p->get_file_w(r_child, l_child, FS_COPYFLAGS_OVERWRITE, &ri) != FS_FILE_OK) {
                ok = FALSE;
                break;
            }
            (*out_files)++;
        }
    } while (p->find_next_w(h, &fd));

    p->find_close(h);
    return ok;
}

static BOOL verify_pattern_file(const WCHAR* path, DWORD size);

/* Exercises FsGetFileW (single file, overwrite protocol, move rejection, missing file) and recursive folder copy */
static void test_e2e_file_and_folder_extraction(const Plugin* p, const WCHAR* dir, const WCHAR* data_path) {
    WCHAR local_readme[MAX_PATH];
    swprintf_s(local_readme, _countof(local_readme), L"%ls\\extracted_readme.txt", dir);
    DeleteFileW(local_readme);

    WCHAR remote_readme[MAX_PATH * 2];
    swprintf_s(remote_readme, _countof(remote_readme), L"%ls\\readme.txt", data_path);

    RemoteInfo ri = {5, 0, {0x802D2000, 0x01D5EF01}, FILE_ATTRIBUTE_READONLY};
    g_e2e_progress_percent = -1;
    EXPECT_TRUE(p->get_file_w(remote_readme, local_readme, 0, &ri) == FS_FILE_OK,
                "FsGetFileW extracts readme.txt");
    EXPECT_TRUE(g_e2e_progress_percent == 100, "FsGetFileW reports 100% completion to ProgressProcW");

    /* Calling FsGetFileW again without FS_COPYFLAGS_OVERWRITE returns FS_FILE_EXISTS (TC overwrite prompt) */
    EXPECT_TRUE(p->get_file_w(remote_readme, local_readme, 0, &ri) == FS_FILE_EXISTS,
                "Existing local file without OVERWRITE flag returns FS_FILE_EXISTS");
    /* With FS_COPYFLAGS_OVERWRITE it succeeds */
    EXPECT_TRUE(p->get_file_w(remote_readme, local_readme, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK,
                "Existing local file with FS_COPYFLAGS_OVERWRITE is overwritten");

    /* Move (F6) and resume flags are rejected because snapshots are read-only */
    EXPECT_TRUE(p->get_file_w(remote_readme, local_readme, FS_COPYFLAGS_MOVE, &ri) == FS_FILE_NOTSUPPORTED,
                "FS_COPYFLAGS_MOVE returns FS_FILE_NOTSUPPORTED");
    EXPECT_TRUE(p->get_file_w(remote_readme, local_readme, FS_COPYFLAGS_RESUME, &ri) == FS_FILE_NOTSUPPORTED,
                "FS_COPYFLAGS_RESUME returns FS_FILE_NOTSUPPORTED");
    EXPECT_TRUE(p->get_file("a", "b", 0, &ri) == FS_FILE_NOTSUPPORTED,
                "ANSI FsGetFile stub returns FS_FILE_NOTSUPPORTED");

    /* Non-existent remote file -> FS_FILE_NOTFOUND */
    WCHAR remote_missing[MAX_PATH * 2], local_missing[MAX_PATH];
    swprintf_s(remote_missing, _countof(remote_missing), L"%ls\\no_such.txt", data_path);
    swprintf_s(local_missing, _countof(local_missing), L"%ls\\no_such.txt", dir);
    EXPECT_TRUE(p->get_file_w(remote_missing, local_missing, 0, &ri) == FS_FILE_NOTFOUND &&
                GetFileAttributesW(local_missing) == INVALID_FILE_ATTRIBUTES,
                "Missing snapshot file returns FS_FILE_NOTFOUND without creating local file");

    /* Recursive folder copy (simulating Total Commander copying the "sub" folder and all its contents) */
    WCHAR remote_sub[MAX_PATH * 2], local_sub[MAX_PATH];
    swprintf_s(remote_sub, _countof(remote_sub), L"%ls\\sub", data_path);
    swprintf_s(local_sub, _countof(local_sub), L"%ls\\copied_sub", dir);
    int copied_dirs = 0, copied_files = 0;
    EXPECT_TRUE(tc_copy_folder_recursive(p, remote_sub, local_sub, &copied_dirs, &copied_files),
                "Recursive folder copy via FsFindFirstW + FsGetFileW succeeds");
    EXPECT_TRUE(copied_dirs == 2 && copied_files == 3,
                "Copied 'sub', 'sub\\sub2', 'inner.txt', 'deep.txt' and 'binary.dat'");
    WCHAR deep_local[MAX_PATH], inner_local[MAX_PATH];
    swprintf_s(inner_local, _countof(inner_local), L"%ls\\inner.txt", local_sub);
    swprintf_s(deep_local, _countof(deep_local), L"%ls\\sub2\\deep.txt", local_sub);
    EXPECT_TRUE(GetFileAttributesW(inner_local) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(deep_local) != INVALID_FILE_ATTRIBUTES,
                "Extracted nested files exist on disk");

    WCHAR binary_local[MAX_PATH];
    swprintf_s(binary_local, _countof(binary_local), L"%ls\\sub2\\binary.dat", local_sub);
    EXPECT_TRUE(verify_pattern_file(binary_local, 70000), "Binary file inside the copied folder is intact");
    DeleteFileW(binary_local);
    DeleteFileW(deep_local);
    DeleteFileW(inner_local);
    WCHAR sub2_local[MAX_PATH];
    swprintf_s(sub2_local, _countof(sub2_local), L"%ls\\sub2", local_sub);
    RemoveDirectoryW(sub2_local);
    RemoveDirectoryW(local_sub);
    DeleteFileW(local_readme);
}

/* Deterministic binary payload used by the fixture (0x00, 0x0A, 0x0D and 0x1A included) */
static unsigned char pattern_byte(unsigned long i) {
    return (unsigned char)((i * 7u + i / 251u) & 0xFFu);
}

static BOOL verify_pattern_file(const WCHAR* path, DWORD size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    DWORD pos = 0, got = 0;
    unsigned char buf[4096];
    while (ok && ReadFile(h, buf, sizeof(buf), &got, NULL) && got > 0) {
        for (DWORD i = 0; ok && i < got; ++i) ok = (buf[i] == pattern_byte(pos + i));
        pos += got;
    }
    CloseHandle(h);
    return ok && pos == size;
}

static BOOL file_has_text(const WCHAR* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[64] = {0};
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got == strlen(text) && memcmp(buf, text, got) == 0;
    CloseHandle(h);
    return ok;
}

static int count_partial_files(const WCHAR* dir) {
    WCHAR pattern[MAX_PATH];
    swprintf_s(pattern, MAX_PATH, L"%ls\\~rp*.tmp", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do { n++; } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* Copies the first `segments` backslash-separated components of a virtual path ("\A\B\C", 2 -> "\A\B") */
static void virtual_path_prefix(const WCHAR* path, int segments, WCHAR* out, size_t cap) {
    size_t n = 0;
    int seen = 0;
    for (; path[n] != L'\0' && n + 1 < cap; ++n) {
        if (path[n] == L'\\' && ++seen > segments) break;
        out[n] = path[n];
    }
    out[n] = L'\0';
}

/* FsGetFileW edge cases: data fidelity, failure atomicity, argument validation, abort, password flows */
static void test_e2e_extraction_edge_cases(const Plugin* p, const WCHAR* dir, const WCHAR* data_path) {
    const int saved_prompts = g_e2e_pwd_prompts, saved_dialogs = g_e2e_msg_dialogs;
    const int saved_logs = g_e2e_error_logs, saved_mode = g_e2e_pwd_mode;
    RemoteInfo ri = {5, 0, {0x802D2000, 0x01D5EF01}, FILE_ATTRIBUTE_READONLY};
    WCHAR remote[MAX_PATH * 2], local[MAX_PATH], prefix[MAX_PATH * 2];

    /* Binary fidelity and mtime preservation */
    RemoteInfo ri_bin = {70000, 0, {0x802D2000, 0x01D5EF01}, 0};
    swprintf_s(remote, _countof(remote), L"%ls\\sub\\sub2\\binary.dat", data_path);
    swprintf_s(local, _countof(local), L"%ls\\bin.dat", dir);
    DeleteFileW(local);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri_bin) == FS_FILE_OK && verify_pattern_file(local, 70000),
                "Binary file (0x00/0x0A/0x0D/0x1A) is extracted byte for byte");
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(GetFileAttributesExW(local, GetFileExInfoStandard, &fad) &&
                fad.ftLastWriteTime.dwLowDateTime == ri_bin.LastWriteTime.dwLowDateTime &&
                fad.ftLastWriteTime.dwHighDateTime == ri_bin.LastWriteTime.dwHighDateTime,
                "LastWriteTime from RemoteInfoStruct is applied to the extracted file");

    /* A failed overwrite must leave the previous file untouched */
    swprintf_s(remote, _countof(remote), L"%ls\\no_such.bin", data_path);
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTFOUND &&
                verify_pattern_file(local, 70000), "Missing remote file does not destroy the existing local file");

    /* MOVE is refused even together with OVERWRITE and never touches the target */
    swprintf_s(remote, _countof(remote), L"%ls\\readme.txt", data_path);
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_MOVE | FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTSUPPORTED &&
                verify_pattern_file(local, 70000), "MOVE|OVERWRITE -> FS_FILE_NOTSUPPORTED, target untouched");

    /* Local-side failures map to FS_FILE_WRITEERROR without popups */
    EXPECT_TRUE(p->get_file_w(remote, (WCHAR*)dir, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_WRITEERROR,
                "A directory as LocalName -> FS_FILE_WRITEERROR");
    swprintf_s(local, _countof(local), L"%ls\\no_such_subdir\\x.txt", dir);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_WRITEERROR,
                "Non-existent local directory -> FS_FILE_WRITEERROR");

    /* Target names Windows would map to a stream, a device or another file are refused up front */
    static const WCHAR* const unsafe_leaves[] = {L"carrier.txt:hidden", L"CON", L"nul.txt", L"COM1", L"trail."};
    WCHAR carrier[MAX_PATH], other[MAX_PATH];
    swprintf_s(carrier, _countof(carrier), L"%ls\\carrier.txt", dir);
    swprintf_s(other, _countof(other), L"%ls\\trail", dir);
    EXPECT_TRUE(write_file(carrier, "keep"), "Carrier file for the stream test is created");
    DeleteFileW(other);
    for (size_t i = 0; i < _countof(unsafe_leaves); ++i) {
        swprintf_s(local, _countof(local), L"%ls\\%ls", dir, unsafe_leaves[i]);
        EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_WRITEERROR,
                    "A target name Windows cannot store as is -> FS_FILE_WRITEERROR");
    }
    swprintf_s(local, _countof(local), L"%ls:hidden", carrier);
    EXPECT_TRUE(GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES && file_has_text(carrier, "keep") &&
                GetFileAttributesW(other) == INVALID_FILE_ATTRIBUTES,
                "No alternate data stream is written and \"trail.\" does not become \"trail\"");
    DeleteFileW(carrier);
    EXPECT_TRUE(g_e2e_msg_dialogs == saved_dialogs, "Local-side failures do not raise Restic error popups");

    /* RemoteInfoStruct variants */
    swprintf_s(local, _countof(local), L"%ls\\noinfo.txt", dir);
    DeleteFileW(local);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, NULL) == FS_FILE_OK && file_has_text(local, "xxxxx"),
                "ri == NULL is tolerated");
    RemoteInfo ri_unknown = {5, 0, {0xFFFFFFFE, 0xFFFFFFFF}, 0};
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri_unknown) == FS_FILE_OK &&
                GetFileAttributesExW(local, GetFileExInfoStandard, &fad) &&
                fad.ftLastWriteTime.dwHighDateTime != 0xFFFFFFFF,
                "The 'unknown time' marker is not written to the file");
    RemoteInfo ri_dir = {0, 0, {0, 0}, FILE_ATTRIBUTE_DIRECTORY};
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri_dir) == FS_FILE_NOTSUPPORTED,
                "Directories are never extracted through FsGetFileW");
    SetFileAttributesW(local, FILE_ATTRIBUTE_READONLY);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_EXISTS, "Read-only target reported as existing");
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK &&
                (GetFileAttributesW(local) & FILE_ATTRIBUTE_READONLY) == 0,
                "OVERWRITE replaces a read-only target");
    DeleteFileW(local);

    /* Invalid remote names never reach Restic */
    WCHAR* bad_remote[6];
    WCHAR snap_level[MAX_PATH * 2], alias_level[MAX_PATH * 2], no_alias[] = L"\\NoSuchAlias\\x\\y";
    virtual_path_prefix(data_path, 2, snap_level, _countof(snap_level));
    virtual_path_prefix(data_path, 1, alias_level, _countof(alias_level));
    swprintf_s(prefix, _countof(prefix), L"\\MainRepo\\not-a-snapshot\\file");
    bad_remote[0] = L"\\";
    bad_remote[1] = alias_level;
    bad_remote[2] = snap_level;
    bad_remote[3] = no_alias;
    bad_remote[4] = prefix;
    bad_remote[5] = L"";
    swprintf_s(local, _countof(local), L"%ls\\never.txt", dir);
    for (int i = 0; i < 6; ++i) {
        g_tests_run++;
        DeleteFileW(local);
        if (p->get_file_w(bad_remote[i], local, 0, &ri) != FS_FILE_NOTFOUND ||
            GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES) {
            printf("[FAIL] %s:%d: invalid remote name #%d must give FS_FILE_NOTFOUND\n", __FUNCTION__, __LINE__, i);
            g_tests_failed++;
        }
    }
    EXPECT_TRUE(p->get_file_w(NULL, local, 0, &ri) == FS_FILE_NOTFOUND &&
                p->get_file_w(remote, NULL, 0, &ri) == FS_FILE_NOTFOUND && g_e2e_pwd_prompts == saved_prompts,
                "NULL arguments are rejected without prompting");

    /* ESC: before the transfer, and in the middle of it (with and without an existing target) */
    swprintf_s(local, _countof(local), L"%ls\\esc.bin", dir);
    DeleteFileW(local);
    g_e2e_force_abort = TRUE;
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_USERABORT &&
                GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES, "ESC at the start -> FS_FILE_USERABORT");
    g_e2e_force_abort = FALSE;
    RemoteInfo ri_big = {100000, 0, {0x802D2000, 0x01D5EF01}, 0};
    swprintf_s(remote, _countof(remote), L"%ls\\big.bin", data_path);
    g_e2e_abort_after_start = TRUE;
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri_big) == FS_FILE_USERABORT &&
                GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES && count_partial_files(dir) == 0,
                "ESC in the middle of a transfer leaves neither target nor temporary file");
    swprintf_s(local, _countof(local), L"%ls\\bin.dat", dir);
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri_big) == FS_FILE_USERABORT &&
                verify_pattern_file(local, 70000) && count_partial_files(dir) == 0,
                "ESC during an overwrite keeps the previous file intact");
    g_e2e_abort_after_start = FALSE;
    DeleteFileW(local);

    /* Password flows on a repository whose password is not cached yet */
    swprintf_s(remote, _countof(remote), L"\\ExtractRepo%ls\\readme.txt", data_path + wcslen(L"\\MainRepo"));
    swprintf_s(local, _countof(local), L"%ls\\slot.txt", dir);
    g_e2e_pwd_mode = 4; /* user cancels the password dialog */
    g_e2e_pwd_prompts = 0;
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_USERABORT && g_e2e_pwd_prompts == 1 &&
                g_e2e_msg_dialogs == saved_dialogs, "Cancelled password dialog -> FS_FILE_USERABORT, no popup");
    g_e2e_pwd_mode = 2; /* wrong password three times */
    g_e2e_pwd_prompts = 0;
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_READERROR && g_e2e_pwd_prompts == 3 &&
                g_e2e_msg_dialogs == saved_dialogs + 1 && GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES,
                "Three wrong passwords -> FS_FILE_READERROR with one popup and no local file");
    g_e2e_pwd_mode = 0; /* wrong first, correct second */
    g_e2e_pwd_prompts = 0;
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_OK && g_e2e_pwd_prompts == 2 &&
                file_has_text(local, "xxxxx"), "Retry after a wrong password extracts the file");
    g_e2e_pwd_prompts = 0;
    EXPECT_TRUE(p->get_file_w(remote, local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK && g_e2e_pwd_prompts == 0,
                "The validated password is cached for the next file of the same copy operation");
    DeleteFileW(local);

    EXPECT_TRUE(count_partial_files(dir) == 0, "No temporary files remain in the destination directory");
    g_e2e_pwd_prompts = saved_prompts;
    g_e2e_msg_dialogs = saved_dialogs;
    g_e2e_error_logs  = saved_logs;
    g_e2e_pwd_mode    = saved_mode;
}

/*
 * Lists `path` and joins the entry names with '|' (sorted as the plugin returns them); FALSE on
 * error and when they do not fit into `cap` characters. Checked here, not left to wcscat_s: its
 * overflow is an invalid parameter, which the Debug CRT reports in a modal dialog that stops an
 * unattended run, and which the Release CRT lets pass with an emptied buffer.
 */
static BOOL list_names(const Plugin* p, const WCHAR* path, WCHAR* out, size_t cap) {
    WIN32_FIND_DATAW fd;
    out[0] = L'\0';
    HANDLE h = p->find_first_w((WCHAR*)path, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL fits   = TRUE;
    size_t used = 0;
    do {
        size_t len = wcslen(fd.cFileName);
        if (used + (used > 0 ? 1 : 0) + len >= cap) {
            fits = FALSE;
            continue;
        }
        if (used > 0) out[used++] = L'|';
        wmemcpy(out + used, fd.cFileName, len);
        used += len;
        out[used] = L'\0';
    } while (p->find_next_w(h, &fd));
    p->find_close(h);
    return fits;
}

/* Recursively descends the first directory at each depth until the fixture "data" directory is found */
static BOOL descend_to_fixture_data(
    const Plugin* p,
    const WCHAR* current_path,
    int depth,
    WCHAR* out_data_path,
    size_t out_cap
) {
    if (depth > 16) return FALSE;

    WIN32_FIND_DATAW fd;
    HANDLE h = p->find_first_w((WCHAR*)current_path, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    WCHAR first_dir[MAX_PATH] = {0};
    BOOL found_data = FALSE;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (wcscmp(fd.cFileName, L"data") == 0) {
                found_data = TRUE;
                break;
            }
            if (first_dir[0] == L'\0') {
                wcsncpy_s(first_dir, MAX_PATH, fd.cFileName, _TRUNCATE);
            }
        }
    } while (p->find_next_w(h, &fd));
    p->find_close(h);

    if (found_data) {
        swprintf_s(out_data_path, out_cap, L"%ls\\data", current_path);
        return TRUE;
    }
    if (first_dir[0] == L'\0') return FALSE;

    WCHAR next_path[MAX_PATH * 2];
    swprintf_s(next_path, _countof(next_path), L"%ls\\%ls", current_path, first_dir);
    return descend_to_fixture_data(p, next_path, depth + 1, out_data_path, out_cap);
}

/* Number of times `what` occurs in the plugin's diagnostic log */
static int count_in_log(const WCHAR* log_file, const char* what) {
    char* text = read_text_file(log_file);
    int n = 0;
    for (const char* p = text; p && (p = strstr(p, what)) != NULL; p++) n++;
    free(text);
    return n;
}

/* Number of restic processes the plugin started so far, as its diagnostic log records them */
static int count_restic_runs(const WCHAR* log_file) {
    return count_in_log(log_file, "restic run: ");
}

/*
 * Entering a snapshot starts reading all of it in the background; once that is done, its folders
 * are listed without running restic, and forgetting the password drops what was read
 */
static void test_e2e_snapshot_cache(const Plugin* p, const WCHAR* dir, const WCHAR* data_path) {
    /* data\many holds 300 names of 60 characters: room for all of them */
    static WCHAR log_file[MAX_PATH], sub[MAX_PATH * 2], names[4096], names_again[32768];
    swprintf_s(log_file, _countof(log_file), L"%ls\\log dir\\e2e.log", dir);
    BOOL read = FALSE;
    for (int i = 0; i < 600 && !read; ++i) { /* Up to two minutes */
        char* text = read_text_file(log_file);
        read = text && strstr(text, "Listing of snapshot ") && strstr(text, " read: ");
        free(text);
        if (!read) Sleep(200);
    }
    EXPECT_TRUE(read, "The whole snapshot was read in the background after entering it");
    /* (The test has entered two snapshots by now: one through ids\, one through snapshots\) */
    EXPECT_TRUE(conn_log_count(L"3:[MainRepo] Reading every file and folder of snapshot ") >= 1 &&
                conn_log_count(L"3:[MainRepo] Snapshot ") >= 1 &&
                conn_log_count(L"files and folders, ") >= 1 && conn_log_count(L"its folders now open without restic") >= 1,
                "The connection log tells when the background read starts and when it is done (MSGTYPE_DETAILS)");

    int runs = count_restic_runs(log_file);
    g_e2e_pwd_prompts = 0;
    WIN32_FIND_DATAW fd;
    EXPECT_TRUE(list_names(p, data_path, names, _countof(names)) && wcsstr(names, L"readme.txt") != NULL,
                "Fixture folder listed from memory");
    swprintf_s(sub, _countof(sub), L"%ls\\many", data_path);
    EXPECT_TRUE(list_names(p, sub, names_again, _countof(names_again)) && wcsstr(names_again, L"|") != NULL,
                "Its subfolder too");
    swprintf_s(sub, _countof(sub), L"%ls\\empty_dir", data_path);
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(sub, &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES,
                "An empty folder is still empty");
    swprintf_s(sub, _countof(sub), L"%ls\\no_such_folder", data_path);
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(sub, &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
                "A missing folder is missing (and no file there to search the versions of)");
    EXPECT_TRUE(count_restic_runs(log_file) == runs && g_e2e_pwd_prompts == 0, "None of it ran restic");

    /* Forgetting the passwords drops the listing with them: the next visit asks and runs restic */
    WCHAR forget[] = L"\\\U0001F5D1 Forget 1 Password", open_verb[] = L"open";
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(p->execute_w(NULL, forget, open_verb) == FS_EXEC_SYMLINK, "Forget the passwords");
    EXPECT_TRUE(conn_log_count(L"3:All remembered passwords and cached listings forgotten; Disconnect closes this toolbar") == 1 &&
                conn_log_count(L"2:DISCONNECT") == 0,
                "...which the connection log says, pointing to Disconnect: the plugin cannot close the connection itself");
    g_e2e_pwd_mode = 1;
    EXPECT_TRUE(list_names(p, data_path, names_again, _countof(names_again)) && wcscmp(names, names_again) == 0,
                "Listed again after a password prompt, with the same entries");
    EXPECT_TRUE(g_e2e_pwd_prompts == 1 && count_restic_runs(log_file) > runs, "Asked for the password and ran restic");
    EXPECT_TRUE(conn_log_count(L"1:CONNECT \\\n") == 1, "...on the connection still listed, not a new one");
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
}

/* The size of a local file, or -1 */
static long long local_size(const WCHAR* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

/*
 * The plugin's own dialog naming the folder of a file's versions (IDD_VERSIONS_FOLDER in
 * resource.h): found by its caption, its path, default button and focus read, then a button pressed
 */
typedef struct {
    const WCHAR* caption;
    WORD button; /* IDCANCEL ("Close"), 1002 ("Copy to clipboard") or 1003 ("Go to folder") */
    BOOL found;
    WCHAR path[MAX_PATH * 2];
    int default_id, focus_id; /* Control IDs, 0 when unknown */
} DialogAnswer;

static DWORD WINAPI answer_dialog(LPVOID param) {
    DialogAnswer* a = (DialogAnswer*)param;
    for (int i = 0; i < 1000; ++i) { /* Up to 10 s */
        HWND dlg = FindWindowW(L"#32770", a->caption);
        if (dlg) {
            a->found = GetDlgItemTextW(dlg, 1001, a->path, _countof(a->path)) > 0;
            LRESULT def = SendMessageW(dlg, DM_GETDEFID, 0, 0);
            a->default_id = HIWORD(def) == DC_HASDEFID ? LOWORD(def) : 0;
            GUITHREADINFO gui = {sizeof(gui)};
            a->focus_id = GetGUIThreadInfo(GetWindowThreadProcessId(dlg, NULL), &gui) && gui.hwndFocus
                              ? GetDlgCtrlID(gui.hwndFocus) : 0;
            PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(a->button, BN_CLICKED), 0);
            return 0;
        }
        Sleep(10);
    }
    return 0;
}

/*
 * A stand-in for Total Commander's main window, on the thread calling the plugin: keeps what a
 * WM_COPYDATA brought (Total Commander's "CD" request)
 */
static struct {
    ULONG_PTR kind;
    DWORD len;
    char data[4096];
    int count;
} g_copydata;

static LRESULT CALLBACK fake_total_cmd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lparam;
        g_copydata.kind = cds->dwData;
        g_copydata.len  = cds->cbData < sizeof(g_copydata.data) ? cds->cbData : (DWORD)sizeof(g_copydata.data);
        memcpy(g_copydata.data, cds->lpData, g_copydata.len);
        g_copydata.count++;
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static HWND fake_total_cmd(void) {
    static HWND window;
    if (!window) {
        WNDCLASSW wc     = {0};
        wc.lpfnWndProc   = fake_total_cmd_proc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.lpszClassName = L"TCResticBrowserTestTotalCmd";
        RegisterClassW(&wc);
        window = CreateWindowW(wc.lpszClassName, L"Total Commander stand-in", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                               NULL, NULL, wc.hInstance, NULL);
    }
    return window;
}

/* Lets the calling thread's message loop run for `ms` milliseconds, as Total Commander's would */
static void pump_messages(DWORD ms) {
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < ms) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

/* Alt+Enter on `remote` from `main_win` with the dialog answered by `button`: FS_EXEC_* */
static int properties_with_dialog(const Plugin* p, HWND main_win, WCHAR* remote, DialogAnswer* a, WORD button) {
    WCHAR properties[] = L"properties";
    a->button = button;
    a->found  = FALSE;
    a->path[0] = L'\0';
    a->default_id = a->focus_id = 0;
    HANDLE t = CreateThread(NULL, 0, answer_dialog, a, 0, NULL);
    int rc   = p->execute_w(main_win, remote, properties);
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    return rc;
}

/* The clipboard's text into `out` ("" when it holds none); `set` non-NULL: puts that there first */
static void clipboard_text(const WCHAR* set, WCHAR* out, size_t cap) {
    out[0] = L'\0';
    if (!OpenClipboard(NULL)) return;
    if (set) {
        size_t bytes = (wcslen(set) + 1) * sizeof(WCHAR);
        HGLOBAL mem  = GlobalAlloc(GMEM_MOVEABLE, bytes);
        void* data   = mem ? GlobalLock(mem) : NULL;
        if (data) {
            memcpy(data, set, bytes);
            GlobalUnlock(mem);
            if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else if (mem) {
            GlobalFree(mem);
        }
    }
    HANDLE clip       = GetClipboardData(CF_UNICODETEXT);
    const WCHAR* text = clip ? (const WCHAR*)GlobalLock(clip) : NULL;
    if (text) {
        wcsncpy_s(out, cap, text, _TRUNCATE);
        GlobalUnlock(clip);
    }
    CloseClipboard();
}

/*
 * Alt+Enter (FsExecuteFileW "properties"): a file names, in a dialog, the folder of its versions
 * across the snapshots, which copy like files; a version, a snapshot and a folder in it show their properties
 */
static void test_e2e_file_history(const Plugin* p, const WCHAR* dir, const WCHAR* snap_root, const WCHAR* data_path) {
    static WCHAR remote[MAX_PATH * 2], names[2048], version_path[MAX_PATH * 2], local[MAX_PATH], log_file[MAX_PATH];
    WCHAR properties[] = L"properties";
    swprintf_s(log_file, _countof(log_file), L"%ls\\log dir\\e2e.log", dir);
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;

    /* with space.txt was changed between the two fixture snapshots: two versions */
    swprintf_s(remote, _countof(remote), L"%ls\\with space.txt", data_path);
    static DialogAnswer answer = {L"Versions of with space.txt", IDCANCEL, FALSE, {0}, 0, 0};
    static WCHAR clip[MAX_PATH * 2];
    HWND total_cmd = fake_total_cmd();
    EXPECT_TRUE(total_cmd != NULL, "A stand-in for Total Commander's window");
    clipboard_text(L"before", clip, _countof(clip));
    g_copydata.count = 0;
    EXPECT_TRUE(properties_with_dialog(p, total_cmd, remote, &answer, IDCANCEL) == FS_EXEC_OK &&
                    g_e2e_msg_dialogs == 0 && answer.found,
                "Alt+Enter on a file names the folder of its versions in a dialog");
    EXPECT_TRUE(answer.default_id == 1003 && answer.focus_id == 1003,
                "...whose default button, focused, is Go to folder");
    size_t value_len = wcslen(answer.path), remote_len = wcslen(remote);
    EXPECT_TRUE(wcsncmp(answer.path, L"\\\\\\", 3) == 0 && value_len > remote_len + 1 &&
                    wcsncmp(answer.path + value_len - remote_len - 1, remote, remote_len) == 0 &&
                    answer.path[value_len - 1] == L'\\' &&
                    wcschr(answer.path + 3, L'\\') == answer.path + value_len - remote_len - 1,
                "...the file's own full path, with a trailing backslash");
    clipboard_text(NULL, clip, _countof(clip));
    pump_messages(200);
    EXPECT_TRUE(wcscmp(clip, L"before") == 0 && g_copydata.count == 0,
                "...which Close leaves off the clipboard, without going there");
    EXPECT_TRUE(properties_with_dialog(p, NULL, remote, &answer, 1002) == FS_EXEC_OK && answer.found,
                "...and Copy to clipboard");
    EXPECT_TRUE(answer.default_id == 1002 && answer.focus_id == 1002,
                "...the default without Total Commander's window, Go to folder disabled");
    clipboard_text(NULL, clip, _countof(clip));
    EXPECT_TRUE(wcscmp(clip, answer.path) == 0, "...puts there");
    EXPECT_TRUE(properties_with_dialog(p, total_cmd, remote, &answer, 1003) == FS_EXEC_OK && answer.found,
                "...and Go to folder");
    EXPECT_TRUE(g_copydata.count == 0, "...asks Total Commander only once Alt+Enter has returned");
    pump_messages(500);
    static char expected[4096];
    int path_bytes = WideCharToMultiByte(CP_UTF8, 0, answer.path, -1, expected + 3, (int)sizeof(expected) - 8, NULL,
                                         NULL);
    memcpy(expected, "\xEF\xBB\xBF", 3);
    memcpy(expected + 3 + path_bytes - 1, "\r\0S", 4); /* The literal's NUL ends the flags */
    DWORD expected_len = (DWORD)(3 + path_bytes - 1 + 4);
    EXPECT_TRUE(g_copydata.count == 1 && g_copydata.kind == ('C' + 256 * 'D') && g_copydata.len == expected_len &&
                    memcmp(g_copydata.data, expected, expected_len) == 0,
                "...then asks it, once, to show the folder in the active panel (WM_COPYDATA \"CD\", UTF-8, flag S)");
    swprintf_s(remote, _countof(remote), L"%ls\\with space.txt\\", data_path);
    WIN32_FIND_DATAW fd;
    WCHAR older[MAX_PATH] = {0}, newer[MAX_PATH] = {0};
    long long older_size = -1, newer_size = -1;
    size_t versions = 0;
    HANDLE h = p->find_first_w(remote, &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "...which lists the file's versions");
    if (h != INVALID_HANDLE_VALUE) {
        do {
            ++versions;
            BOOL plain = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && fd.ftLastWriteTime.dwHighDateTime != 0xFFFFFFFF;
            EXPECT_TRUE(plain && wcsstr(fd.cFileName, L" with space.txt") != NULL,
                        "A version is a file named by its modification time and the file's name");
            WCHAR* slot = older[0] ? newer : older;
            wcsncpy_s(slot, MAX_PATH, fd.cFileName, _TRUNCATE);
            *(slot == older ? &older_size : &newer_size) = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } while (p->find_next_w(h, &fd));
        p->find_close(h);
    }
    EXPECT_TRUE(versions == 2 && older_size == 5 && newer_size == 12 && wcscmp(older, newer) < 0,
                "Two versions, oldest first: 5 bytes, then 12");
    EXPECT_TRUE(older[4] == L'-' && older[10] == L'T' && older[13] == L'.', "Named by SnapshotTimeFormat");
    int finds = count_in_log(log_file, " find --json ");
    EXPECT_TRUE(finds == 1, "...found by one restic find");
    EXPECT_TRUE(wcsncmp(older + 19, L" (", 2) == 0 && wcsspn(older + 21, L"0123456789abcdef") == 8 &&
                    wcsncmp(older + 29, L") with space.txt", 16) == 0 && wcsncmp(older + 19, newer + 19, 10) != 0,
                "...then the short ID of the snapshot holding the version, a different one for each");

    remote[wcslen(remote) - 1] = L'\0'; /* Total Commander names what is inside without the backslash */

    /* F5 on a version copies the file from a snapshot that holds that version */
    swprintf_s(version_path, _countof(version_path), L"%ls\\%ls", remote, older);
    swprintf_s(local, _countof(local), L"%ls\\older version.txt", dir);
    DeleteFileW(local);
    RemoteInfo ri = {5, 0, {0x802D2000, 0x01D5EF01}, FILE_ATTRIBUTE_READONLY};
    EXPECT_TRUE(p->get_file_w(version_path, local, 0, &ri) == FS_FILE_OK && local_size(local) == 5,
                "The older version copied: the content of the older snapshot");
    DeleteFileW(local);
    swprintf_s(version_path, _countof(version_path), L"%ls\\%ls", remote, newer);
    ri.SizeLow = 12;
    EXPECT_TRUE(p->get_file_w(version_path, local, 0, &ri) == FS_FILE_OK && local_size(local) == 12,
                "The newer version copied");
    DeleteFileW(local);
    swprintf_s(version_path, _countof(version_path), L"%ls\\1999-01-01T00.00.00 with space.txt", remote);
    EXPECT_TRUE(p->get_file_w(version_path, local, 0, &ri) == FS_FILE_NOTFOUND &&
                    GetFileAttributesW(local) == INVALID_FILE_ATTRIBUTES,
                "No such version: not found, nothing written");

    /* Alt+Enter on a version: the snapshots holding it */
    swprintf_s(version_path, _countof(version_path), L"%ls\\%ls", remote, newer);
    EXPECT_TRUE(p->execute_w(NULL, version_path, properties) == FS_EXEC_OK && g_e2e_msg_dialogs == 1 &&
                    wcsstr(g_e2e_last_msg, L"Version of with space.txt\n") == g_e2e_last_msg &&
                    wcsstr(g_e2e_last_msg, L"Size: 12 B\n") != NULL && wcsstr(g_e2e_last_msg, L"In 1 snapshot:\n") != NULL,
                "Alt+Enter on a version names it, its size and the snapshot holding it");

    /* An unchanged file: one version in both snapshots */
    swprintf_s(remote, _countof(remote), L"%ls\\readme.txt", data_path);
    EXPECT_TRUE(list_names(p, remote, names, _countof(names)) && wcschr(names, L'|') == NULL &&
                    wcsstr(names, L" readme.txt") != NULL,
                "An unchanged file has one version");
    swprintf_s(version_path, _countof(version_path), L"%ls\\%ls", remote, names);
    EXPECT_TRUE(p->execute_w(NULL, version_path, properties) == FS_EXEC_OK && g_e2e_msg_dialogs == 2 &&
                    wcsstr(g_e2e_last_msg, L"In 2 snapshots:\n") != NULL,
                "...held by both snapshots");

    EXPECT_TRUE(count_in_log(log_file, " find --json ") == finds + 1,
                "Copying the versions and Alt+Enter on them ran no restic find again (one for readme.txt)");

    /* A snapshot, or a folder in it: the snapshot's properties */
    EXPECT_TRUE(p->execute_w(NULL, (WCHAR*)snap_root, properties) == FS_EXEC_OK && g_e2e_msg_dialogs == 3 &&
                    wcsncmp(g_e2e_last_msg, L"Snapshot ", 9) == 0 && wcsstr(g_e2e_last_msg, L"\nPaths: ") != NULL &&
                    wcsstr(g_e2e_last_msg, L"\nFiles: ") != NULL && wcsstr(g_e2e_last_msg, L"\nProgram: restic ") != NULL,
                "Alt+Enter on a snapshot shows its properties");
    static WCHAR snapshot_text[1024];
    wcscpy_s(snapshot_text, _countof(snapshot_text), g_e2e_last_msg);
    wcscpy_s(remote, _countof(remote), data_path);
    EXPECT_TRUE(p->execute_w(NULL, remote, properties) == FS_EXEC_OK && g_e2e_msg_dialogs == 4 &&
                    wcscmp(g_e2e_last_msg, snapshot_text) == 0 && wcscmp(remote, data_path) == 0,
                "...and on a folder inside it the same");
    wcscpy_s(remote, _countof(remote), L"\\MainRepo\\hosts");
    EXPECT_TRUE(p->execute_w(NULL, remote, properties) == FS_EXEC_OK && g_e2e_msg_dialogs == 4,
                "Above the snapshots: nothing to show");

    /* A missing path is neither a folder nor a file */
    swprintf_s(remote, _countof(remote), L"%ls\\no_such_file.txt", data_path);
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(remote, &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
                "A missing file has no versions");
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 4, "No password asked, no error shown");
    swprintf_s(local, _countof(local), L"%ls\\nothing.txt", dir);
    swprintf_s(remote, _countof(remote), L"%ls\\no_such_file.txt\\2026-01-01T00.00.00 (00000000) no_such_file.txt",
               data_path);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_NOTFOUND, "A version of a missing file: not found");
    swprintf_s(remote, _countof(remote), L"%ls\\many\\no_such_file.txt", data_path);
    EXPECT_TRUE(p->get_file_w(remote, local, 0, &ri) == FS_FILE_NOTFOUND, "A missing file in a folder: not found");
    EXPECT_TRUE(count_in_log(log_file, " find --json ") == finds + 1 && g_e2e_msg_dialogs == 4,
                "...and its folder or a missing path searched for by no restic find");

    /* Without a password, Alt+Enter above the snapshots asks for none: there is nothing to show */
    WCHAR forget[] = L"\\\U0001F5D1 Forget 1 Password", open_verb[] = L"open";
    EXPECT_TRUE(p->execute_w(NULL, forget, open_verb) == FS_EXEC_SYMLINK, "Forget the password");
    int runs = count_restic_runs(log_file);
    static const WCHAR* const above[] = {L"\\MainRepo\\hosts", L"\\MainRepo\\snapshots", L"\\MainRepo\\snapshots.csv",
                                         L"\\MainRepo\\snapshots.json", L"\\MainRepo\\ids\\", L"\\MainRepo\\tags\\daily"};
    for (size_t i = 0; i < _countof(above); ++i) {
        wcscpy_s(remote, _countof(remote), above[i]);
        EXPECT_TRUE(p->execute_w(NULL, remote, properties) == FS_EXEC_OK, "Alt+Enter above the snapshots");
    }
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && count_restic_runs(log_file) == runs && g_e2e_msg_dialogs == 5,
                "...asked for no password and ran no restic (only the forget entry's message)");
    g_e2e_pwd_mode = 1;
    EXPECT_TRUE(list_names(p, data_path, names, _countof(names)), "The password given again");
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
}

/* Lists `path` and stores the number of entries (0 on error) */
static void count_entries(const Plugin* p, const WCHAR* path, size_t* out) {
    WIN32_FIND_DATAW fd;
    *out = 0;
    HANDLE h = p->find_first_w((WCHAR*)path, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do (*out)++; while (p->find_next_w(h, &fd));
    p->find_close(h);
}

/* Takes one more snapshot of the fixture data into `repo` with restic itself */
static BOOL run_restic_backup(const WCHAR* restic_exe, const WCHAR* repo) {
    static WCHAR cmd[MAX_PATH * 4], data[MAX_PATH];
    GetFullPathNameW(L"tmp\\test_fixture\\data", MAX_PATH, data, NULL);
    swprintf_s(cmd, _countof(cmd), L"\"%ls\" -r \"%ls\" backup \"%ls\" --quiet --host fresh-test", restic_exe, repo, data);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD", FIXTURE_PASSWORD);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD", NULL);
    if (!ok) return FALSE;
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, 120000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

/* TRUE for a time within the last hour (and not ahead of the clock) */
static BOOL is_recent(const FILETIME* t) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    ULONGLONG v = ((ULONGLONG)t->dwHighDateTime << 32) | t->dwLowDateTime;
    return v <= n + 600000000ull && n - v < 36000000000ull; /* 1 min ahead, 1 hour back */
}

static BOOL file_mtime(const WCHAR* path, FILETIME* out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = GetFileTime(f, NULL, NULL, out);
    CloseHandle(f);
    return ok;
}

/*
 * snapshots.csv in the repository's folder (the E2E INI sets CsvSeparator=;): listed as a read-only
 * file of its exact size, copied through FsGetFileW with the overwrite protocol, never a folder.
 * `latest_id` is the short ID of the newer of the fixture's two snapshots (same host and paths).
 */
static void test_snapshots_csv(const Plugin* p, const WCHAR* dir, const WCHAR* some_id, const WCHAR* latest_id) {
    WIN32_FIND_DATAW fd;
    RemoteInfo ri;
    memset(&ri, 0, sizeof(ri));
    BOOL listed = FALSE;
    FILETIME newest_snapshot = {0, 0};
    HANDLE h = p->find_first_w(L"\\MainRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L"snapshots") == 0) newest_snapshot = fd.ftLastWriteTime;
            if (wcscmp(fd.cFileName, L"snapshots.csv") != 0) continue;
            listed = (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) ==
                         FILE_ATTRIBUTE_READONLY &&
                     fd.nFileSizeHigh == 0 && fd.nFileSizeLow > 0 && fd.ftLastWriteTime.dwHighDateTime != 0xFFFFFFFF;
            ri.SizeLow       = fd.nFileSizeLow;
            ri.SizeHigh      = fd.nFileSizeHigh;
            ri.LastWriteTime = fd.ftLastWriteTime;
            ri.Attr          = (int)fd.dwFileAttributes;
        } while (p->find_next_w(h, &fd));
        p->find_close(h);
    }
    EXPECT_TRUE(listed, "snapshots.csv is listed as a read-only file with a size and a time");
    EXPECT_TRUE(is_recent(&ri.LastWriteTime) && CompareFileTime(&ri.LastWriteTime, &newest_snapshot) > 0,
                "Its time is when restic listed the snapshots, after the newest one was taken");

    WCHAR local[MAX_PATH];
    swprintf_s(local, MAX_PATH, L"%ls\\snapshots.csv", dir);
    DeleteFileW(local);
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\snapshots.csv", local, 0, &ri) == FS_FILE_OK, "snapshots.csv copied");
    static char text[4096];
    DWORD got = 0;
    HANDLE f  = CreateFileW(local, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    FILETIME mtime = {0};
    if (f != INVALID_HANDLE_VALUE) {
        if (!ReadFile(f, text, sizeof(text) - 1, &got, NULL)) got = 0;
        GetFileTime(f, NULL, NULL, &mtime);
        CloseHandle(f);
    }
    text[got] = '\0';
    EXPECT_TRUE(got == ri.SizeLow, "The copy has exactly the listed size");
    EXPECT_TRUE(CompareFileTime(&mtime, &ri.LastWriteTime) == 0, "The copy carries the listed time");
    const char* header = "\xEF\xBB\xBFshort ID;host;paths;size;timestamp;tags;full ID\r\n";
    EXPECT_TRUE(strncmp(text, header, strlen(header)) == 0, "UTF-8 BOM, then the header with CsvSeparator=;");
    char some_a[16] = {0}, latest_a[16] = {0};
    WideCharToMultiByte(CP_UTF8, 0, some_id, -1, some_a, sizeof(some_a), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, latest_id, -1, latest_a, sizeof(latest_a), NULL, NULL);
    const char* first  = strstr(text, "\r\n");
    const char* second = first ? strstr(first + 2, "\r\n") : NULL;
    EXPECT_TRUE(first && second && strncmp(second + 2, latest_a, 8) == 0 &&
                    strstr(second + 2, "\r\n") == text + got - 2,
                "Two records sorted by time: the newer snapshot is the last line");
    enum { FULL_ID_CHARS = 64 };
    const char* last_id = text + got - 2 - FULL_ID_CHARS;
    EXPECT_TRUE(got > FULL_ID_CHARS + 3 && last_id[-1] == ';' && strncmp(last_id, latest_a, 8) == 0 &&
                    strspn(last_id, "0123456789abcdef") == FULL_ID_CHARS,
                "Each record ends with the full ID, the last one the newer snapshot's");
    EXPECT_TRUE(strstr(text, some_a) != NULL && strstr(text, "\\test_fixture\\data") != NULL,
                "The records hold the short IDs and the backed-up path");
    /* restic 0.17 and later record a summary, so the size follows the path as restic snapshots prints it */
    const char* size  = second ? strstr(second + 2, "\\test_fixture\\data;") : NULL;
    size              = size ? size + strlen("\\test_fixture\\data;") : NULL;
    const char* end   = size ? strchr(size, ';') : NULL;
    size_t size_len   = end ? (size_t)(end - size) : 0;
    int size_has_unit = size_len >= 9 && (strncmp(end - 4, " KiB", 4) == 0 || strncmp(end - 4, " MiB", 4) == 0);
    EXPECT_TRUE(size_has_unit && size[0] >= '1' && size[0] <= '9' && end[-8] == '.' &&
                    strspn(end - 7, "0123456789") == 3,
                "The newer snapshot's size, as \"<n>.<3 decimals> KiB\" or MiB, after its path");

    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\snapshots.csv", local, 0, &ri) == FS_FILE_EXISTS,
                "An existing copy is reported first (overwrite protocol)");
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\SNAPSHOTS.CSV", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK,
                "... and replaced with FS_COPYFLAGS_OVERWRITE (the name compares ignoring case)");
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(L"\\MainRepo\\snapshots.csv", &fd) == INVALID_HANDLE_VALUE &&
                    GetLastError() == ERROR_PATH_NOT_FOUND,
                "snapshots.csv is not a folder");
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(L"\\MainRepo\\snapshots.csv\\x", &fd) == INVALID_HANDLE_VALUE &&
                    GetLastError() == ERROR_PATH_NOT_FOUND &&
                    p->get_file_w(L"\\MainRepo\\snapshots.csv\\x", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTFOUND,
                "Nothing is below snapshots.csv");
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\hosts\\snapshots.csv", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTFOUND,
                "snapshots.csv is only in the repository's folder");
    DeleteFileW(local);
}

/* Reads a small local file into `text` (NUL-terminated); its length, or -1 */
static int read_small_file(const WCHAR* path, char* text, DWORD cap) {
    DWORD got = 0;
    HANDLE f  = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    BOOL ok = ReadFile(f, text, cap - 1, &got, NULL);
    CloseHandle(f);
    text[ok ? got : 0] = '\0';
    return ok ? (int)got : -1;
}

/*
 * snapshots.json beside snapshots.csv: restic's own `snapshots --json` output, listed as a read-only
 * file of its exact size with the time of snapshots.csv, copied byte for byte, never a folder
 */
static void test_snapshots_json(const Plugin* p, const WCHAR* dir, const WCHAR* some_id, const WCHAR* latest_id) {
    WIN32_FIND_DATAW fd;
    RemoteInfo ri;
    memset(&ri, 0, sizeof(ri));
    BOOL listed  = FALSE;
    FILETIME csv = {0, 0};
    HANDLE h     = p->find_first_w(L"\\MainRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L"snapshots.csv") == 0) csv = fd.ftLastWriteTime;
            if (wcscmp(fd.cFileName, L"snapshots.json") != 0) continue;
            listed = (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) ==
                         FILE_ATTRIBUTE_READONLY && fd.nFileSizeHigh == 0 && fd.nFileSizeLow > 0;
            ri.SizeLow       = fd.nFileSizeLow;
            ri.LastWriteTime = fd.ftLastWriteTime;
            ri.Attr          = (int)fd.dwFileAttributes;
        } while (p->find_next_w(h, &fd));
        p->find_close(h);
    }
    EXPECT_TRUE(listed, "snapshots.json is listed as a read-only file with a size");
    EXPECT_TRUE(csv.dwHighDateTime != 0 && CompareFileTime(&ri.LastWriteTime, &csv) == 0,
                "...dated like snapshots.csv: when restic listed the snapshots");

    WCHAR local[MAX_PATH];
    swprintf_s(local, MAX_PATH, L"%ls\\snapshots.json", dir);
    DeleteFileW(local);
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\snapshots.json", local, 0, &ri) == FS_FILE_OK, "snapshots.json copied");
    static char text[65536];
    int got = read_small_file(local, text, sizeof(text));
    EXPECT_TRUE(got > 0 && (DWORD)got == ri.SizeLow, "The copy has exactly the listed size");
    char some_a[16] = {0}, latest_a[16] = {0}, key[64];
    WideCharToMultiByte(CP_UTF8, 0, some_id, -1, some_a, sizeof(some_a), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, latest_id, -1, latest_a, sizeof(latest_a), NULL, NULL);
    snprintf(key, sizeof(key), "\"short_id\":\"%s\"", latest_a);
    BOOL has_latest = strstr(text, key) != NULL;
    snprintf(key, sizeof(key), "\"short_id\":\"%s\"", some_a);
    EXPECT_TRUE(got > 0 && text[0] == '[' && has_latest && strstr(text, key) != NULL &&
                    strstr(text, "\"summary\":") != NULL,
                "...restic's JSON array of the snapshots, summaries included");

    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\snapshots.json", local, 0, &ri) == FS_FILE_EXISTS &&
                    p->get_file_w(L"\\MainRepo\\Snapshots.JSON", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK,
                "The overwrite protocol, the name ignoring case");
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(L"\\MainRepo\\snapshots.json", &fd) == INVALID_HANDLE_VALUE &&
                    GetLastError() == ERROR_PATH_NOT_FOUND &&
                    p->get_file_w(L"\\MainRepo\\snapshots.json\\x", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTFOUND &&
                    p->get_file_w(L"\\MainRepo\\tags\\snapshots.json", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_NOTFOUND,
                "snapshots.json is a file, only in the repository's folder");
    DeleteFileW(local);
}

/*
 * repository_id.txt beside snapshots.csv: listed with its fixed size (64 hexadecimal characters and
 * CRLF), read by restic once and then taken from the cache, never a folder
 */
static void test_repository_id(const Plugin* p, const WCHAR* dir) {
    WIN32_FIND_DATAW fd;
    RemoteInfo ri;
    memset(&ri, 0, sizeof(ri));
    BOOL listed  = FALSE;
    FILETIME csv = {0, 0};
    HANDLE h     = p->find_first_w(L"\\MainRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L"snapshots.csv") == 0) csv = fd.ftLastWriteTime;
            if (wcscmp(fd.cFileName, L"repository_id.txt") != 0) continue;
            listed = (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) ==
                         FILE_ATTRIBUTE_READONLY && fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 66;
            ri.SizeLow       = fd.nFileSizeLow;
            ri.LastWriteTime = fd.ftLastWriteTime;
            ri.Attr          = (int)fd.dwFileAttributes;
        } while (p->find_next_w(h, &fd));
        p->find_close(h);
    }
    EXPECT_TRUE(listed, "repository_id.txt is listed as a read-only file of 66 bytes");
    EXPECT_TRUE(ri.LastWriteTime.dwHighDateTime != 0xFFFFFFFF && CompareFileTime(&ri.LastWriteTime, &csv) == 0,
                "Before restic was asked for the ID, the time of snapshots.csv");
    FILETIME asked;
    GetSystemTimeAsFileTime(&asked);

    WCHAR local[MAX_PATH];
    swprintf_s(local, MAX_PATH, L"%ls\\repository_id.txt", dir);
    DeleteFileW(local);
    int reads_before = conn_log_count(L"Repository ID read by restic");
    int prompts      = g_e2e_pwd_prompts;
    static char first[128], second[128];
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\repository_id.txt", local, 0, &ri) == FS_FILE_OK &&
                    read_small_file(local, first, sizeof(first)) == 66,
                "repository_id.txt copied: 66 bytes");
    EXPECT_TRUE(strspn(first, "0123456789abcdef") == 64 && strcmp(first + 64, "\r\n") == 0,
                "One line: the 64-character ID and a Windows line end");
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\repository_id.txt", local, 0, &ri) == FS_FILE_EXISTS,
                "An existing copy is reported first (overwrite protocol)");
    EXPECT_TRUE(p->get_file_w(L"\\MainRepo\\Repository_ID.TXT", local, FS_COPYFLAGS_OVERWRITE, &ri) == FS_FILE_OK &&
                    read_small_file(local, second, sizeof(second)) == 66 && strcmp(first, second) == 0,
                "... and replaced by the same ID (the name compares ignoring case)");
    EXPECT_TRUE(conn_log_count(L"Repository ID read by restic") == reads_before + 1 && g_e2e_pwd_prompts == prompts,
                "restic read the ID once, without a prompt; the second copy came from the cache");
    FILETIME copied = {0, 0}, relisted = {0, 0};
    h = p->find_first_w(L"\\MainRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L"repository_id.txt") == 0) relisted = fd.ftLastWriteTime;
        } while (p->find_next_w(h, &fd));
        p->find_close(h);
    }
    EXPECT_TRUE(file_mtime(local, &copied) && CompareFileTime(&copied, &asked) >= 0 && is_recent(&copied) &&
                    CompareFileTime(&copied, &relisted) == 0,
                "The copy has the time restic was asked for the ID, which the listing now shows too");
    SetLastError(0);
    EXPECT_TRUE(p->find_first_w(L"\\MainRepo\\repository_id.txt", &fd) == INVALID_HANDLE_VALUE &&
                    GetLastError() == ERROR_PATH_NOT_FOUND &&
                    p->get_file_w(L"\\MainRepo\\repository_id.txt\\x", local, FS_COPYFLAGS_OVERWRITE, &ri) ==
                        FS_FILE_NOTFOUND &&
                    p->get_file_w(L"\\MainRepo\\ids\\repository_id.txt", local, FS_COPYFLAGS_OVERWRITE, &ri) ==
                        FS_FILE_NOTFOUND,
                "repository_id.txt is no folder, has nothing below it and is only in the repository's folder");
    DeleteFileW(local);
}

/* Scenario C: full end-to-end navigation (Level 0 -> Level 1 -> Level 2+, password retry & session) */
static void test_live_end_to_end_navigation(void) {
    if (GetFileAttributesW(L"tmp\\restic.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\test_fixture\\repo\\config") == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe or tmp\\test_fixture\\repo not found: E2E DLL test skipped.\n");
        return;
    }

    WCHAR restic_abs[MAX_PATH] = {0}, repo_abs[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_abs, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\repo", MAX_PATH, repo_abs, NULL);

    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_e2e", dir, MAX_PATH), "Create E2E plugin directory");
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);

    char restic_a[MAX_PATH] = {0}, repo_a[MAX_PATH] = {0}, dir_a[MAX_PATH] = {0};
    static char ini_text[6144]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    WideCharToMultiByte(CP_ACP, 0, restic_abs, -1, restic_a, sizeof(restic_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, dir, -1, dir_a, sizeof(dir_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, repo_abs, -1, repo_a, sizeof(repo_a), NULL, NULL);
    char long_alias[256];
    memset(long_alias, 'L', 250);
    long_alias[250] = '\0';
    int ini_len = snprintf(
        ini_text,
        sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\nSnapshotTimeFormat=YYYY-MM-DDThh.mm.ss\r\n" /* ConnectionLog: default */
        "CsvSeparator=;\r\n"
        "[Logging]\r\nLogPath=%s\\log dir\\e2e.log\r\n"
        "[Repositories]\r\nMainRepo=%s\r\nSecondRepo=%s\r\nThirdRepo=%s\r\n"
        "ExtractRepo=%s\r\nHintRepo=%s\r\n"
        "MissingRepo=C:\\no_such_restic_repo_999\r\n%s=C:\\no_such_restic_repo_999\r\n",
        restic_a,
        dir_a,
        repo_a,
        repo_a,
        repo_a,
        repo_a,
        repo_a,
        long_alias
    );
    for (int i = 1; i <= 10 && ini_len > 0 && (size_t)ini_len < sizeof(ini_text); ++i) { /* More than 8 slots */
        int wrote = snprintf(ini_text + ini_len, sizeof(ini_text) - (size_t)ini_len, "Slot%02d=%s\r\n", i, repo_a);
        ini_len = wrote < 0 || (size_t)wrote >= sizeof(ini_text) - (size_t)ini_len ? -1 : ini_len + wrote;
    }
    EXPECT_TRUE(ini_len > 0 && (size_t)ini_len < sizeof(ini_text), "E2E INI fits its buffer");
    EXPECT_TRUE(write_file(ini, ini_text), "Write E2E INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy for E2E");
    EXPECT_TRUE(p.init_w(42, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks");

    /* 1. Level 1 (\MainRepo): wrong password on first prompt -> automatic retry -> restic mount's folders */
    g_e2e_pwd_mode    = 0;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    g_e2e_error_logs  = 0;

    WIN32_FIND_DATAW fd;
    static WCHAR names[1024];
    EXPECT_TRUE(list_names(&p, L"\\MainRepo", names, _countof(names)) &&
                    wcscmp(names, L"hosts|ids|snapshots|tags|snapshots.csv|snapshots.json|repository_id.txt") == 0,
                "Level 1 shows restic mount's folders, snapshots.csv, snapshots.json and repository_id.txt after a password retry");
    EXPECT_TRUE(g_e2e_pwd_prompts == 2, "Prompted twice (initial + 1 retry after wrong password)");
    EXPECT_TRUE(wcsstr(g_e2e_last_prompt, L"Wrong password") != NULL, "Retry prompt tells the user the password was wrong");
    EXPECT_TRUE(wcscmp(g_e2e_last_title, L"MainRepo - Restic repository password") == 0,
                "The dialog title names the repository (for password managers matching window titles)");
    EXPECT_TRUE(g_e2e_msg_dialogs == 0, "No error dialog when retry succeeds");

    /* The levels above the snapshots come from the one snapshot list cached for this repository */
    g_e2e_pwd_prompts = 0;
    HANDLE h = p.find_first_w(L"\\MainRepo\\snapshots", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "snapshots/ listed");
    WCHAR latest_snap[MAX_PATH] = {0};
    size_t snap_count = 0;
    BOOL has_latest = FALSE;
    WCHAR latest_link[MAX_PATH] = {0}, latest_folder_link[MAX_PATH] = {0};
    do {
        snap_count++;
        EXPECT_TRUE((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && fd.ftLastWriteTime.dwHighDateTime != 0xFFFFFFFF,
                    "Snapshot folders carry the snapshot time");
        if (wcscmp(fd.cFileName, L"latest") == 0) {
            has_latest = TRUE;
        } else if (wcsncmp(fd.cFileName, L"latest \u2192 ", 9) == 0) {
            /* latest → <short id> (8 characters) or latest → <the latest snapshot's folder> */
            wcsncpy_s(wcslen(fd.cFileName) == 17 ? latest_link : latest_folder_link, MAX_PATH, fd.cFileName, _TRUNCATE);
        } else if (wcscmp(fd.cFileName, latest_snap) > 0) {
            wcsncpy_s(latest_snap, MAX_PATH, fd.cFileName, _TRUNCATE); /* Names sort by time */
        }
    } while (p.find_next_w(h, &fd));
    p.find_close(h);
    EXPECT_TRUE(snap_count == 5 && has_latest && wcslen(latest_link) == 17,
                "Both fixture snapshots, latest, latest → <short id> and latest → <folder> in snapshots/");
    EXPECT_TRUE(wcscmp(latest_folder_link + 9, latest_snap) == 0,
                "latest → <folder> names the newest snapshot's folder, as restic mount's latest link does");
    EXPECT_TRUE(latest_snap[4] == L'-' && latest_snap[10] == L'T' && latest_snap[13] == L'.' && wcslen(latest_snap) >= 19,
                "Named by [Settings] SnapshotTimeFormat=YYYY-MM-DDThh.mm.ss");
    EXPECT_TRUE(list_names(&p, L"\\MainRepo\\ids", names, _countof(names)) && wcslen(names) == 17 && names[8] == L'|',
                "ids/ holds two 8-character short IDs");
    WCHAR short_id[9] = {0};
    wcsncpy_s(short_id, _countof(short_id), names, 8);
    EXPECT_TRUE(list_names(&p, L"\\MainRepo\\hosts", names, _countof(names)) && wcschr(names, L'|') == NULL &&
                names[0] != L'\0', "hosts/ holds the fixture's single host");
    static WCHAR host_dir[MAX_PATH * 2];
    swprintf_s(host_dir, _countof(host_dir), L"\\MainRepo\\hosts\\%ls", names);
    EXPECT_TRUE(list_names(&p, host_dir, names, _countof(names)) && wcsstr(names, L"|latest") != NULL,
                "hosts/<host>/ holds the snapshots and latest");
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\MainRepo\\tags", &fd) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_NO_MORE_FILES,
                "tags/ is empty: the fixture snapshots have no tags");
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\MainRepo\\no-such-folder", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Unknown folder below the alias -> ERROR_PATH_NOT_FOUND");
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\MainRepo\\snapshots\\1999-01-01_00-00-00", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Unknown snapshot -> ERROR_PATH_NOT_FOUND");
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 0, "No prompt and no dialog while browsing the tree");
    test_snapshots_csv(&p, dir, short_id, latest_link + 9);
    test_snapshots_json(&p, dir, short_id, latest_link + 9);
    test_repository_id(&p, dir);

    /* The snapshot root is the same through latest and ids/ */
    static WCHAR via_latest[1024], via_id[1024];
    EXPECT_TRUE(list_names(&p, L"\\MainRepo\\snapshots\\latest", via_latest, _countof(via_latest)) && via_latest[0],
                "latest/ shows the newest snapshot's root");
    static WCHAR id_path[MAX_PATH];
    swprintf_s(id_path, _countof(id_path), L"\\MainRepo\\ids\\%ls", short_id);
    EXPECT_TRUE(list_names(&p, id_path, via_id, _countof(via_id)) && wcscmp(via_id, via_latest) == 0,
                "ids/<short id>/ shows a snapshot root");
    static WCHAR link_path[MAX_PATH], via_link[1024], latest_id_path[MAX_PATH], via_latest_id[1024];
    swprintf_s(link_path, _countof(link_path), L"\\MainRepo\\snapshots\\%ls", latest_link);
    swprintf_s(latest_id_path, _countof(latest_id_path), L"\\MainRepo\\ids\\%ls", latest_link + 9);
    EXPECT_TRUE(list_names(&p, link_path, via_link, _countof(via_link)) && wcscmp(via_link, via_latest) == 0 &&
                list_names(&p, latest_id_path, via_latest_id, _countof(via_latest_id)) &&
                wcscmp(via_latest_id, via_latest) == 0, "latest → <short id> is latest, and ids/<that id> too");

    /* 2. Level 2+ (\MainRepo\<snap>\...): session password cached in RAM (0 additional prompts) */
    g_e2e_pwd_prompts = 0;
    static WCHAR snap_root[MAX_PATH * 2], data_path[MAX_PATH * 2]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    swprintf_s(snap_root, _countof(snap_root), L"\\MainRepo\\snapshots\\%ls", latest_snap);
    EXPECT_TRUE(descend_to_fixture_data(&p, snap_root, 0, data_path, _countof(data_path)),
                "Descend snapshot directory tree down to fixture data folder");

    h = p.find_first_w(data_path, &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "List data directory inside snapshot");
    BOOL saw_readme = FALSE, saw_empty_dir = FALSE;
    do {
        if (wcscmp(fd.cFileName, L"readme.txt") == 0 && fd.nFileSizeLow == 5) saw_readme = TRUE;
        if (wcscmp(fd.cFileName, L"empty_dir") == 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            saw_empty_dir = TRUE;
        }
    } while (p.find_next_w(h, &fd));
    p.find_close(h);
    EXPECT_TRUE(saw_readme && saw_empty_dir, "data folder contains readme.txt (5 B) and empty_dir");

    /* Empty directory inside snapshot -> ERROR_NO_MORE_FILES, no error popup */
    static WCHAR empty_path[MAX_PATH * 2];
    swprintf_s(empty_path, _countof(empty_path), L"%ls\\empty_dir", data_path);
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(empty_path, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_NO_MORE_FILES, "Empty snapshot directory returns ERROR_NO_MORE_FILES");

    /* 2b. File extraction via FsGetFileW + recursive directory copy (as Total Commander does on F5) */
    test_e2e_file_and_folder_extraction(&p, dir, data_path);
    test_e2e_extraction_edge_cases(&p, dir, data_path);

    /* Non-existent directory inside snapshot -> ERROR_PATH_NOT_FOUND, no error popup */
    static WCHAR missing_sub[MAX_PATH * 2];
    swprintf_s(missing_sub, _countof(missing_sub), L"%ls\\does_not_exist", data_path);
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(missing_sub, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Missing snapshot path returns ERROR_PATH_NOT_FOUND");
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 0,
                "All navigations within MainRepo reused the cached RAM password with no popups");
    test_e2e_snapshot_cache(&p, dir, data_path);
    test_e2e_file_history(&p, dir, snap_root, data_path);

    /* 3. Repositories keep independent sessions: SecondRepo prompts once, MainRepo is NOT asked again */
    g_e2e_pwd_mode = 1;
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Level 1 on SecondRepo succeeds");
    p.find_close(h);
    EXPECT_TRUE(g_e2e_pwd_prompts == 1, "First visit of another repository prompts exactly once");
    h = p.find_first_w(L"\\MainRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Back to MainRepo");
    p.find_close(h);
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "And again SecondRepo");
    p.find_close(h);
    EXPECT_TRUE(g_e2e_pwd_prompts == 1, "Alternating between two panels' repositories never re-prompts");

    /* The connection log (on by default): one connection for all repositories, announced once */
    EXPECT_TRUE(conn_log_count(L"1:CONNECT \\\n") == 1 && conn_log_count(L"1:CONNECT") == 1,
                "One connection (MSGTYPE_CONNECT \"CONNECT \\\"), announced when a repository was first used, "
                "not again for another repository nor after the forget entry");
    /* What the log shows besides the background work */
    EXPECT_TRUE(conn_log_count(L"3:[MainRepo] Password refused by restic\n") >= 1 &&
                conn_log_count(L"3:[MainRepo] Password accepted\n") >= 1 && conn_log_count(L"3:[SecondRepo] Password accepted\n") == 1,
                "Passwords refused and accepted are logged (the connection is announced before the first prompt)");
    EXPECT_TRUE(conn_log_count(L"3:[MainRepo] Snapshot list read: 2 snapshots, ") >= 1,
                "The snapshot list read in the foreground is logged, also on the very first visit");
    EXPECT_TRUE(conn_log_count(L" entries listed by restic in ") >= 1, "Folders listed by restic are logged with the time");
    EXPECT_TRUE(conn_log_count(L"4:\\MainRepo\\snapshots\\") >= 1 && conn_log_count(L"\\readme.txt -> ") >= 1,
                "Files copied are logged as MSGTYPE_TRANSFERCOMPLETE \"source -> target\"");
    /* Its Disconnect button forgets every password and what was read: the next visits ask again */
    EXPECT_TRUE(p.disconnect_w != NULL && p.disconnect_w(L"\\"), "FsDisconnectW succeeds");
    EXPECT_TRUE(conn_log_count(L"2:DISCONNECT \\\n") == 1 &&
                conn_log_count(L"3:All remembered passwords and cached listings forgotten\n") == 1,
                "Disconnect is logged (MSGTYPE_DISCONNECT)");
    EXPECT_TRUE(p.disconnect_w(L"\\") && p.disconnect_w(NULL) && conn_log_count(L"2:DISCONNECT") == 1,
                "Disconnecting again succeeds and does nothing");
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 2, "After Disconnect the password is asked again");
    p.find_close(h);
    EXPECT_TRUE(conn_log_count(L"1:CONNECT \\\n") == 2, "...and the plugin connected again");
    h = p.find_first_w(L"\\MainRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 3 && conn_log_count(L"1:CONNECT \\\n") == 2,
                "Disconnect forgot every repository's password; the connection is not announced twice");
    p.find_close(h);
    /* 4. Exhausting all 3 password attempts returns ERROR_ACCESS_DENIED and shows an error dialog */
    g_e2e_pwd_mode    = 2;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    g_e2e_error_logs  = 0;
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\ThirdRepo", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_ACCESS_DENIED, "3 wrong passwords -> ERROR_ACCESS_DENIED");
    EXPECT_TRUE(g_e2e_pwd_prompts == 3, "Asked password 3 times before giving up");
    EXPECT_TRUE(g_e2e_msg_dialogs == 1 && g_e2e_error_logs == 1, "Notified user and log after password failure");
    /* A wrong password must not stay cached: the next visit prompts again (and succeeds now) */
    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\ThirdRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "Rejected password was not cached");
    p.find_close(h);

    /* 4b. Empty password and cancelled dialog: ERROR_CANCELLED, no popup, nothing cached */
    g_e2e_msg_dialogs = 0;
    for (int mode = 3; mode <= 4; ++mode) {
        g_e2e_pwd_mode    = mode;
        g_e2e_pwd_prompts = 0;
        SetLastError(0);
        EXPECT_TRUE(p.find_first_w(L"\\Slot01", &fd) == INVALID_HANDLE_VALUE &&
                    GetLastError() == ERROR_CANCELLED, "Empty/cancelled password -> ERROR_CANCELLED");
        EXPECT_TRUE(g_e2e_pwd_prompts == 1 && g_e2e_msg_dialogs == 0, "Single prompt and no error dialog");
    }
    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\Slot01", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "Nothing was cached after empty/cancelled input");
    p.find_close(h);

    /* 4c. Session table has 8 slots: visiting 10 repositories recycles the oldest ones */
    g_e2e_pwd_prompts = 0;
    for (int i = 2; i <= 10; ++i) {
        WCHAR slot_path[32];
        swprintf_s(slot_path, _countof(slot_path), L"\\Slot%02d", i);
        h = p.find_first_w(slot_path, &fd);
        EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Slot repository listed");
        p.find_close(h);
    }
    EXPECT_TRUE(g_e2e_pwd_prompts == 9, "Each new repository prompted once");
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\Slot10", &fd);
    p.find_close(h);
    EXPECT_TRUE(g_e2e_pwd_prompts == 0, "Most recent repository still cached");
    h = p.find_first_w(L"\\Slot01", &fd);
    p.find_close(h);
    EXPECT_TRUE(g_e2e_pwd_prompts == 1, "Oldest session was evicted and requires a new password (no crash, no corruption)");
    BOOL recycled_logged = FALSE; /* Logged by a worker: the eviction happens with the session lock held */
    for (int i = 0; i < 100 && !recycled_logged; ++i) {
        recycled_logged = conn_log_count(L"to make room for another repository's (8 are remembered at most)") >= 1;
        if (!recycled_logged) Sleep(50);
    }
    EXPECT_TRUE(recycled_logged, "A password forgotten to make room is logged");

    /* 5. Non-existent repository (\MissingRepo) -> exit code 10 -> ERROR_PATH_NOT_FOUND + dialog */
    g_e2e_pwd_mode    = 1;
    g_e2e_msg_dialogs = 0;
    g_e2e_error_logs  = 0;
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\MissingRepo", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Missing repository returns ERROR_PATH_NOT_FOUND");
    EXPECT_TRUE(g_e2e_msg_dialogs == 1 && g_e2e_error_logs == 1, "Missing repository notifies user and log");
    /* Unvalidated password for a repository that failed to open is not kept */
    g_e2e_pwd_prompts = 0;
    p.find_first_w(L"\\MissingRepo", &fd);
    EXPECT_TRUE(g_e2e_pwd_prompts == 1, "Password of an unreachable repository is not cached");

    /* 5b. A 250-character alias plus a long Restic message must not overflow any message buffer */
    WCHAR long_path[300] = L"\\";
    for (int i = 0; i < 250; ++i) long_path[1 + i] = L'L';
    long_path[251] = L'\0';
    g_e2e_msg_dialogs = 0;
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(long_path, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND && g_e2e_msg_dialogs == 1,
                "Very long alias is reported without crashing");

    /* 6. Pressing ESC in Total Commander (ProgressProc returns 1) cancels without error dialog */
    g_e2e_pwd_mode       = 1;
    g_e2e_force_abort    = TRUE;
    g_e2e_progress_calls = 0;
    g_e2e_msg_dialogs    = 0;
    g_e2e_progress_target[0] = L'\0';
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(L"\\SecondRepo", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_CANCELLED, "User abort via ProgressProc returns ERROR_CANCELLED");
    EXPECT_TRUE(g_e2e_progress_calls > 0 && g_e2e_msg_dialogs == 0,
                "ProgressProc was polled and no error dialog was shown on user cancel");
    EXPECT_TRUE(g_e2e_progress_percent == 0, "Progress is reported as indeterminate (0 %)");
    EXPECT_TRUE(wcsstr(g_e2e_progress_target, repo_abs) == NULL && wcsstr(g_e2e_progress_target, L"SecondRepo") != NULL,
                "Progress dialog shows the alias, never the repository URL (it may contain credentials)");
    g_e2e_force_abort = FALSE;
    /* A password restic has not accepted yet is not kept after ESC: the next visit asks again */
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "Unverified password not cached after a user abort");
    p.find_close(h);
    /* A password restic has accepted survives a later ESC (inside a snapshot: the folders above come from
       the cached snapshot list and run no restic that could be aborted) */
    g_e2e_force_abort = TRUE;
    EXPECT_TRUE(p.find_first_w(L"\\SecondRepo\\snapshots\\latest", &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_CANCELLED, "Second user abort");
    g_e2e_force_abort = FALSE;
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 0, "Verified password survives a user abort");
    p.find_close(h);

    /* 7. Concurrent listings from several threads (TC may call the plugin from worker threads) */
    {
        enum { THREADS = 6 };
        HANDLE threads[THREADS];
        MtJob jobs[THREADS];
        g_e2e_mt_failures = 0;
        for (int i = 0; i < THREADS; ++i) {
            jobs[i].plugin = &p;
            jobs[i].path   = (i % 2) ? L"\\MainRepo\\snapshots" : L"\\SecondRepo\\snapshots";
            threads[i]     = CreateThread(NULL, 0, mt_list_worker, &jobs[i], 0, NULL);
        }
        WaitForMultipleObjects(THREADS, threads, TRUE, 120000);
        for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
        EXPECT_TRUE(g_e2e_mt_failures == 0, "Concurrent listings all succeed (cached sessions, thread-safe)");
    }

    /* A rejected password with a trailing space: the retry prompt (and the log) says so */
    g_e2e_pwd_mode    = 5;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    h = p.find_first_w(L"\\HintRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) p.find_close(h);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 2 && g_e2e_msg_dialogs == 0,
                "Password with a trailing space rejected, then the correct one accepted");
    EXPECT_TRUE(wcsstr(g_e2e_last_prompt, L"Wrong password. Note: the password you entered ends with a space.") != NULL,
                "Retry prompt points out the trailing space");

    /* While passwords are cached, the root starts with "<wastebasket> Forget N Passwords"; Enter on it
       wipes them all, reports how many and sends Total Commander back to the root (re-read) */
    WCHAR forget[MAX_PATH] = L"\\", exec_name[MAX_PATH];
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && wcsncmp(fd.cFileName, L"\U0001F5D1 Forget ", 9) == 0 &&
                wcscmp(fd.cFileName + wcslen(fd.cFileName) - 10, L" Passwords") == 0 &&
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
                "Root starts with the 'Forget N Passwords' file while passwords are cached");
    wcscat_s(forget, MAX_PATH, fd.cFileName);
    p.find_close(h);
    WCHAR open_verb[] = L"open";
    g_e2e_msg_dialogs = 0;
    wcscpy_s(exec_name, MAX_PATH, forget);
    EXPECT_TRUE(p.execute_w(NULL, exec_name, open_verb) == FS_EXEC_SYMLINK && g_e2e_msg_dialogs == 1 &&
                wcsstr(g_e2e_last_msg, L"remembered repository passwords.") != NULL && wcscmp(exec_name, L"\\") == 0,
                "Forget wipes the cache, reports the number and sends Total Commander back to the root");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && wcsncmp(fd.cFileName, L"\U0001F5D1", 2) != 0,
                "The forget entry disappears once nothing is cached");
    p.find_close(h);
    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\MainRepo", &fd);
    if (h != INVALID_HANDLE_VALUE) p.find_close(h);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "A forgotten password is asked for again");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && wcscmp(fd.cFileName, L"\U0001F5D1 Forget 1 Password") == 0,
                "Singular entry name");
    p.find_close(h);
    /* An INI without repositories still lists the entry, so cached passwords can always be wiped */
    EXPECT_TRUE(write_file(ini, "[Settings]\r\n[Repositories]\r\n"), "Write an INI without repositories");
    h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && wcscmp(fd.cFileName, L"\U0001F5D1 Forget 1 Password") == 0 &&
                !p.find_next_w(h, &fd),
                "Forget entry listed alone when the INI has no repositories");
    if (h != INVALID_HANDLE_VALUE) p.find_close(h);
    EXPECT_TRUE(write_file(ini, ini_text), "Restore the E2E INI");
    h = p.find_first_w(L"\\", &fd); /* Reads it again, which also turns the log back on */
    if (h != INVALID_HANDLE_VALUE) p.find_close(h);
    wcscpy_s(exec_name, MAX_PATH, forget);
    EXPECT_TRUE(p.execute_w(NULL, exec_name, open_verb) == FS_EXEC_SYMLINK &&
                wcscmp(g_e2e_last_msg, L"Forgot 1 remembered repository password.") == 0,
                "A stale entry name (other count) still forgets; singular message");
    wcscpy_s(exec_name, MAX_PATH, forget);
    EXPECT_TRUE(p.execute_w(NULL, exec_name, open_verb) == FS_EXEC_SYMLINK &&
                wcscmp(g_e2e_last_msg, L"No repository password was remembered.") == 0 &&
                wcscmp(exec_name, L"\\") == 0,
                "Nothing to forget: a stale entry still sends Total Commander back to the root");
    WCHAR some_file[] = L"\\MainRepo\\snapshots\\2026-10-18_12-00-00\\readme.txt";
    WCHAR properties[] = L"properties", chmod_verb[] = L"chmod 755", quote_verb[] = L"quote cd x";
    EXPECT_TRUE(p.execute_w(NULL, some_file, open_verb) == FS_EXEC_YOURSELF,
                "Enter on a snapshot file is left to Total Commander (download and open)");
    EXPECT_TRUE(p.execute_w(NULL, forget, properties) == FS_EXEC_OK &&
                p.execute_w(NULL, some_file, chmod_verb) == FS_EXEC_ERROR &&
                p.execute_w(NULL, some_file, quote_verb) == FS_EXEC_ERROR,
                "properties is a no-op, chmod and quote commands are refused");
    WCHAR mode_binary[] = L"MODE I", mode_text[] = L"mode A", mode_auto[] = L"MODE X *.txt *.ini", modem[] = L"modem";
    WCHAR repo_dir[] = L"\\MainRepo";
    EXPECT_TRUE(p.execute_w(NULL, repo_dir, mode_binary) == FS_EXEC_OK && p.execute_w(NULL, repo_dir, mode_text) == FS_EXEC_OK &&
                p.execute_w(NULL, repo_dir, mode_auto) == FS_EXEC_OK && p.execute_w(NULL, repo_dir, modem) == FS_EXEC_ERROR,
                "The toolbar's transfer modes are accepted and ignored (files are copied byte for byte)");
    EXPECT_TRUE(p.find_first_w(forget, &fd) == INVALID_HANDLE_VALUE, "The forget entry is not a folder");

    /* [Logging] LogPath: restic runs, exit codes and prompts are logged; passwords never are */
    WCHAR log_dir[MAX_PATH], log_file[MAX_PATH];
    swprintf_s(log_dir, MAX_PATH, L"%ls\\log dir", dir);
    swprintf_s(log_file, MAX_PATH, L"%ls\\e2e.log", log_dir);
    char* log_text = read_text_file(log_file);
    EXPECT_TRUE(log_text != NULL, "Diagnostic log written to [Logging] LogPath");
    char fixture_pwd_u8[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, FIXTURE_PASSWORD, -1, fixture_pwd_u8, sizeof(fixture_pwd_u8), NULL, NULL);
    BOOL logged_init   = strstr(log_text, "FsInitW: TCResticBrowser") != NULL;
    BOOL logged_run    = strstr(log_text, "restic run: ") != NULL && strstr(log_text, "snapshots --json --no-lock") != NULL;
    BOOL logged_wrong  = strstr(log_text, "exit code 12 after ") != NULL &&
                         strstr(log_text, "wrong password or no key found") != NULL;
    BOOL logged_prompt = strstr(log_text, "Password prompt for 'MainRepo'") != NULL &&
                         strstr(log_text, "characters entered") != NULL;
    BOOL logged_error  = strstr(log_text, "Error shown to the user: ") != NULL;
    BOOL logged_forget = strstr(log_text, "Forget All Passwords: 1 cached password(s) wiped") != NULL;
    BOOL logged_hint   = strstr(log_text, "Password prompt for 'HintRepo': ") != NULL &&
                         strstr(log_text, "characters entered; the password ends with a space") != NULL;
    BOOL leaked        = strstr(log_text, fixture_pwd_u8) != NULL || strstr(log_text, "wrong_first_attempt") != NULL ||
                         strstr(log_text, "always_wrong") != NULL;
    free(log_text);
    EXPECT_TRUE(logged_init && logged_run && logged_wrong, "Log has the plugin start, restic command lines and exit codes");
    EXPECT_TRUE(logged_prompt && logged_error, "Log has password prompts (length only) and errors shown to the user");
    EXPECT_TRUE(logged_hint, "Log notes a trailing space in an entered password");
    EXPECT_TRUE(logged_forget, "Log notes Forget All Passwords");
    EXPECT_TRUE(!leaked, "No password, right or wrong, appears in the log");

    unload_plugin_copy(&p, dir);
    DeleteFileW(ini);
    DeleteFileW(log_file);
    RemoveDirectoryW(log_dir);
    RemoveDirectoryW(dir);
}

/* Copies the directory tree src into dst (created); used to place the fixture repository elsewhere */
static BOOL copy_tree(const WCHAR* src, const WCHAR* dst) {
    if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    WCHAR pattern[MAX_PATH * 2];
    swprintf_s(pattern, _countof(pattern), L"%ls\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        WCHAR from[MAX_PATH * 2], to[MAX_PATH * 2];
        swprintf_s(from, _countof(from), L"%ls\\%ls", src, fd.cFileName);
        swprintf_s(to, _countof(to), L"%ls\\%ls", dst, fd.cFileName);
        ok = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? copy_tree(from, to) : CopyFileW(from, to, FALSE);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* Deletes the directory tree path (best effort) */
static void delete_tree(const WCHAR* path) {
    WCHAR pattern[MAX_PATH * 2];
    swprintf_s(pattern, _countof(pattern), L"%ls\\*", path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            WCHAR child[MAX_PATH * 2];
            swprintf_s(child, _countof(child), L"%ls\\%ls", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

/*
 * A restic older than 0.17.1 (fake_restic.exe, built next to this test, printing the version
 * FAKE_RESTIC_VERSION names, as text with FAKE_RESTIC_VERSION_TEXT like restic before 0.17.0) is
 * refused with a message before any prompt, and asked for its version once, not before every
 * listing; 0.17.0, the first to answer in JSON, is still too old
 */
static void test_old_restic_refused(void) {
    WCHAR fake[MAX_PATH], dir[MAX_PATH], ini[MAX_PATH], log_file[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");
    if (GetFileAttributesW(fake) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] fake_restic.exe not found next to the test: old restic test skipped.\n");
        return;
    }
    EXPECT_TRUE(make_unique_dir(L"restic_dll_old", dir, MAX_PATH), "Create plugin directory (old restic)");
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);
    swprintf_s(log_file, MAX_PATH, L"%ls\\old.log", dir);
    DeleteFileW(log_file); /* Left by a run that stopped early: the directory is named after the process ID */
    char fake_u8[MAX_PATH * 3] = {0}, log_u8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, fake, -1, fake_u8, sizeof(fake_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, log_file, -1, log_u8, sizeof(log_u8), NULL, NULL);
    static char ini_text[2048];
    int n = snprintf(ini_text, sizeof(ini_text),
                     "[Settings]\r\nResticPath=%s\r\nConnectionLog=off\r\n[Logging]\r\nLogPath=%s\r\n"
                     "[Repositories]\r\nOld=C:\\no_such_restic_repo_999\r\n",
                     fake_u8, log_u8);
    EXPECT_TRUE(n > 0 && (size_t)n < sizeof(ini_text) && write_file(ini, ini_text), "Write INI (old restic)");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy (old restic)");
    EXPECT_TRUE(p.init_w(45, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks (old restic)");
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION", L"0.16.4");
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION_TEXT", L"1");
    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    WIN32_FIND_DATAW fd;
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(p.find_first_w(L"\\Old\\snapshots", &fd) == INVALID_HANDLE_VALUE, "An old restic is not used");
    }
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION_TEXT", NULL);
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 2 && wcsstr(g_e2e_last_msg, L"older than restic 0.17.0") &&
                    wcsstr(g_e2e_last_msg, L"0.17.1 or newer"),
                "... and each attempt says why, before any password prompt");

    /* Another restic.exe (another file: checked anew) that answers in JSON, but is 0.17.0 */
    WCHAR fake2[MAX_PATH];
    swprintf_s(fake2, MAX_PATH, L"%ls\\restic-0.17.0.exe", dir);
    char fake2_u8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, fake2, -1, fake2_u8, sizeof(fake2_u8), NULL, NULL);
    n = snprintf(ini_text, sizeof(ini_text),
                 "[Settings]\r\nResticPath=%s\r\nConnectionLog=off\r\n[Logging]\r\nLogPath=%s\r\n"
                 "[Repositories]\r\nOld=C:\\no_such_restic_repo_999\r\n",
                 fake2_u8, log_u8);
    EXPECT_TRUE(CopyFileW(fake, fake2, FALSE) && n > 0 && (size_t)n < sizeof(ini_text) && write_file(ini, ini_text),
                "Point ResticPath at a restic 0.17.0");
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION", L"0.17.0");
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(p.find_first_w(L"\\Old\\snapshots", &fd) == INVALID_HANDLE_VALUE && g_e2e_msg_dialogs == 1 &&
                    wcsstr(g_e2e_last_msg, L"is restic 0.17.0") && g_e2e_pwd_prompts == 0,
                "restic 0.17.0 is refused too, naming its version");
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION", NULL);
    char* log_text = read_text_file(log_file);
    int version_runs = 0;
    for (const char* q = log_text; q && (q = strstr(q, " --json version")) != NULL; ++q) version_runs++;
    free(log_text);
    EXPECT_TRUE(version_runs == 2, "restic is asked for its version once per file");

    unload_plugin_copy(&p, dir);
    delete_tree(dir);
}

/* Lists \<alias>\snapshots; TRUE when it worked and has a latest folder */
static BOOL lists_snapshots(const Plugin* p, const WCHAR* alias) {
    WCHAR path[MAX_PATH];
    static WCHAR names[2048];
    swprintf_s(path, MAX_PATH, L"\\%ls\\snapshots", alias);
    return list_names(p, path, names, _countof(names)) && wcsstr(names, L"latest") != NULL;
}

/*
 * Scenario E: [Repository: <alias>] settings. A password from PasswordFile or PasswordCommand opens
 * the repository without a prompt, also from a .cmd in a folder with spaces; both together are
 * passed to restic, which refuses them; a wrong one is reported, not asked for again; an unusable
 * setting stops the repository before restic runs. AWS_ACCESS_KEY_ID makes the plugin ask for the
 * S3 secret key (restic ignores it for a local repository), before the password if that is asked
 * for too, and caches it like a password.
 */
static void test_live_repository_settings(void) {
    if (GetFileAttributesW(L"tmp\\restic.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\test_fixture\\repo\\config") == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe or tmp\\test_fixture\\repo not found: repository settings E2E test skipped.\n");
        return;
    }
    WCHAR restic_abs[MAX_PATH] = {0}, repo_abs[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_abs, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\repo", MAX_PATH, repo_abs, NULL);

    WCHAR dir[MAX_PATH], tools[MAX_PATH], path[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_settings", dir, MAX_PATH), "Create plugin directory");
    swprintf_s(tools, MAX_PATH, L"%ls\\tools dir", dir);
    EXPECT_TRUE(CreateDirectoryW(tools, NULL) || GetLastError() == ERROR_ALREADY_EXISTS, "Create a folder with spaces");
    char fixture_pwd_u8[128] = {0};
    WideCharToMultiByte(CP_UTF8, 0, FIXTURE_PASSWORD, -1, fixture_pwd_u8, sizeof(fixture_pwd_u8), NULL, NULL);
    swprintf_s(path, MAX_PATH, L"%ls\\pw.txt", dir);
    EXPECT_TRUE(write_file(path, fixture_pwd_u8), "Write the password file");
    swprintf_s(path, MAX_PATH, L"%ls\\wrong.txt", dir);
    EXPECT_TRUE(write_file(path, "not the password\r\n"), "Write a wrong password file");
    swprintf_s(path, MAX_PATH, L"%ls\\get pw.cmd", tools);
    EXPECT_TRUE(write_file(path, "@type \"%~dp0..\\pw.txt\"\r\n"), "Write a password script in a folder with spaces");

    char restic_u8[MAX_PATH * 3] = {0}, repo_u8[MAX_PATH * 3] = {0}, dir_u8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, restic_abs, -1, restic_u8, sizeof(restic_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, repo_abs, -1, repo_u8, sizeof(repo_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dir_u8, sizeof(dir_u8), NULL, NULL);
    static char ini_text[8192]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    int ini_len = snprintf(
        ini_text, sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\n"
        "[Logging]\r\nLogPath=%s\\settings.log\r\n"
        "[Repositories]\r\nFileRepo=%s\r\nCommandRepo=%s\r\nQuotedCommand=%s\r\nBothRepo=%s\r\nWrongFile=%s\r\n"
        "BadSetting=%s\r\nS3Key=%s\r\nS3Typed=%s\r\nBadOption=%s\r\n"
        "[Repository: FileRepo]\r\nPasswordFile=pw.txt\r\nlocal.connections=1\r\ns3.region=ignored-by-local\r\n"
        "[Repository: BadOption]\r\nPasswordFile=pw.txt\r\nlocal.no-such-option=1\r\n"
        "[Repository: CommandRepo]\r\nPasswordCommand=%s\\tools dir\\get pw.cmd\r\n"
        "[Repository: QuotedCommand]\r\nPasswordCommand=\"%s\\tools dir\\get pw.cmd\" unused-argument\r\n"
        "[Repository: BothRepo]\r\nPasswordFile=pw.txt\r\nPasswordCommand=%s\\tools dir\\get pw.cmd\r\n"
        "[Repository: WrongFile]\r\nPasswordFile=wrong.txt\r\n"
        "[Repository: BadSetting]\r\nPasswordFile=missing.txt\r\n"
        "[Repository: S3Key]\r\nPasswordFile=pw.txt\r\nAWS_ACCESS_KEY_ID=AKIATESTKEY\r\nAWS_PROFILE=backup\r\n"
        "[Repository: S3Typed]\r\nAWS_ACCESS_KEY_ID=AKIATESTKEY\r\n",
        restic_u8, dir_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8, dir_u8,
        dir_u8, dir_u8);
    EXPECT_TRUE(ini_len > 0 && (size_t)ini_len < sizeof(ini_text), "Settings INI fits its buffer");
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);
    EXPECT_TRUE(write_file(ini, ini_text), "Write settings INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy (settings)");
    EXPECT_TRUE(p.init_w(44, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks (settings)");

    /* A prompt would be answered wrongly: none may come */
    g_e2e_pwd_mode = 2;
    static const WCHAR* const opened[] = {L"FileRepo", L"CommandRepo", L"QuotedCommand"};
    for (size_t i = 0; i < _countof(opened); ++i) {
        g_e2e_pwd_prompts = 0;
        g_e2e_msg_dialogs = 0;
        EXPECT_TRUE(lists_snapshots(&p, opened[i]) && g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 0,
                    "PasswordFile / PasswordCommand: the repository opens without a prompt");
        EXPECT_TRUE(lists_snapshots(&p, opened[i]) && g_e2e_pwd_prompts == 0, "... and again from the cache");
    }

    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(!lists_snapshots(&p, L"BothRepo") && g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 1 &&
                    wcsstr(g_e2e_last_msg, L"mutually exclusive") != NULL,
                "PasswordFile and PasswordCommand together: restic's refusal shown, no prompt");
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(!lists_snapshots(&p, L"WrongFile") && g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 1,
                "A wrong password from a file is reported once and not asked for");
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(!lists_snapshots(&p, L"BadSetting") && g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 1 &&
                    wcsstr(g_e2e_last_msg, L"PasswordFile=missing.txt") != NULL,
                "An unusable setting is shown before anything runs");
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(!lists_snapshots(&p, L"BadOption") && g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 1 &&
                    wcsstr(g_e2e_last_msg, L"is not known") != NULL,
                "An option restic does not know: restic's refusal shown (FileRepo's local.connections worked)");

    /* AWS_ACCESS_KEY_ID: the secret key is asked for once, then cached */
    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(lists_snapshots(&p, L"S3Key") && g_e2e_pwd_prompts == 1 &&
                    wcscmp(g_e2e_last_title, L"S3Key - S3 secret access key") == 0 &&
                    wcsstr(g_e2e_last_prompt, L"AKIATESTKEY") != NULL,
                "AWS_ACCESS_KEY_ID: the S3 secret key is asked for, naming the repository and the key");
    EXPECT_TRUE(lists_snapshots(&p, L"S3Key") && g_e2e_pwd_prompts == 1, "The secret key is cached");
    g_e2e_pwd_prompts = 0;
    EXPECT_TRUE(lists_snapshots(&p, L"S3Typed") && g_e2e_pwd_prompts == 2 &&
                    wcscmp(g_e2e_last_title, L"S3Typed - Restic repository password") == 0 && g_e2e_msg_dialogs == 0,
                "With a typed password too: the secret key first, then the password");
    static WCHAR root[1024];
    static const WCHAR forget_two[] = L"\U0001F5D1 Forget 2 Passwords|";
    EXPECT_TRUE(list_names(&p, L"\\", root, _countof(root)) && wcsncmp(root, forget_two, wcslen(forget_two)) == 0,
                "The forget entry counts the repositories with something typed in, not those using a file or command");

    WCHAR log_file[MAX_PATH];
    swprintf_s(log_file, MAX_PATH, L"%ls\\settings.log", dir);
    char* log_text = read_text_file(log_file);
    BOOL logged = log_text && strstr(log_text, "RESTIC_PASSWORD_FILE: ") && strstr(log_text, "RESTIC_PASSWORD_COMMAND: ") &&
                  strstr(log_text, "AWS_ACCESS_KEY_ID=AKIATESTKEY") && strstr(log_text, "AWS_SECRET_ACCESS_KEY: ") &&
                  strstr(log_text, " -o \"local.connections=1\" ") && strstr(log_text, "S3 secret key prompt for 'S3Key': ");
    BOOL leaked = log_text && strstr(log_text, fixture_pwd_u8) != NULL;
    free(log_text);
    EXPECT_TRUE(logged, "The log shows where restic gets the password and the S3 settings");
    EXPECT_TRUE(!leaked, "Neither the password nor the secret key appears in the log");

    unload_plugin_copy(&p, dir);
    delete_tree(dir);
}

/* After a failed check of a live scenario: the last message shown and the end of the plugin's log */
static void print_failure_context(const WCHAR* log_file) {
    printf("       prompts %d, dialogs %d, last message: %ls\n", g_e2e_pwd_prompts, g_e2e_msg_dialogs, g_e2e_last_msg);
    char* text = read_text_file(log_file);
    if (!text) return;
    size_t len = strlen(text);
    printf("       --- end of %ls ---\n%s\n       ---\n", log_file, text + (len > 6000 ? len - 6000 : 0));
    free(text);
}

/* Runs `cmd` hidden with the test's environment; TRUE when it exits with 0 within `ms` */
static BOOL run_and_wait(WCHAR* cmd, DWORD ms) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    DWORD code = 1;
    BOOL done  = WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code);
    if (!done) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return done && code == 0;
}

/* Number of running processes whose executable is named `exe_name` */
static int count_processes(const WCHAR* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    int n = 0;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe_name) == 0) n++;
    }
    CloseHandle(snap);
    return n;
}

/* Waits up to `ms` for the number of `exe_name` processes to drop to `count`; TRUE when it did */
static BOOL processes_back_to(const WCHAR* exe_name, int count, DWORD ms) {
    for (DWORD waited = 0;; waited += 250) {
        int n = count_processes(exe_name);
        if (n >= 0 && n <= count) return TRUE;
        if (waited >= ms) return FALSE;
        Sleep(250);
    }
}

/*
 * Scenario F: an rclone: repository (rclone's on-the-fly local backend over the fixture repository,
 * tmp\rclone.exe named by rclone.program as a plain path) lists, opens a snapshot, copies a file,
 * reports a wrong password as wrong (exit code 12 through rclone) and can be cancelled; no rclone
 * process outlives the restic that started it.
 */
static void test_live_rclone_repository(void) {
    if (GetFileAttributesW(L"tmp\\restic.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\rclone.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\test_fixture\\repo\\config") == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe, tmp\\rclone.exe or tmp\\test_fixture\\repo not found: rclone E2E test skipped.\n");
        return;
    }
    WCHAR restic_abs[MAX_PATH] = {0}, rclone_abs[MAX_PATH] = {0}, repo_abs[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_abs, NULL);
    GetFullPathNameW(L"tmp\\rclone.exe", MAX_PATH, rclone_abs, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\repo", MAX_PATH, repo_abs, NULL);
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_rclone", dir, MAX_PATH), "Create plugin directory (rclone)");
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);

    char restic_u8[MAX_PATH * 3] = {0}, rclone_u8[MAX_PATH * 3] = {0}, repo_u8[MAX_PATH * 3] = {0},
         dir_u8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, restic_abs, -1, restic_u8, sizeof(restic_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, rclone_abs, -1, rclone_u8, sizeof(rclone_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, repo_abs, -1, repo_u8, sizeof(repo_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dir_u8, sizeof(dir_u8), NULL, NULL);
    static char ini_text[8192]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    int n = snprintf(ini_text, sizeof(ini_text),
                     "[Settings]\r\nResticPath=%s\r\n[Logging]\r\nLogPath=%s\\rclone.log\r\n"
                     "[Repositories]\r\nRclone=rclone::local:%s\r\nRcloneWrong=rclone::local:%s\r\n"
                     "RcloneCancel=rclone::local:%s\r\n"
                     "[Repository: Rclone]\r\nrclone.program=%s\r\n"
                     "[Repository: RcloneWrong]\r\nrclone.program=%s\r\n"
                     "[Repository: RcloneCancel]\r\nrclone.program=\"%s\"\r\nrclone.args=serve restic --stdio\r\n",
                     restic_u8, dir_u8, repo_u8, repo_u8, repo_u8, rclone_u8, rclone_u8, rclone_u8);
    EXPECT_TRUE(n > 0 && (size_t)n < sizeof(ini_text) && write_file(ini, ini_text), "Write INI (rclone)");

    int rclones_before = count_processes(L"rclone.exe");
    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy (rclone)");
    EXPECT_TRUE(p.init_w(46, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks (rclone)");

    g_e2e_pwd_mode    = 1;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    WCHAR log_file[MAX_PATH];
    swprintf_s(log_file, MAX_PATH, L"%ls\\rclone.log", dir);
    BOOL listed = lists_snapshots(&p, L"Rclone") && g_e2e_pwd_prompts == 1 && g_e2e_msg_dialogs == 0;
    if (!listed) print_failure_context(log_file);
    EXPECT_TRUE(listed, "The snapshots of an rclone: repository are listed");
    static WCHAR data_path[MAX_PATH * 2];
    EXPECT_TRUE(descend_to_fixture_data(&p, L"\\Rclone\\snapshots\\latest", 0, data_path, _countof(data_path)),
                "... and a snapshot is browsed down to the fixture's data folder");
    WCHAR remote[MAX_PATH * 3], local[MAX_PATH];
    swprintf_s(remote, _countof(remote), L"%ls\\readme.txt", data_path);
    swprintf_s(local, MAX_PATH, L"%ls\\readme.txt", dir);
    DeleteFileW(local); /* Left by an earlier run that stopped early */
    RemoteInfo ri = {5, 0, {0x802D2000, 0x01D5EF01}, FILE_ATTRIBUTE_READONLY};
    EXPECT_TRUE(p.get_file_w(remote, local, 0, &ri) == FS_FILE_OK && GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES &&
                    g_e2e_msg_dialogs == 0,
                "A file is copied out of it");

    g_e2e_pwd_mode    = 2; /* Always wrong */
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    EXPECT_TRUE(!lists_snapshots(&p, L"RcloneWrong") && g_e2e_pwd_prompts == 3 && g_e2e_msg_dialogs == 1,
                "A wrong password is recognised as one through rclone (asked three times)");

    g_e2e_pwd_mode    = 1;
    g_e2e_force_abort = TRUE;
    EXPECT_TRUE(!lists_snapshots(&p, L"RcloneCancel"), "ESC cancels a listing of an rclone: repository");
    g_e2e_force_abort = FALSE;
    EXPECT_TRUE(lists_snapshots(&p, L"RcloneCancel"),
                "rclone.program quoted, with rclone.args of its own, works too");

    unload_plugin_copy(&p, dir);
    EXPECT_TRUE(rclones_before >= 0 && processes_back_to(L"rclone.exe", rclones_before, 30000),
                "No rclone process outlives the restic that started it");
    delete_tree(dir);
}

/*
 * Scenario G: an S3 repository on a local S3 server (rclone serve s3, keys AKIATEST / goodsecret),
 * with AWS_ACCESS_KEY_ID in its section. A wrong secret key, which restic would retry for 15
 * minutes, is refused at once and asked for again, then the right one opens the repository and is
 * cached; three wrong ones end with the storage's reason in one dialog.
 */
static void test_s3_repository_with(const WCHAR* dir, const char* target) {
    WCHAR ini[MAX_PATH], log_file[MAX_PATH], restic_abs[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_abs, NULL);
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);
    swprintf_s(log_file, MAX_PATH, L"%ls\\s3.log", dir);
    char restic_u8[MAX_PATH * 3] = {0}, log_u8[MAX_PATH * 3] = {0};
    WideCharToMultiByte(CP_UTF8, 0, restic_abs, -1, restic_u8, sizeof(restic_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, log_file, -1, log_u8, sizeof(log_u8), NULL, NULL);
    static char ini_text[4096];
    int n = snprintf(ini_text, sizeof(ini_text),
                     "[Settings]\r\nResticPath=%s\r\n[Logging]\r\nLogPath=%s\r\n"
                     "[Repositories]\r\nS3=%s\r\nS3Wrong=%s\r\n"
                     "[Repository: S3]\r\nAWS_ACCESS_KEY_ID=AKIATEST\r\n"
                     "[Repository: S3Wrong]\r\nAWS_ACCESS_KEY_ID=AKIATEST\r\n",
                     restic_u8, log_u8, target, target);
    EXPECT_TRUE(n > 0 && (size_t)n < sizeof(ini_text) && write_file(ini, ini_text), "Write INI (S3)");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy (S3)");
    EXPECT_TRUE(p.init_w(47, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks (S3)");
    g_e2e_pwd_mode      = 1; /* The repository password is the fixture's */
    g_e2e_pwd_prompts   = 0;
    g_e2e_msg_dialogs   = 0;
    g_e2e_s3_prompts    = 0;
    g_e2e_s3_answers[0] = L"wrongsecret";
    g_e2e_s3_answers[1] = L"goodsecret";
    ULONGLONG t0 = GetTickCount64();
    BOOL listed  = lists_snapshots(&p, L"S3") && g_e2e_s3_prompts == 2 && g_e2e_pwd_prompts == 3 && g_e2e_msg_dialogs == 0;
    if (!listed) print_failure_context(log_file);
    EXPECT_TRUE(listed && GetTickCount64() - t0 < 120000,
                "A refused S3 secret key is asked for again at once; the right one opens the repository");
    EXPECT_TRUE(conn_log_count(L"[S3] S3 secret key refused by the storage") == 1, "... and the log says why");
    EXPECT_TRUE(lists_snapshots(&p, L"S3") && g_e2e_pwd_prompts == 3, "The accepted secret key is cached");

    g_e2e_pwd_prompts   = 0;
    g_e2e_msg_dialogs   = 0;
    g_e2e_s3_prompts    = 0;
    g_e2e_s3_answers[1] = L"wrongsecret";
    g_e2e_s3_answers[2] = L"wrongsecret";
    t0                  = GetTickCount64();
    BOOL refused = !lists_snapshots(&p, L"S3Wrong") && g_e2e_s3_prompts == 3 && g_e2e_pwd_prompts == 4 &&
                   g_e2e_msg_dialogs == 1 && wcsstr(g_e2e_last_msg, L"refused the credentials") != NULL;
    if (!refused) print_failure_context(log_file);
    EXPECT_TRUE(refused && GetTickCount64() - t0 < 120000,
                "Three refused secret keys end with the storage's reason, in minutes rather than a quarter of an hour");
    unload_plugin_copy(&p, dir);
}

static void test_live_s3_repository(void) {
    if (GetFileAttributesW(L"tmp\\restic.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\rclone.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\test_fixture\\data") == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe, tmp\\rclone.exe or tmp\\test_fixture\\data not found: S3 E2E test skipped.\n");
        return;
    }
    WCHAR dir[MAX_PATH], data[MAX_PATH], bucket[MAX_PATH], serve_log[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_s3", dir, MAX_PATH), "Create plugin directory (S3)");
    delete_tree(dir); /* Whatever a run with the same process ID left */
    EXPECT_TRUE(CreateDirectoryW(dir, NULL), "Create a clean plugin directory (S3)");
    swprintf_s(data, MAX_PATH, L"%ls\\s3data", dir);
    swprintf_s(bucket, MAX_PATH, L"%ls\\bucket", data);
    swprintf_s(serve_log, MAX_PATH, L"%ls\\serve.log", dir);
    EXPECT_TRUE(CreateDirectoryW(data, NULL) && CreateDirectoryW(bucket, NULL), "Create the S3 server's bucket");

    /* rclone serve s3 on a port of its own, its output in serve.log */
    unsigned port = 39000u + (unsigned)(GetCurrentProcessId() % 900u);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE out = CreateFileW(serve_log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, 0, NULL);
    WCHAR rclone_abs[MAX_PATH] = {0};
    GetFullPathNameW(L"tmp\\rclone.exe", MAX_PATH, rclone_abs, NULL);
    static WCHAR cmd[4 * MAX_PATH];
    swprintf_s(cmd, _countof(cmd), L"\"%ls\" serve s3 --auth-key AKIATEST,goodsecret --addr 127.0.0.1:%u \"%ls\"",
               rclone_abs, port, data);
    STARTUPINFOW si;
    PROCESS_INFORMATION server;
    memset(&si, 0, sizeof(si));
    memset(&server, 0, sizeof(server));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError  = out;
    BOOL started  = out != INVALID_HANDLE_VALUE &&
                   CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &server);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    EXPECT_TRUE(started, "Start rclone serve s3");
    BOOL ready = FALSE;
    for (int i = 0; i < 120 && !ready; ++i) {
        Sleep(250);
        char* text = read_text_file(serve_log);
        ready      = text && strstr(text, "Starting s3 server") != NULL;
        free(text);
    }

    /* A repository there with one snapshot, made by restic itself */
    static char target[128];
    snprintf(target, sizeof(target), "s3:http://127.0.0.1:%u/bucket/repo", port);
    WCHAR target_w[128], restic_abs[MAX_PATH] = {0}, data_abs[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, target, -1, target_w, (int)_countof(target_w));
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_abs, NULL);
    GetFullPathNameW(L"tmp\\test_fixture\\data", MAX_PATH, data_abs, NULL);
    SetEnvironmentVariableW(L"AWS_ACCESS_KEY_ID", L"AKIATEST");
    SetEnvironmentVariableW(L"AWS_SECRET_ACCESS_KEY", L"goodsecret");
    SetEnvironmentVariableW(L"RESTIC_PASSWORD", FIXTURE_PASSWORD);
    SetEnvironmentVariableW(L"RESTIC_REPOSITORY", target_w);
    swprintf_s(cmd, _countof(cmd), L"\"%ls\" init", restic_abs);
    BOOL made = ready && run_and_wait(cmd, 120000);
    swprintf_s(cmd, _countof(cmd), L"\"%ls\" backup \"%ls\"", restic_abs, data_abs);
    made = made && run_and_wait(cmd, 300000);
    static const WCHAR* const vars[] = {L"AWS_ACCESS_KEY_ID", L"AWS_SECRET_ACCESS_KEY", L"RESTIC_PASSWORD", L"RESTIC_REPOSITORY"};
    for (size_t i = 0; i < _countof(vars); ++i) SetEnvironmentVariableW(vars[i], NULL);

    if (made) test_s3_repository_with(dir, target);
    if (started) {
        TerminateProcess(server.hProcess, 0);
        WaitForSingleObject(server.hProcess, 10000);
        CloseHandle(server.hThread);
        CloseHandle(server.hProcess);
    }
    memset((void*)g_e2e_s3_answers, 0, sizeof(g_e2e_s3_answers));
    EXPECT_TRUE(made, "rclone serve s3 started and restic made a repository there");
    delete_tree(dir);
}

/*
 * Scenario D: paths with spaces, written the ways a user would write them in TCResticBrowser.ini -
 * ResticPath and repository paths with spaces, unquoted and quoted, with and without a trailing
 * backslash. restic.exe and the repository live in folders with spaces, like "C:\Program Files".
 */
static void test_live_paths_with_spaces(void) {
    if (GetFileAttributesW(L"tmp\\restic.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"tmp\\test_fixture\\repo\\config") == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe or tmp\\test_fixture\\repo not found: spaces E2E test skipped.\n");
        return;
    }

    WCHAR dir[MAX_PATH], exe_dir[MAX_PATH], exe[MAX_PATH], repo[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic dll spaces", dir, MAX_PATH), "Create plugin directory with spaces");
    swprintf_s(exe_dir, MAX_PATH, L"%ls\\Program Files\\Restic", dir);
    swprintf_s(exe, MAX_PATH, L"%ls\\restic.exe", exe_dir);
    swprintf_s(repo, MAX_PATH, L"%ls\\My Backups\\Firefox Thunderbird.restic-repo", dir);
    swprintf_s(ini, MAX_PATH, L"%ls\\TCResticBrowser.ini", dir);

    WCHAR program_files[MAX_PATH], backups[MAX_PATH];
    swprintf_s(program_files, MAX_PATH, L"%ls\\Program Files", dir);
    swprintf_s(backups, MAX_PATH, L"%ls\\My Backups", dir);
    EXPECT_TRUE((CreateDirectoryW(program_files, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) &&
                (CreateDirectoryW(exe_dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) &&
                CopyFileW(L"tmp\\restic.exe", exe, FALSE),
                "Copy restic.exe into a folder with spaces");
    EXPECT_TRUE((CreateDirectoryW(backups, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) &&
                copy_tree(L"tmp\\test_fixture\\repo", repo),
                "Copy the fixture repository into a folder with spaces");

    char exe_u8[MAX_PATH * 3] = {0}, repo_u8[MAX_PATH * 3] = {0}, dir_u8[MAX_PATH * 3] = {0};
    static char ini_text[4096]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe_u8, sizeof(exe_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, dir, -1, dir_u8, sizeof(dir_u8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, repo, -1, repo_u8, sizeof(repo_u8), NULL, NULL);
    int ini_len = snprintf(
        ini_text,
        sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\nPasswordTimeoutSeconds=5\r\nConnectionLog=off\r\n"
        "[Logging]\r\nLogPath=%s\\spaces.log\r\n"
        "[Repositories]\r\n"
        "Plain=%s\r\nTrailing Backslash=%s\\\r\nQuoted=\"%s\"\r\nQuoted Trailing=\"%s\\\"\r\nTimed=%s\r\n",
        exe_u8, dir_u8, repo_u8, repo_u8, repo_u8, repo_u8, repo_u8
    );
    EXPECT_TRUE(ini_len > 0 && (size_t)ini_len < sizeof(ini_text), "Spaces INI fits its buffer");
    EXPECT_TRUE(write_file(ini, ini_text), "Write spaces INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy from a folder with spaces");
    int connects_before = conn_log_count(L"1:CONNECT"); /* ConnectionLog=off: none here */
    EXPECT_TRUE(p.init_w(43, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks (spaces)");

    const WCHAR* aliases[] = {L"Plain", L"Trailing Backslash", L"Quoted", L"Quoted Trailing"};
    for (size_t i = 0; i < _countof(aliases); ++i) {
        g_e2e_pwd_mode    = 1;
        g_e2e_pwd_prompts = 0;
        g_e2e_msg_dialogs = 0;
        WCHAR path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"\\%ls\\snapshots", aliases[i]);
        WIN32_FIND_DATAW fd;
        HANDLE h = p.find_first_w(path, &fd);
        size_t snaps = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do snaps++; while (p.find_next_w(h, &fd));
            p.find_close(h);
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "Snapshots listed through alias #%u (path with spaces), one prompt, no error",
                 (unsigned)i);
        EXPECT_TRUE(snaps == 5 && g_e2e_pwd_prompts == 1 && g_e2e_msg_dialogs == 0, msg); /* Two, latest, aliases */
    }

    /* PasswordTimeoutSeconds=5: reused within the timeout (counted from when restic first accepted
       it, not from the last use), wiped by the timer afterwards even without any plugin call */
    g_e2e_pwd_prompts = 0;
    WIN32_FIND_DATAW fd2;
    HANDLE h2 = p.find_first_w(L"\\Timed", &fd2);
    if (h2 != INVALID_HANDLE_VALUE) p.find_close(h2);
    h2 = p.find_first_w(L"\\Timed", &fd2);
    if (h2 != INVALID_HANDLE_VALUE) p.find_close(h2);
    EXPECT_TRUE(h2 != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "Cached password reused within the timeout");
    Sleep(6500);
    WCHAR log_file[MAX_PATH];
    swprintf_s(log_file, MAX_PATH, L"%ls\\spaces.log", dir);
    char* log_text = read_text_file(log_file);
    BOOL expired = log_text && strstr(log_text, "Cached password for 'Timed' forgotten: PasswordTimeoutSeconds (5 s) elapsed");
    free(log_text);
    EXPECT_TRUE(expired, "The timer wiped the password after the timeout without a plugin call");
    h2 = p.find_first_w(L"\\Timed", &fd2);
    if (h2 != INVALID_HANDLE_VALUE) p.find_close(h2);
    EXPECT_TRUE(h2 != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 2, "Expired password is asked for again");

    /* PasswordTimeoutResetOnUse=yes: each completed use starts the interval again, so a repository
       in active use outlives the timeout; once it is left alone the password expires */
    int reset_len = snprintf(
        ini_text, sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\nPasswordTimeoutSeconds=4\r\nPasswordTimeoutResetOnUse=yes\r\nConnectionLog=off\r\n"
        "[Logging]\r\nLogPath=%s\\spaces.log\r\n[Repositories]\r\nSliding=%s\r\n",
        exe_u8, dir_u8, repo_u8);
    EXPECT_TRUE(reset_len > 0 && (size_t)reset_len < sizeof(ini_text) && write_file(ini, ini_text),
                "Rewrite the INI with PasswordTimeoutResetOnUse=yes");
    g_e2e_pwd_prompts = 0;
    for (int i = 0; i < 4; ++i) { /* 4 uses 2 s apart: well past the 4 s timeout in total */
        h2 = p.find_first_w(L"\\Sliding", &fd2);
        if (h2 != INVALID_HANDLE_VALUE) p.find_close(h2);
        if (i < 3) Sleep(2000);
    }
    EXPECT_TRUE(h2 != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 1, "Password kept while in use (reset on use)");
    Sleep(5500);
    h2 = p.find_first_w(L"\\Sliding", &fd2);
    if (h2 != INVALID_HANDLE_VALUE) p.find_close(h2);
    EXPECT_TRUE(h2 != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 2, "Password expires once it is no longer used");

    /* SnapshotListRefreshSeconds: an outdated snapshot list is still shown at once, and re-read in
       the background for the next listing */
    int fresh_len = snprintf(
        ini_text, sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\nSnapshotListRefreshSeconds=1\r\nConnectionLog=off\r\n"
        "[Logging]\r\nLogPath=%s\\spaces.log\r\n[Repositories]\r\nFresh=%s\r\n",
        exe_u8, dir_u8, repo_u8);
    EXPECT_TRUE(fresh_len > 0 && (size_t)fresh_len < sizeof(ini_text) && write_file(ini, ini_text),
                "Rewrite the INI with SnapshotListRefreshSeconds=1");
    g_e2e_pwd_prompts = 0;
    size_t before = 0, stale = 0, after = 0;
    count_entries(&p, L"\\Fresh\\snapshots", &before);
    EXPECT_TRUE(before == 5 && g_e2e_pwd_prompts == 1, "Two snapshots, latest and its two aliases");
    EXPECT_TRUE(run_restic_backup(exe, repo), "Another snapshot taken by restic itself");
    Sleep(1500);
    count_entries(&p, L"\\Fresh\\snapshots", &stale);
    EXPECT_TRUE(stale == 5, "The outdated list is shown at once");
    BOOL reread = FALSE;
    for (int i = 0; i < 300 && !reread; ++i) {
        char* text = read_text_file(log_file);
        reread = text && strstr(text, "Snapshots of 'Fresh' re-read in the background");
        free(text);
        if (!reread) Sleep(200);
    }
    EXPECT_TRUE(reread, "Meanwhile restic re-read it in the background");
    count_entries(&p, L"\\Fresh\\snapshots", &after);
    EXPECT_TRUE(after == 6 && g_e2e_pwd_prompts == 1, "The next listing shows the new snapshot, without a prompt");
    EXPECT_TRUE(conn_log_count(L"1:CONNECT") == connects_before && conn_log_count(L"[Fresh]") == 0,
                "With ConnectionLog=off no connection is announced and nothing goes to its log");

    unload_plugin_copy(&p, dir);
    DeleteFileW(ini);
    delete_tree(dir);
}

/* Fills `out` with `name` in the directory of this executable */
static BOOL path_next_to_exe(const WCHAR* name, WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;
    WCHAR* slash = wcsrchr(exe, L'\\');
    if (!slash) return FALSE;
    *slash = L'\0';
    return swprintf_s(out, cap, L"%ls\\%ls", exe, name) > 0;
}

/*
 * A test that hangs (a deadlock in the plugin, a restic that never ends) must not keep a CI run
 * waiting for hours without a word: when no check has finished for WATCHDOG_MINUTES, say which one
 * finished last and what the connection log holds, and end the process. TerminateProcess, not
 * exit(): a deadlock may hold the loader lock, which exit() would need for DLL_PROCESS_DETACH.
 */
#ifndef WATCHDOG_MINUTES
#define WATCHDOG_MINUTES 10u
#endif
static DWORD WINAPI watchdog_thread(void* arg) {
    (void)arg;
    LONG seen       = -1;
    ULONGLONG since = GetTickCount64();
    for (;;) {
        Sleep(5000);
        LONG done = InterlockedCompareExchange(&g_checks_done, 0, 0);
        if (done != seen) {
            seen  = done;
            since = GetTickCount64();
            continue;
        }
        if (GetTickCount64() - since < WATCHDOG_MINUTES * 60000ull) continue;
        printf("[FAIL] Watchdog: no check finished for %u minutes; the last one was %s:%ld: %s\n", WATCHDOG_MINUTES,
               g_last_check_function, (long)g_last_check_line, g_last_check);
        if (TryAcquireSRWLockShared(&g_e2e_conn_log_lock)) {
            /* As UTF-8: printf("%ls") would stop at the first character the C locale cannot show (→) */
            static char tail[3000 * 3 + 1];
            size_t len = wcslen(g_e2e_conn_log);
            int n = WideCharToMultiByte(CP_UTF8, 0, g_e2e_conn_log + (len > 3000 ? len - 3000 : 0), -1, tail,
                                        (int)sizeof(tail), NULL, NULL);
            ReleaseSRWLockShared(&g_e2e_conn_log_lock);
            printf("       End of the connection log:\n");
            if (n > 1) fwrite(tail, 1, (size_t)n - 1, stdout);
            printf("\n");
        }
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
}

int main(void) {
    /* No modal system dialog (crash, missing disk, failed start) may stop an unattended CI run */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    /* Nor may a Debug CRT report: it goes to stderr (the plugin's own CRT does the same, see DllMain) */
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    HANDLE watchdog = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    if (watchdog) CloseHandle(watchdog);
    printf("Running Restic WFX plugin DLL smoke tests...\n");
    if (!path_next_to_exe(L"" RESTIC_WFX_FILE_NAME, g_plugin_dll, MAX_PATH) ||
        !path_next_to_exe(L"pluginst.inf", g_pluginst_inf, MAX_PATH)) {
        printf("[FAIL] Cannot determine the directory of this executable.\n");
        return 1;
    }
    if (GetFileAttributesW(g_plugin_dll) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] %ls not found: DLL smoke tests skipped.\n", g_plugin_dll);
        return 0;
    }

    /* Never pick up the developer's own %APPDATA%\GHISLER\TCResticBrowser.ini */
    WCHAR appdata_dir[MAX_PATH];
    if (!make_unique_dir(L"restic_dll_appdata", appdata_dir, MAX_PATH) ||
        swprintf_s(g_user_dir, MAX_PATH, L"%ls\\GHISLER", appdata_dir) <= 0 ||
        !(CreateDirectoryW(g_user_dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) ||
        !SetEnvironmentVariableW(L"APPDATA", appdata_dir)) {
        printf("[FAIL] Cannot set up a temporary %%APPDATA%%.\n");
        return 1;
    }

    test_export_surface();
    test_primary_ini_next_to_dll();
    test_user_ini_overrides_plugin_ini();
    test_live_end_to_end_navigation();
    test_live_paths_with_spaces();
    test_live_repository_settings();
    test_old_restic_refused();
    test_live_rclone_repository();
    test_live_s3_repository();

    RemoveDirectoryW(g_user_dir);
    RemoveDirectoryW(appdata_dir);

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
