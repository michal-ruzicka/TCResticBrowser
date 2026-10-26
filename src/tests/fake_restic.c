/*
 * Stand-in for restic.exe in test_restic_cli's dump tests (test_dump_with_fake_restic). The last
 * argument, the path restic_dump_file asks for, picks the behaviour, so every restic outcome the
 * plugin distinguishes can be produced without a repository. It is a real Windows program on
 * purpose: restic_cli.c refuses to start anything without a PE header, batch files included.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

static void put_out(const char* text) {
    fwrite(text, 1, strlen(text), stdout);
    fflush(stdout);
}

static void put_err(const char* text) {
    fputs(text, stderr);
    fflush(stderr);
}

/*
 * With FAKE_RESTIC_UNC set: a repository of one Windows snapshot that backed up network shares, as
 * restic names them (test_unc_shares in test_plugin_dll.c). restic under Wine cannot make one: it
 * opens a share as "\\?\UNC\...", which Wine does not resolve. The snapshot holds "C:\Users",
 * "\\srv\share\dir", "\\nas\backup", volumes no folder may be made of ("\\..\..\evil",
 * "\\srv\..\up") and a device path ("\\?\UNC\dev\x", whose volume is "\\?\UNC"); `ls` prints the whole snapshot whatever path it is given
 * (the plugin keeps what is below the path, and finds nothing for a path restic would not know),
 * `dump` knows the two files on the shares, and `find` answers only the patterns that keep restic
 * from walking every snapshot whole (file_history_pattern), the one of a.txt also with a file of
 * the same name below two other folders, as restic would.
 */
#define UNC_SNAPSHOT_ID "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define UNC_NODE(name, type, path, extra)                                                                    \
    "{\"name\":\"" name "\",\"type\":\"" type "\",\"path\":\"" path "\"" extra                                 \
    ",\"mtime\":\"2026-10-20T09:00:00+02:00\",\"message_type\":\"node\",\"struct_type\":\"node\"}\n"
#define UNC_MATCH(path, size) \
    "{\"path\":\"" path "\",\"type\":\"file\",\"size\":" size ",\"mtime\":\"2026-10-20T09:00:00+02:00\"}"

static int fake_unc_repository(int argc, char** argv) {
    const char* cmd = argc > 1 ? argv[1] : "";
    if (strcmp(cmd, "snapshots") == 0) {
        put_out("[{\"time\":\"2026-10-20T10:00:00+02:00\",\"tree\":\"00\",\"paths\":[\"C:\\\\Users\","
                "\"\\\\\\\\srv\\\\share\\\\dir\",\"\\\\\\\\nas\\\\backup\",\"\\\\\\\\?\\\\UNC\\\\dev\\\\x\","
                "\"\\\\\\\\..\\\\..\\\\evil\",\"\\\\\\\\srv\\\\..\\\\up\"],"
                "\"hostname\":\"pc\",\"username\":\"me\",\"id\":\"" UNC_SNAPSHOT_ID "\",\"short_id\":\"01234567\"}]\n");
        return 0;
    }
    if (strcmp(cmd, "ls") == 0) {
        put_out("{\"time\":\"2026-10-20T10:00:00+02:00\",\"tree\":\"00\",\"paths\":[],\"hostname\":\"pc\",\"id\":\""
                UNC_SNAPSHOT_ID "\",\"short_id\":\"01234567\",\"message_type\":\"snapshot\","
                "\"struct_type\":\"snapshot\"}\n");
        put_out(UNC_NODE("C", "dir", "/C", ""));
        put_out(UNC_NODE("Users", "dir", "/C/Users", ""));
        put_out(UNC_NODE("\\\\\\\\srv\\\\share", "dir", "/\\\\\\\\srv\\\\share", ""));
        put_out(UNC_NODE("dir", "dir", "/\\\\\\\\srv\\\\share/dir", ""));
        put_out(UNC_NODE("a.txt", "file", "/\\\\\\\\srv\\\\share/dir/a.txt", ",\"size\":5"));
        put_out(UNC_NODE("\\\\\\\\nas\\\\backup", "dir", "/\\\\\\\\nas\\\\backup", ""));
        put_out(UNC_NODE("b.txt", "file", "/\\\\\\\\nas\\\\backup/b.txt", ",\"size\":3"));
        put_out(UNC_NODE("\\\\\\\\?\\\\UNC", "dir", "/\\\\\\\\?\\\\UNC", ""));
        put_out(UNC_NODE("dev", "dir", "/\\\\\\\\?\\\\UNC/dev", ""));
        put_out(UNC_NODE("x", "dir", "/\\\\\\\\?\\\\UNC/dev/x", ""));
        put_out(UNC_NODE("c.txt", "file", "/\\\\\\\\?\\\\UNC/dev/x/c.txt", ",\"size\":3"));
        put_out(UNC_NODE("\\\\\\\\..\\\\..", "dir", "/\\\\\\\\..\\\\..", ""));
        put_out(UNC_NODE("evil", "file", "/\\\\\\\\..\\\\../evil", ",\"size\":4"));
        put_out(UNC_NODE("\\\\\\\\srv\\\\..", "dir", "/\\\\\\\\srv\\\\..", ""));
        put_out(UNC_NODE("up", "file", "/\\\\\\\\srv\\\\../up", ",\"size\":2"));
        return 0;
    }
    const char* path = argv[argc - 1];
    if (strcmp(cmd, "find") == 0) {
        if (strcmp(path, "/*/*/srv\\share/dir/a.txt") == 0) {
            put_out("[{\"matches\":[" UNC_MATCH("/\\\\\\\\srv\\\\share/dir/a.txt", "5") ","
                    UNC_MATCH("/C/Users/srv/share/dir/a.txt", "9") "],\"hits\":2,\"snapshot\":\"" UNC_SNAPSHOT_ID "\"}]\n");
        } else if (strcmp(path, "/*/*/nas\\backup/b.txt") == 0) {
            put_out("[{\"matches\":[" UNC_MATCH("/\\\\\\\\nas\\\\backup/b.txt", "3")
                    "],\"hits\":1,\"snapshot\":\"" UNC_SNAPSHOT_ID "\"}]\n");
        } else {
            put_out("[]\n");
        }
        return 0;
    }
    if (strcmp(cmd, "dump") == 0 && strcmp(path, "/\\\\srv\\share/dir/a.txt") == 0) {
        put_out("share");
        return 0;
    }
    if (strcmp(cmd, "dump") == 0 && strcmp(path, "/\\\\nas\\backup/b.txt") == 0) {
        put_out("nas");
        return 0;
    }
    put_err("Fatal: cannot dump file: path not found in snapshot\n");
    return 1;
}

int main(int argc, char** argv) {
    /* The bytes must reach the dump target exactly as written, without a text-mode "\n" -> "\r\n" */
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        put_err("fake_restic: cannot switch stdout to binary mode\n");
        return 98;
    }
    const char* mode = argc > 1 ? argv[argc - 1] : "";
    if (strcmp(mode, "version") != 0 && GetEnvironmentVariableA("FAKE_RESTIC_UNC", NULL, 0) > 0) {
        return fake_unc_repository(argc, argv);
    }

    if (strcmp(mode, "/ok") == 0) {
        put_out("hello");
        return 0;
    }
    if (strcmp(mode, "/env") == 0) {
        char repo[256] = {0}, password[256] = {0};
        GetEnvironmentVariableA("RESTIC_REPOSITORY", repo, sizeof(repo));
        GetEnvironmentVariableA("RESTIC_PASSWORD", password, sizeof(password));
        put_out(repo);
        put_out("|");
        put_out(password);
        return 0;
    }
    if (strcmp(mode, "version") == 0) {
        /* `restic --json version`; FAKE_RESTIC_VERSION sets the version, FAKE_RESTIC_VERSION_TEXT
           prints text instead of JSON, as restic did before 0.17.0 */
        char version[64] = "0.19.1";
        GetEnvironmentVariableA("FAKE_RESTIC_VERSION", version, sizeof(version));
        if (GetEnvironmentVariableA("FAKE_RESTIC_VERSION_TEXT", NULL, 0) > 0) {
            put_out("restic ");
            put_out(version);
            put_out(" compiled with go1.22.1 on windows/amd64\n");
            return 0;
        }
        put_out("{\"message_type\":\"version\",\"version\":\"");
        put_out(version);
        put_out("\",\"go_version\":\"go1.26.4\",\"go_os\":\"windows\",\"go_arch\":\"amd64\"}\n");
        return 0;
    }
    if (strcmp(mode, "/args") == 0) { /* Every argument, each followed by '|' */
        for (int i = 1; i < argc; ++i) {
            put_out(argv[i]);
            put_out("|");
        }
        return 0;
    }
    if (strcmp(mode, "/sleep") == 0) { /* The helper process of /child-*: lives for 20 s unless killed */
        Sleep(20000);
        return 0;
    }
    if (strcmp(mode, "/child-hang") == 0 || strcmp(mode, "/child-exit") == 0) {
        /* Starts a helper (as restic starts rclone or ssh), prints its process ID, then hangs or exits */
        char self[MAX_PATH], cmd[MAX_PATH + 32];
        DWORD n = GetModuleFileNameA(NULL, self, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return 97;
        snprintf(cmd, sizeof(cmd), "\"%s\" /sleep", self);
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 96;
        char pid[32];
        snprintf(pid, sizeof(pid), "%lu\n", pi.dwProcessId);
        put_out(pid);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (strcmp(mode, "/child-hang") == 0) Sleep(20000);
        return 0;
    }
    if (strcmp(mode, "/retry-auth") == 0) { /* restic's retries after an S3 server refused the secret key */
        put_err("Stat(<config/>) returned error, retrying after 757.01693ms: Stat: The request signature we calculated "
                "does not match the signature you provided. Check your key and signing method.\n");
        Sleep(20000);
        return 1;
    }
    if (strcmp(mode, "/retry-net") == 0) { /* A retry for another reason, then success */
        put_err("Load(<config/>, 0, 0) returned error, retrying after 1.2s: dial tcp 10.0.0.1:443: i/o timeout\n");
        put_out("ok");
        return 0;
    }
    if (strcmp(mode, "/empty") == 0) return 0;
    if (strcmp(mode, "/partial") == 0) {
        put_out("partial-data");
        put_err("Fatal: boom\n");
        return 1;
    }
    if (strcmp(mode, "/missing") == 0) {
        put_err("Fatal: cannot dump file: path not found in snapshot\n");
        return 1;
    }
    if (strcmp(mode, "/silent") == 0) return 1;
    if (strcmp(mode, "/longline") == 0) { /* 17 MiB without a line break, then success */
        static char block[1024 * 1024];
        memset(block, 'x', sizeof(block));
        for (int i = 0; i < 17; ++i) fwrite(block, 1, sizeof(block), stdout);
        fflush(stdout);
        return 0;
    }
    if (strcmp(mode, "/norepo") == 0) {
        put_err("Fatal: repository does not exist\n");
        return 10;
    }
    if (strcmp(mode, "/locked") == 0) return 11;
    if (strcmp(mode, "/badpw") == 0) return 12;
    if (strcmp(mode, "/hang") == 0) { /* Silent for longer than the test's inactivity timeout */
        Sleep(7000);
        return 0;
    }
    if (strcmp(mode, "/slow") == 0) { /* A byte every second: slow, but never inactive for long */
        for (const char* p = "abc"; *p != '\0'; ++p) {
            char one[2] = {*p, '\0'};
            put_out(one);
            Sleep(1000);
        }
        return 0;
    }
    return 99;
}
