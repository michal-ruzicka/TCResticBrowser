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

int main(int argc, char** argv) {
    /* The bytes must reach the dump target exactly as written, without a text-mode "\n" -> "\r\n" */
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
        put_err("fake_restic: cannot switch stdout to binary mode\n");
        return 98;
    }
    const char* mode = argc > 1 ? argv[argc - 1] : "";

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
    if (strcmp(mode, "/args") == 0) { /* Every argument, each followed by '|' */
        for (int i = 1; i < argc; ++i) {
            put_out(argv[i]);
            put_out("|");
        }
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
