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
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "version.h"

/* Full paths of the built plugin and the staged pluginst.inf next to this executable */
static WCHAR g_plugin_dll[MAX_PATH];
static WCHAR g_pluginst_inf[MAX_PATH];

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
    if (wcscmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%ls', got '%ls')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
        g_tests_failed++; \
        return; \
    } \
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
                              "FsGetFile", "FsGetFileW"};
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
                               "FsExecuteFile", "json_parse", "json_free", "DllMain"};
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
    EXPECT_TRUE(wcscmp(infType, L"wfx") == 0 && wcscmp(infFile, L"TCResticBrowser.wfx") == 0 &&
                wcscmp(infVer, L"" RESTIC_WFX_PRODUCT_VER) == 0,
                "Staged pluginst.inf has valid [plugininstall] type, file, and version");

    FreeLibrary(m);
}

/* Scenario A: restic_plugin.ini sits next to the DLL (no per-user INI) */
static void test_primary_ini_next_to_dll(void) {
    WCHAR dir[MAX_PATH], ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_primary", dir, MAX_PATH), "Create plugin directory");
    swprintf_s(ini, MAX_PATH, L"%ls\\restic_plugin.ini", dir);
    EXPECT_TRUE(write_file(ini, "[Repositories]\r\nAlpha=D:\\r1\r\nBeta=D:\\r2\r\n"), "Write INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy");
    EXPECT_TRUE(p.init_w && p.get_root_name && p.set_default_params && p.find_first_w && p.find_next_w &&
                p.find_close && p.find_first, "Entry points resolved");

    char root_name[64] = {0};
    p.get_root_name(root_name, (int)sizeof(root_name));
    EXPECT_TRUE(strcmp(root_name, "Restic Backups") == 0, "Default root name");
    char tiny[5] = {'x', 'x', 'x', 'x', 'x'};
    p.get_root_name(tiny, (int)sizeof(tiny));
    EXPECT_TRUE(strcmp(tiny, "Rest") == 0, "Root name truncated and terminated for small buffers");

    EXPECT_TRUE(p.init_w(1, NULL, NULL, NULL) == 0, "FsInitW returns 0");

    WIN32_FIND_DATAW fd;
    HANDLE h = p.find_first_w(L"\\", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Root enumeration starts");
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
 * Scenario B: the per-user %APPDATA%\GHISLER\restic_plugin.ini wins over the INI next to the DLL;
 * DefaultIniName from FsSetDefaultParams is ignored and no INI is ever auto-created.
 */
static void test_user_ini_overrides_plugin_ini(void) {
    WCHAR dir[MAX_PATH], plug_ini[MAX_PATH], user_ini[MAX_PATH], tc_dir[MAX_PATH], tc_ini[MAX_PATH];
    EXPECT_TRUE(make_unique_dir(L"restic_dll_userini", dir, MAX_PATH) &&
                make_unique_dir(L"restic_dll_tcconfig", tc_dir, MAX_PATH), "Create directories");
    swprintf_s(plug_ini, MAX_PATH, L"%ls\\restic_plugin.ini", dir);
    swprintf_s(user_ini, MAX_PATH, L"%ls\\restic_plugin.ini", g_user_dir);
    swprintf_s(tc_ini, MAX_PATH, L"%ls\\restic_plugin.ini", tc_dir);
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
                                           3 = user submits an empty password, 4 = user cancels the dialog */
static WCHAR g_e2e_last_prompt[256] = {0};
static WCHAR g_e2e_last_msg[256]    = {0};
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

static void __stdcall e2e_log_proc(int PluginNr, int MsgType, WCHAR* LogString) {
    (void)PluginNr;
    if (MsgType == MSGTYPE_IMPORTANTERROR && LogString) {
        g_e2e_error_logs++;
    }
}

static BOOL __stdcall e2e_request_proc(
    int PluginNr,
    int RequestType,
    WCHAR* CustomTitle,
    WCHAR* CustomText,
    WCHAR* ReturnedText,
    int maxlen
) {
    (void)PluginNr;
    (void)CustomTitle;
    if (RequestType == RT_Password) {
        g_e2e_pwd_prompts++;
        if (CustomText) {
            wcsncpy_s(g_e2e_last_prompt, _countof(g_e2e_last_prompt), CustomText, _TRUNCATE);
        }
        if (g_e2e_pwd_mode == 3) {
            ReturnedText[0] = L'\0';
            return TRUE;
        }
        if (g_e2e_pwd_mode == 4) {
            return FALSE;
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
        if (entries != 2) InterlockedIncrement(&g_e2e_mt_failures); /* Fixture has two snapshots */
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
    swprintf_s(ini, MAX_PATH, L"%ls\\restic_plugin.ini", dir);

    char restic_a[MAX_PATH] = {0}, repo_a[MAX_PATH] = {0}, ini_text[6144] = {0};
    WideCharToMultiByte(CP_ACP, 0, restic_abs, -1, restic_a, sizeof(restic_a), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, repo_abs, -1, repo_a, sizeof(repo_a), NULL, NULL);
    char long_alias[256];
    memset(long_alias, 'L', 250);
    long_alias[250] = '\0';
    int ini_len = snprintf(
        ini_text,
        sizeof(ini_text),
        "[Settings]\r\nResticPath=%s\r\n[Repositories]\r\nMainRepo=%s\r\nSecondRepo=%s\r\nThirdRepo=%s\r\n"
        "ExtractRepo=%s\r\n"
        "MissingRepo=C:\\no_such_restic_repo_999\r\n%s=C:\\no_such_restic_repo_999\r\n",
        restic_a,
        repo_a,
        repo_a,
        repo_a,
        repo_a,
        long_alias
    );
    for (int i = 1; i <= 10; ++i) { /* More aliases than session slots (8) */
        ini_len += snprintf(ini_text + ini_len, sizeof(ini_text) - (size_t)ini_len, "Slot%02d=%s\r\n", i, repo_a);
    }
    EXPECT_TRUE(ini_len > 0 && (size_t)ini_len < sizeof(ini_text), "E2E INI fits its buffer");
    EXPECT_TRUE(write_file(ini, ini_text), "Write E2E INI");

    Plugin p;
    EXPECT_TRUE(load_plugin_copy(dir, &p), "Load plugin copy for E2E");
    EXPECT_TRUE(p.init_w(42, (void*)e2e_progress_proc, (void*)e2e_log_proc, (void*)e2e_request_proc) == 0,
                "Register Total Commander callbacks");

    /* 1. Level 1 (\MainRepo): wrong password on first prompt -> automatic retry -> 2 snapshots listed */
    g_e2e_pwd_mode    = 0;
    g_e2e_pwd_prompts = 0;
    g_e2e_msg_dialogs = 0;
    g_e2e_error_logs  = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = p.find_first_w(L"\\MainRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE, "Level 1 snapshot enumeration succeeds after password retry");
    EXPECT_TRUE(g_e2e_pwd_prompts == 2, "Prompted twice (initial + 1 retry after wrong password)");
    EXPECT_TRUE(wcsstr(g_e2e_last_prompt, L"Wrong password") != NULL, "Retry prompt tells the user the password was wrong");
    EXPECT_TRUE(g_e2e_msg_dialogs == 0, "No error dialog when retry succeeds");

    WCHAR latest_snap[MAX_PATH] = {0};
    size_t snap_count = 0;
    do {
        snap_count++;
        wcsncpy_s(latest_snap, MAX_PATH, fd.cFileName, _TRUNCATE);
    } while (p.find_next_w(h, &fd));
    p.find_close(h);
    EXPECT_TRUE(snap_count == 2, "Both fixture snapshots returned at Level 1");

    /* 2. Level 2+ (\MainRepo\<snap>\...): session password cached in RAM (0 additional prompts) */
    g_e2e_pwd_prompts = 0;
    WCHAR snap_root[MAX_PATH * 2], data_path[MAX_PATH * 2];
    swprintf_s(snap_root, _countof(snap_root), L"\\MainRepo\\%ls", latest_snap);
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
    WCHAR empty_path[MAX_PATH * 2];
    swprintf_s(empty_path, _countof(empty_path), L"%ls\\empty_dir", data_path);
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(empty_path, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_NO_MORE_FILES, "Empty snapshot directory returns ERROR_NO_MORE_FILES");

    /* 2b. File extraction via FsGetFileW + recursive directory copy (as Total Commander does on F5) */
    test_e2e_file_and_folder_extraction(&p, dir, data_path);
    test_e2e_extraction_edge_cases(&p, dir, data_path);

    /* Non-existent directory inside snapshot -> ERROR_PATH_NOT_FOUND, no error popup */
    WCHAR missing_sub[MAX_PATH * 2];
    swprintf_s(missing_sub, _countof(missing_sub), L"%ls\\does_not_exist", data_path);
    SetLastError(0);
    EXPECT_TRUE(p.find_first_w(missing_sub, &fd) == INVALID_HANDLE_VALUE &&
                GetLastError() == ERROR_PATH_NOT_FOUND, "Missing snapshot path returns ERROR_PATH_NOT_FOUND");
    EXPECT_TRUE(g_e2e_pwd_prompts == 0 && g_e2e_msg_dialogs == 0,
                "All navigations within MainRepo reused the cached RAM password with no popups");

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
    /* ESC does not discard a valid password */
    g_e2e_pwd_prompts = 0;
    h = p.find_first_w(L"\\SecondRepo", &fd);
    EXPECT_TRUE(h != INVALID_HANDLE_VALUE && g_e2e_pwd_prompts == 0, "Password survives a user abort");
    p.find_close(h);

    /* 7. Concurrent listings from several threads (TC may call the plugin from worker threads) */
    {
        enum { THREADS = 6 };
        HANDLE threads[THREADS];
        MtJob jobs[THREADS];
        g_e2e_mt_failures = 0;
        for (int i = 0; i < THREADS; ++i) {
            jobs[i].plugin = &p;
            jobs[i].path   = (i % 2) ? L"\\MainRepo" : L"\\SecondRepo";
            threads[i]     = CreateThread(NULL, 0, mt_list_worker, &jobs[i], 0, NULL);
        }
        WaitForMultipleObjects(THREADS, threads, TRUE, 120000);
        for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
        EXPECT_TRUE(g_e2e_mt_failures == 0, "Concurrent listings all succeed (cached sessions, thread-safe)");
    }

    unload_plugin_copy(&p, dir);
    DeleteFileW(ini);
    RemoveDirectoryW(dir);
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

int main(void) {
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

    /* Never pick up the developer's own %APPDATA%\GHISLER\restic_plugin.ini */
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

    RemoveDirectoryW(g_user_dir);
    RemoveDirectoryW(appdata_dir);

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
