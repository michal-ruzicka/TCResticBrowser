#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#define _CRTDBG_MAP_ALLOC
#include <stdlib.h>
#include <crtdbg.h>
#else
#include <stdlib.h>
#endif
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "fsplugin.h"
#include "file_history.h"
#include "restic_cli.h"
#include "repo_cache.h"

/*
 * A ResticRepo for the calls below: repository and password, nothing else. One static structure
 * rather than a compound literal per call, each of which would take its own stack space: a test
 * function with dozens of calls would exceed what /analyze allows a frame (C6262, 16 KB).
 */
static const ResticRepo* REPO(const WCHAR* target, const WCHAR* password) {
    static ResticRepo repo;
    memset(&repo, 0, sizeof(repo));
    repo.target   = target;
    repo.password = password;
    return &repo;
}

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

#define EXPECT_STR_EQ(actual, expected, msg) do { \
    g_tests_run++; \
    if (strcmp((actual), (expected)) != 0) { \
        printf("[FAIL] %s:%d: %s (expected '%s', got '%s')\n", \
               __FUNCTION__, __LINE__, (msg), (expected), (actual)); \
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

static unsigned long long ft_u64(FILETIME ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static unsigned long long file_size_of(const WIN32_FIND_DATAW* fd) {
    return ((unsigned long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
}

static const WIN32_FIND_DATAW* find_entry(const WIN32_FIND_DATAW* items, size_t count, const WCHAR* name) {
    for (size_t i = 0; i < count; ++i) {
        if (wcscmp(items[i].cFileName, name) == 0) return &items[i];
    }
    return NULL;
}

static BOOL is_dir_entry(const WIN32_FIND_DATAW* fd) {
    return (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* ============================================================================================ */
/* Pure unit tests                                                                              */
/* ============================================================================================ */

/* ISO-8601 parsing: exact values, timezones, fractions and rejection of invalid dates */
static void test_iso8601_parsing(void) {
    FILETIME ft, ft2;

    EXPECT_TRUE(restic_parse_iso8601_time("1970-01-01T00:00:00Z", &ft), "Unix epoch parses");
    EXPECT_TRUE(ft_u64(ft) == 116444736000000000ULL, "Unix epoch equals the known FILETIME value");

    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T14:28:15.182004Z", &ft), "UTC Z timestamp");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T16:28:15.182004+02:00", &ft2), "+02:00 timestamp");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+02:00 converts to identical UTC FILETIME");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T09:28:15.182004-05:00", &ft2), "-05:00 timestamp");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "-05:00 converts to identical UTC FILETIME");

    /* Compact and hour-only offsets */
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T10:00:00+0530", &ft), "+0530 offset");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T04:30:00Z", &ft2), "Reference UTC time");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+0530 equals 04:30 UTC");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T12:00:00+02", &ft), "+02 offset");
    EXPECT_TRUE(restic_parse_iso8601_time("2026-10-18T10:00:00Z", &ft2), "Reference UTC time");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(ft2), "+02 equals 10:00 UTC");

    /* Fractional seconds: 100 ns resolution, extra digits ignored */
    SYSTEMTIME st = {2020, 2, 0, 29, 12, 34, 56, 0};
    FILETIME base;
    EXPECT_TRUE(SystemTimeToFileTime(&st, &base), "Reference leap-day FILETIME");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:56.5Z", &ft), "Fraction .5");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 5000000ULL, "Fraction .5 = 5,000,000 ticks");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:56.1234567891Z", &ft), "10 fractional digits");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 1234567ULL, "Digits beyond 100 ns are ignored");
    EXPECT_TRUE(restic_parse_iso8601_time("2020-02-29T12:34:60Z", &ft), "Leap second accepted");
    EXPECT_TRUE(ft_u64(ft) == ft_u64(base) + 30000000ULL, "Leap second maps to 12:34:59");

    /* Rejections */
    EXPECT_TRUE(!restic_parse_iso8601_time(NULL, &ft), "NULL string");
    EXPECT_TRUE(!restic_parse_iso8601_time("invalid-date", &ft), "Garbage");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18", &ft), "Date without time");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-02-30T10:00:00Z", &ft), "February 30");
    EXPECT_TRUE(!restic_parse_iso8601_time("2021-02-29T10:00:00Z", &ft), "Feb 29 of a non-leap year");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-13-01T10:00:00Z", &ft), "Month 13");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T25:00:00Z", &ft), "Hour 25");
    EXPECT_TRUE(!restic_parse_iso8601_time("0001-01-01T00:00:00Z", &ft), "Year before FILETIME epoch");
    EXPECT_TRUE(!restic_parse_iso8601_time("1601-01-01T00:00:00+02:00", &ft), "Offset pushing before epoch");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+24:00", &ft), "Offset hour 24");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+02:75", &ft), "Offset minute 75");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+02:5", &ft), "Incomplete offset minutes");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00X", &ft), "Unknown zone designator");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00Zjunk", &ft), "Characters after Z");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00+02:00x", &ft), "Characters after the offset");
    EXPECT_TRUE(!restic_parse_iso8601_time("2026-10-18T10:00:00.Z", &ft), "Fraction without digits");
}

/* WFX path -> Restic filter path, and command-line quoting */
static void test_path_and_quote_helpers(void) {
    char norm[256] = {0};
    EXPECT_TRUE(restic_normalize_filter_path(L"", norm, sizeof(norm)), "Empty path");
    EXPECT_STR_EQ(norm, "/", "Empty -> /");
    EXPECT_TRUE(restic_normalize_filter_path(NULL, norm, sizeof(norm)), "NULL path");
    EXPECT_STR_EQ(norm, "/", "NULL -> /");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\", norm, sizeof(norm)), "Root backslash");
    EXPECT_STR_EQ(norm, "/", "Backslash -> /");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\C\\Users\\Docs\\", norm, sizeof(norm)), "Subpath");
    EXPECT_STR_EQ(norm, "/C/Users/Docs", "Separators converted, trailing slash stripped");
    EXPECT_TRUE(restic_normalize_filter_path(L"C\\\\Users\\\\\\Docs", norm, sizeof(norm)), "Missing lead + doubled separators");
    EXPECT_STR_EQ(norm, "/C/Users/Docs", "Leading slash added, duplicates collapsed");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\\u00fcn\\\u65e5\u672c", norm, sizeof(norm)), "Unicode path");
    EXPECT_STR_EQ(norm, "/\xC3\xBCn/\xE6\x97\xA5\xE6\x9C\xAC", "UTF-8 encoding");
    EXPECT_TRUE(!restic_normalize_filter_path(L"\\abcdefghijklmnop", norm, 8), "Output buffer too small");
    /* A network share as main.c maps it, after a '/', and paths that only look like one */
    EXPECT_TRUE(restic_normalize_filter_path(L"/\\\\srv\\share\\dir\\", norm, sizeof(norm)), "Mapped share");
    EXPECT_STR_EQ(norm, "/\\\\srv\\share/dir", "The share's volume stays one component, as restic names it");
    EXPECT_TRUE(restic_normalize_filter_path(L"/\\\\?\\UNC\\srv\\share", norm, sizeof(norm)), "Mapped \\\\?\\UNC\\ share");
    EXPECT_STR_EQ(norm, "/\\\\?\\UNC\\srv\\share", "... with its prefix");
    EXPECT_TRUE(restic_normalize_filter_path(L"\\\\Z\\home\\x", norm, sizeof(norm)), "Doubled leading backslash");
    EXPECT_STR_EQ(norm, "/Z/home/x", "... is no share: Total Commander's paths start with a backslash");
    EXPECT_TRUE(restic_normalize_filter_path(L"/\\\\..\\..\\x", norm, sizeof(norm)), "A share named ..");
    EXPECT_STR_EQ(norm, "/../../x", "... is no share either");
    EXPECT_TRUE(!restic_normalize_filter_path(L"/\\\\srv\\share", norm, 12), "No room for the share");

    /* A maximum-length (1023 UTF-16 units) path made of 3-byte characters must fit */
    WCHAR* long_path = (WCHAR*)calloc(1024, sizeof(WCHAR));
    char* long_utf8 = (char*)calloc(3200, 1);
    EXPECT_TRUE(long_path && long_utf8, "Allocate long path buffers");
    long_path[0] = L'\\';
    for (int i = 1; i < 1023; ++i) long_path[i] = L'\u65e5';
    EXPECT_TRUE(restic_normalize_filter_path(long_path, long_utf8, 3200), "1023-character CJK path");
    EXPECT_TRUE(strlen(long_utf8) == 1 + 1022 * 3, "Long path length preserved");
    free(long_path);
    free(long_utf8);

    /* Quoting: exact output ... */
    WCHAR* q = restic_quote_arg(L"plain");
    EXPECT_TRUE(q != NULL, "Quote plain");
    EXPECT_WSTR_EQ(q, L"\"plain\"", "Plain argument");
    free(q);
    q = restic_quote_arg(L"");
    EXPECT_WSTR_EQ(q, L"\"\"", "Empty argument");
    free(q);
    q = restic_quote_arg(L"say \"hi\"");
    EXPECT_WSTR_EQ(q, L"\"say \\\"hi\\\"\"", "Embedded quotes escaped");
    free(q);
    q = restic_quote_arg(L"C:\\dir\\");
    EXPECT_WSTR_EQ(q, L"\"C:\\dir\\\\\"", "Trailing backslash doubled");
    free(q);

    /* ... and round-trip through the same parser used by the C runtime of the child */
    const WCHAR* nasty[] = {
        L"simple", L"with space", L"quote\"inside", L"\"", L"\\\"", L"back\\\\\"slash",
        L"trail\\", L"trail\\\\", L"/C/odd [dir] & (x) ^ 100%", L"/a\\b\"c d\\\\", L"\u00fcn\u65e5"
    };
    for (size_t i = 0; i < _countof(nasty); ++i) {
        q = restic_quote_arg(nasty[i]);
        EXPECT_TRUE(q != NULL, "Quote nasty argument");
        size_t cmd_len = wcslen(q) + 16;
        WCHAR* cmd = (WCHAR*)malloc(cmd_len * sizeof(WCHAR));
        swprintf_s(cmd, cmd_len, L"prog.exe %ls", q);
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
        EXPECT_TRUE(argv != NULL && argc == 2, "Quoted argument stays a single token");
        EXPECT_WSTR_EQ(argv[1], nasty[i], "Argument survives CommandLineToArgvW round-trip");
        LocalFree(argv);
        free(cmd);
        free(q);
    }
}

/* Parsing `restic snapshots --json` */
/* Hints about stray whitespace and invisible characters in a rejected password */
static void test_describe_password(void) {
    WCHAR hint[384];
    EXPECT_TRUE(!restic_describe_password(L"0123456789abcdef", hint, _countof(hint)) && hint[0] == L'\0',
                "Plain password: nothing to report");
    EXPECT_TRUE(!restic_describe_password(L"correct horse battery staple", hint, _countof(hint)),
                "Inner ordinary spaces of a passphrase are not reported");
    EXPECT_TRUE(!restic_describe_password(L"p\u00e4ss\u65e5\u672c", hint, _countof(hint)), "Non-ASCII letters are fine");
    EXPECT_TRUE(!restic_describe_password(L"", hint, _countof(hint)) && !restic_describe_password(NULL, hint, _countof(hint)),
                "Empty and NULL passwords");

    EXPECT_TRUE(restic_describe_password(L"0123abcd ", hint, _countof(hint)), "Trailing space reported");
    EXPECT_WSTR_EQ(hint, L"ends with a space", "Trailing space");
    restic_describe_password(L" 0123abcd", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint, L"starts with a space", "Leading space");
    restic_describe_password(L"0123abcd\r\n", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint, L"ends with a line break", "Trailing CRLF");
    restic_describe_password(L"\tab\ncd\u00a0", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint, L"starts with a tab; ends with a non-breaking space; contains a line break",
                   "Leading tab, inner line feed, trailing no-break space");
    restic_describe_password(L"ab\u200bcd\aef\u3000gh\tij", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint,
                   L"contains a tab; contains a control character; contains an unusual space character; "
                   L"contains an invisible character (such as a zero-width space)",
                   "Inner tab, bell, ideographic space and zero-width space");
    restic_describe_password(L"\ufeffsecret", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint, L"starts with an invisible character (such as a zero-width space)", "Leading BOM");
    restic_describe_password(L" ", hint, _countof(hint));
    EXPECT_WSTR_EQ(hint, L"starts with a space", "Single space");
    WCHAR tiny[8];
    EXPECT_TRUE(restic_describe_password(L" x ", tiny, _countof(tiny)) && wcslen(tiny) < _countof(tiny),
                "Truncated to a small buffer");
}

#define HEADER_LINE "{\"time\":\"2026-10-18T16:28:15+02:00\",\"id\":\"29095f00\",\"message_type\":\"snapshot\"}\r\n"

/* Parsing `restic ls --json` NDJSON */
static void test_parse_ls_ndjson(void) {
    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    ResticResult res;

    /* Direct children only: self node, grandchildren and prefix-sharing siblings are excluded.
       5 GiB + 7 bytes = 5368709127 -> High DWORD 1, Low DWORD 1073741831. */
    const char* sample =
        HEADER_LINE
        "{\"name\":\"sample_data\",\"type\":\"dir\",\"path\":\"/tmp/sample_data\",\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"big_archive.tar\",\"type\":\"file\",\"path\":\"/tmp/sample_data/big_archive.tar\",\"size\":5368709127,\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "\r\n"
        "{\"name\":\"sub1\",\"type\":\"dir\",\"path\":\"/tmp/sample_data/sub1\",\"mtime\":\"2026-10-18T16:28:12+02:00\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"deep.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data/sub1/deep.txt\",\"size\":42,\"message_type\":\"node\"}\r\n"
        "{\"name\":\"sample_data2\",\"type\":\"dir\",\"path\":\"/tmp/sample_data2\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"x.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data2/x.txt\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"sample_data.bak\",\"type\":\"file\",\"path\":\"/tmp/sample_data.bak\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"link\",\"type\":\"symlink\",\"path\":\"/tmp/sample_data/link\",\"mtime\":\"garbage\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"empty.txt\",\"type\":\"file\",\"path\":\"/tmp/sample_data/empty.txt\",\"size\":0,\"message_type\":\"node\"}";

    res = restic_parse_ls_ndjson(sample, "/tmp/sample_data", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK, "Parse NDJSON stream");
    EXPECT_TRUE(count == 4, "big_archive.tar, sub1, link and empty.txt only");
    const WIN32_FIND_DATAW* big = find_entry(items, count, L"big_archive.tar");
    EXPECT_TRUE(big != NULL && !is_dir_entry(big), "File entry");
    EXPECT_TRUE(big->nFileSizeHigh == 1 && big->nFileSizeLow == 1073741831U, "64-bit size split into High/Low");
    EXPECT_TRUE(file_size_of(big) == 5368709127ULL, "64-bit size recombines");
    EXPECT_TRUE((big->dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0, "Entries are read-only");
    const WIN32_FIND_DATAW* sub = find_entry(items, count, L"sub1");
    EXPECT_TRUE(sub != NULL && is_dir_entry(sub), "Directory entry");
    EXPECT_TRUE(find_entry(items, count, L"sample_data") == NULL, "Filter directory itself is not listed");
    EXPECT_TRUE(find_entry(items, count, L"deep.txt") == NULL, "Grandchildren are not listed");
    EXPECT_TRUE(find_entry(items, count, L"sample_data2") == NULL, "Prefix-sharing sibling directory is excluded");
    EXPECT_TRUE(find_entry(items, count, L"sample_data.bak") == NULL, "Prefix-sharing sibling file is excluded");
    const WIN32_FIND_DATAW* link = find_entry(items, count, L"link");
    EXPECT_TRUE(link != NULL && !is_dir_entry(link), "Symlinks are reported as plain files");
    EXPECT_TRUE(link->ftLastWriteTime.dwHighDateTime == 0xFFFFFFFF && link->ftLastWriteTime.dwLowDateTime == 0xFFFFFFFE,
                "Unparsable mtime -> 'no date' marker");
    const WIN32_FIND_DATAW* empty = find_entry(items, count, L"empty.txt");
    EXPECT_TRUE(empty != NULL && file_size_of(empty) == 0, "Zero-byte file");
    free(items);

    /* Root listing needs no self node */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"C\",\"type\":\"dir\",\"path\":\"/C\",\"message_type\":\"node\"}\r\n",
        "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"C") == 0, "Root listing");
    free(items);
    res = restic_parse_ls_ndjson(HEADER_LINE, "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 0 && items == NULL, "Empty snapshot root is valid");

    /* Names that would break Total Commander's navigation are never reported */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"\",\"type\":\"file\",\"path\":\"/x\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\".\",\"type\":\"dir\",\"path\":\"/.\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"..\",\"type\":\"dir\",\"path\":\"/..\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"ok.txt\",\"type\":\"file\",\"path\":\"/ok.txt\",\"message_type\":\"node\"}\r\n",
        "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"ok.txt") == 0,
                "Empty, '.' and '..' node names are skipped");
    free(items);

    /* A name that is not a single component would let a recursive copy (F5) escape the target
       directory: backslashes are legal in Linux file names, and Win32 strips trailing dots/spaces */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"..\\\\..\\\\evil.exe\",\"type\":\"file\",\"path\":\"/..\\\\..\\\\evil.exe\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"a\\\\b\",\"type\":\"dir\",\"path\":\"/a\\\\b\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"x/y\",\"type\":\"file\",\"path\":\"/xy\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"...\",\"type\":\"dir\",\"path\":\"/...\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\".. \",\"type\":\"dir\",\"path\":\"/.. \",\"message_type\":\"node\"}\r\n"
        "{\"name\":\" \",\"type\":\"file\",\"path\":\"/ \",\"message_type\":\"node\"}\r\n"
        "{\"name\":\".hidden\",\"type\":\"file\",\"path\":\"/.hidden\",\"message_type\":\"node\"}\r\n"
        "{\"name\":\"a..\",\"type\":\"file\",\"path\":\"/a..\",\"message_type\":\"node\"}\r\n",
        "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 2, "Only the two single-component names are reported");
    EXPECT_TRUE(find_entry(items, count, L".hidden") != NULL && find_entry(items, count, L"a..") != NULL,
                "Dot files and names merely ending in dots are kept");
    free(items);

    /* Existing but empty directory vs. missing path vs. file used as directory */
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"empty_dir\",\"type\":\"dir\",\"path\":\"/a/empty_dir\",\"message_type\":\"node\"}\r\n",
        "/a/empty_dir", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Empty directory exists");
    BOOL not_a_folder = TRUE;
    res = restic_parse_ls_ndjson(HEADER_LINE, "/a/missing", &items, &count, &not_a_folder);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && items == NULL && count == 0 && !not_a_folder, "Missing directory");
    res = restic_parse_ls_ndjson(HEADER_LINE
        "{\"name\":\"f.txt\",\"type\":\"file\",\"path\":\"/a/f.txt\",\"size\":3,\"message_type\":\"node\"}\r\n",
        "/a/f.txt", &items, &count, &not_a_folder);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && not_a_folder, "A file is not a listable directory, but is there");

    /* Names: UTF-8 decoding and over-long names */
    res = restic_parse_ls_ndjson(
        "{\"name\":\"\xC3\xBCn\xE6\x97\xA5\xE6\x9C\xAC\",\"type\":\"file\",\"path\":\"/\xC3\xBCn\xE6\x97\xA5\xE6\x9C\xAC\",\"message_type\":\"node\"}\n",
        "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "Unicode name");
    EXPECT_WSTR_EQ(items[0].cFileName, L"\u00fcn\u65e5\u672c", "UTF-8 name decoded to UTF-16");
    free(items);

    /* Corrupt or non-object lines */
    res = restic_parse_ls_ndjson(HEADER_LINE "this is not json\r\n", "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE && items == NULL && count == 0, "Garbage line aborts the parse");
    res = restic_parse_ls_ndjson(HEADER_LINE "[1,2,3]\r\n", "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Non-object line");
    res = restic_parse_ls_ndjson("{\"name\":\"x\",\"type\":\"file\",\"pa", "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE, "Truncated line");
    res = restic_parse_ls_ndjson("", "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Empty stream");
    res = restic_parse_ls_ndjson(NULL, "/", &items, &count, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL stream");
}

/* ============================================================================================ */
/* Child process tests (use cmd.exe as a deterministic stand-in for restic.exe)                  */
/* ============================================================================================ */

static BOOL get_cmd_exe(WCHAR* out, DWORD cap) {
    DWORD n = GetEnvironmentVariableW(L"ComSpec", out, cap);
    return n > 0 && n < cap;
}

static ResticResult run_cmd(const WCHAR* args, const ResticExecOptions* opts, ResticCommandOutput* out) {
    WCHAR cmd[MAX_PATH];
    if (!get_cmd_exe(cmd, MAX_PATH)) return RESTIC_ERR_SPAWN_FAILED;
    return restic_exec_command(cmd, REPO(L"repo", L"pw"), args, opts, out);
}

/* stdout/stderr are kept apart and exit codes are mapped */
static void test_exec_streams_and_exit_codes(void) {
    ResticCommandOutput out;

    ResticResult res = run_cmd(L"/c \"echo hello\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && out.exit_code == 0, "Successful command");
    EXPECT_TRUE(strstr(out.output, "hello") != NULL, "stdout captured");
    EXPECT_TRUE(out.error_output != NULL && out.error_length == 0, "stderr is an empty (non-NULL) string");
    restic_free_command_output(&out);

    res = run_cmd(L"/c \"echo to_stdout& echo to_stderr 1>&2& exit /b 3\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && out.exit_code == 3, "Exit code 3 -> RESTIC_ERR_CLI_FAILED");
    EXPECT_TRUE(strstr(out.output, "to_stdout") != NULL && strstr(out.output, "to_stderr") == NULL,
                "stderr text never leaks into stdout");
    EXPECT_TRUE(strstr(out.error_output, "to_stderr") != NULL && strstr(out.error_output, "to_stdout") == NULL,
                "stderr captured separately");
    EXPECT_TRUE(wcsncmp(out.error_message, L"to_stderr", 9) == 0, "Error message taken from stderr, newline trimmed");
    EXPECT_TRUE(wcsstr(out.error_message, L"\n") == NULL, "No trailing newline in error message");
    restic_free_command_output(&out);

    res = run_cmd(L"/c \"exit /b 10\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND, "Exit 10 -> repository not found");
    restic_free_command_output(&out);
    res = run_cmd(L"/c \"exit /b 11\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_LOCKED, "Exit 11 -> repository locked");
    restic_free_command_output(&out);
    res = run_cmd(L"/c \"exit /b 12\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Exit 12 -> wrong password");
    restic_free_command_output(&out);

    /* A JSON error object on stderr is reduced to its "message" field */
    res = run_cmd(L"/c \"echo {\"message_type\":\"exit_error\",\"message\":\"Fatal: boom\"} 1>&2& exit /b 1\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "JSON error exit");
    EXPECT_WSTR_EQ(out.error_message, L"Fatal: boom", "JSON message extracted");
    restic_free_command_output(&out);

    /* Very long messages are truncated and terminated, never rejected */
    WCHAR* long_args = (WCHAR*)calloc(2048, sizeof(WCHAR));
    EXPECT_TRUE(long_args != NULL, "Allocate");
    wcscpy_s(long_args, 2048, L"/c \"echo ");
    for (int i = 0; i < 1500; ++i) wcscat_s(long_args, 2048, L"y");
    wcscat_s(long_args, 2048, L" 1>&2& exit /b 1\"");
    res = run_cmd(long_args, NULL, &out);
    free(long_args);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "Long stderr line");
    EXPECT_TRUE(wcslen(out.error_message) == 511, "Message truncated to the buffer size");
    restic_free_command_output(&out);
}

/* Credentials are injected, conflicting inherited variables removed, the rest inherited */
static void test_exec_environment(void) {
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");

    SetEnvironmentVariableW(L"RESTIC_PASSWORD", L"parent_secret");
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_COMMAND", L"bogus_cmd");
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_FILE", L"bogus_file");
    SetEnvironmentVariableW(L"RESTIC_REPOSITORY_FILE", L"bogus_repo_file");
    SetEnvironmentVariableW(L"RESTIC_TEST_MARKER", L"inherited_marker");

    ResticCommandOutput out;
    ResticResult res = restic_exec_command(
        cmd, REPO(L"C:\\repo dir", L"p@ss w0rd \"q\" 100%"),
        L"/c \"echo [%RESTIC_REPOSITORY%][%RESTIC_PASSWORD%][%RESTIC_PASSWORD_COMMAND%]"
        L"[%RESTIC_PASSWORD_FILE%][%RESTIC_REPOSITORY_FILE%][%RESTIC_TEST_MARKER%][%SystemRoot%]\"",
        NULL, &out);

    SetEnvironmentVariableW(L"RESTIC_PASSWORD", NULL);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_COMMAND", NULL);
    SetEnvironmentVariableW(L"RESTIC_PASSWORD_FILE", NULL);
    SetEnvironmentVariableW(L"RESTIC_REPOSITORY_FILE", NULL);
    SetEnvironmentVariableW(L"RESTIC_TEST_MARKER", NULL);

    EXPECT_TRUE(res == RESTIC_OK, "Environment probe ran");
    EXPECT_TRUE(strstr(out.output, "[C:\\repo dir][p@ss w0rd \"q\" 100%]") != NULL,
                "Repository and password (spaces, quotes, percent) delivered verbatim");
    EXPECT_TRUE(strstr(out.output, "[%RESTIC_PASSWORD_COMMAND%][%RESTIC_PASSWORD_FILE%][%RESTIC_REPOSITORY_FILE%]") != NULL,
                "Conflicting inherited RESTIC_* variables are removed");
    EXPECT_TRUE(strstr(out.output, "[inherited_marker]") != NULL, "Other variables are inherited");
    EXPECT_TRUE(strstr(out.output, "[%SystemRoot%]") == NULL, "System variables are inherited");
    restic_free_command_output(&out);
}

/*
 * A password file or command instead of a typed password, and S3 settings: those of a repository
 * replace every inherited AWS_*, MINIO_* and RESTIC_AWS_* variable, which are inherited otherwise
 */
static void test_exec_environment_credentials(void) {
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");
    static const WCHAR* const inherited[][2] = {
        {L"AWS_ACCESS_KEY_ID", L"parent_key"}, {L"AWS_DEFAULT_REGION", L"parent_region"},
        {L"MINIO_ACCESS_KEY", L"parent_minio"}, {L"RESTIC_AWS_ASSUME_ROLE_ARN", L"parent_role"},
    };
    for (size_t i = 0; i < _countof(inherited); ++i) SetEnvironmentVariableW(inherited[i][0], inherited[i][1]);
    static const WCHAR probe[] =
        L"/c \"echo [%RESTIC_PASSWORD%][%RESTIC_PASSWORD_FILE%][%RESTIC_PASSWORD_COMMAND%][%AWS_ACCESS_KEY_ID%]"
        L"[%AWS_SECRET_ACCESS_KEY%][%AWS_SHARED_CREDENTIALS_FILE%][%AWS_PROFILE%][%AWS_DEFAULT_REGION%]"
        L"[%MINIO_ACCESS_KEY%][%RESTIC_AWS_ASSUME_ROLE_ARN%]\"";

    ResticRepo external = {.target = L"r", .password_file = L"C:\\pw dir\\pw.txt",
                           .password_command = L"\"C:\\x y\\kp.exe\" --name 'a b'"};
    ResticCommandOutput out;
    ResticResult res = restic_exec_command(cmd, &external, probe, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Probe with a password file and command ran");
    EXPECT_TRUE(strstr(out.output, "[%RESTIC_PASSWORD%][C:\\pw dir\\pw.txt][\"C:\\x y\\kp.exe\" --name 'a b']"
                                   "[parent_key][%AWS_SECRET_ACCESS_KEY%][%AWS_SHARED_CREDENTIALS_FILE%][%AWS_PROFILE%]"
                                   "[parent_region][parent_minio][parent_role]") != NULL,
                "No RESTIC_PASSWORD; file and command both passed verbatim; without S3 settings AWS_* are inherited");
    restic_free_command_output(&out);

    ResticRepo s3 = {.target = L"s3:host/bucket", .password = L"pw", .aws_secret_access_key = L"s3cr3t/key+",
                     .environment = L"AWS_ACCESS_KEY_ID=AKIAOWN\0AWS_SHARED_CREDENTIALS_FILE=C:\\aws\\credentials\0"
                                    L"AWS_PROFILE=backup\0"};
    res = restic_exec_command(cmd, &s3, probe, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Probe with S3 settings ran");
    EXPECT_TRUE(strstr(out.output, "[pw][%RESTIC_PASSWORD_FILE%][%RESTIC_PASSWORD_COMMAND%][AKIAOWN][s3cr3t/key+]"
                                   "[C:\\aws\\credentials][backup][%AWS_DEFAULT_REGION%][%MINIO_ACCESS_KEY%]"
                                   "[%RESTIC_AWS_ASSUME_ROLE_ARN%]") != NULL,
                "The repository's S3 settings replace every inherited AWS_*, MINIO_*, RESTIC_AWS_* variable");
    restic_free_command_output(&out);

    ResticRepo profile_only = {.target = L"s3:host/bucket", .password = L"pw", .environment = L"AWS_PROFILE=backup\0"};
    res = restic_exec_command(cmd, &profile_only, probe, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && strstr(out.output, "[pw][%RESTIC_PASSWORD_FILE%][%RESTIC_PASSWORD_COMMAND%]"
                                                       "[%AWS_ACCESS_KEY_ID%]") != NULL,
                "AWS_PROFILE alone also leaves the inherited access key out");
    restic_free_command_output(&out);

    ResticRepo region = {.target = L"s3:host/bucket", .password = L"pw",
                         .environment = L"AWS_DEFAULT_REGION=eu-central-1\0RESTIC_AWS_ASSUME_ROLE_ARN=arn:own\0"};
    res = restic_exec_command(cmd, &region, probe, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && strstr(out.output, "[%AWS_ACCESS_KEY_ID%][%AWS_SECRET_ACCESS_KEY%]"
                                                       "[%AWS_SHARED_CREDENTIALS_FILE%][%AWS_PROFILE%][eu-central-1]"
                                                       "[%MINIO_ACCESS_KEY%][arn:own]") != NULL,
                "Region and role of the repository replace the inherited ones");
    restic_free_command_output(&out);

    for (size_t i = 0; i < _countof(inherited); ++i) SetEnvironmentVariableW(inherited[i][0], NULL);

    ResticRepo no_password = {.target = L"r", .environment = L"AWS_ACCESS_KEY_ID=AKIA\0"};
    EXPECT_TRUE(restic_exec_command(cmd, &no_password, probe, NULL, &out) == RESTIC_ERR_INVALID_ARG &&
                    !restic_repo_usable(&no_password) && !restic_repo_usable(NULL),
                "No password, password file or command: refused before anything is started");
    ResticRepo empty_password = {.target = L"r", .password = L"", .password_file = L""};
    EXPECT_TRUE(!restic_repo_usable(&empty_password), "Empty strings count as not set");
}

static int g_cancel_polls = 0;
static BOOL cancel_after_first_poll(void* ctx) {
    (void)ctx;
    g_cancel_polls++;
    return TRUE;
}

/* Timeout, cancellation and spawn / argument failures */
static void test_exec_limits_and_failures(void) {
    ResticCommandOutput out;
    ResticExecOptions opts;
    ULONGLONG t0;

    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 400;
    t0 = GetTickCount64();
    ResticResult res = run_cmd(L"/c \"ping -n 6 127.0.0.1 >nul\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT, "Long-running child hits the timeout");
    EXPECT_TRUE(GetTickCount64() - t0 < 4000, "Timeout returns promptly");
    restic_free_command_output(&out);

    memset(&opts, 0, sizeof(opts));
    opts.cancel_proc = cancel_after_first_poll;
    g_cancel_polls = 0;
    t0 = GetTickCount64();
    res = run_cmd(L"/c \"ping -n 6 127.0.0.1 >nul\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED && g_cancel_polls >= 1, "Cancellation callback aborts the child");
    EXPECT_TRUE(GetTickCount64() - t0 < 4000, "Cancellation returns promptly");
    restic_free_command_output(&out);

    /* A generous timeout must not disturb a fast command */
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 30000;
    res = run_cmd(L"/c \"echo ok\"", &opts, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Fast command within timeout");
    restic_free_command_output(&out);

    res = restic_exec_command(L"C:\\no\\such\\dir\\restic.exe", REPO(L"r", L"p"), L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Missing executable");
    EXPECT_TRUE(out.output == NULL, "No buffers allocated on spawn failure");
    EXPECT_TRUE(wcsstr(out.error_message, L"Cannot start C:\\no\\such\\dir\\restic.exe") != NULL &&
                wcsstr(out.error_message, L"(Windows error ") != NULL,
                "A spawn failure names the binary and the Windows error");

    /*
     * Files that exist but are no Windows program (text, a DOS "MZ" stub without "PE\0\0") are
     * refused before CreateProcessW: on 64-bit Windows it would show the modal "Unsupported 16-Bit
     * Application" dialog and stop the test run until someone clicks it
     */
    static const struct {
        const char* bytes;
        DWORD size;
    } not_programs[] = {
        {"not a program", 13},
        {"MZ" "\x90\0\x03\0\0\0\x04\0\0\0\xff\xff\0\0\xb8\0\0\0\0\0\0\0\x40\0\0\0\0\0\0\0"
         "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x40\0\0\0" "NE\0\0", 68},
    };
    WCHAR not_exe[MAX_PATH], dest[MAX_PATH], err_text[512];
    GetTempPathW(MAX_PATH, not_exe);
    wcscat_s(not_exe, MAX_PATH, L"restic_test_not_a_program.exe");
    swprintf_s(dest, MAX_PATH, L"%ls.out", not_exe);
    for (size_t i = 0; i < _countof(not_programs); ++i) {
        HANDLE h = CreateFileW(not_exe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD written = 0;
        EXPECT_TRUE(h != INVALID_HANDLE_VALUE &&
                    WriteFile(h, not_programs[i].bytes, not_programs[i].size, &written, NULL),
                    "Create a fake restic.exe");
        CloseHandle(h);
        res = restic_exec_command(not_exe, REPO(L"r", L"p"), L"version", NULL, &out);
        EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED && wcsstr(out.error_message, not_exe) != NULL &&
                    wcsstr(out.error_message, L"not a Windows program") != NULL,
                    "A file that is no Windows program is refused with its path");
        DeleteFileW(dest);
        res = restic_dump_file(not_exe, REPO(L"r", L"p"), "deadbeef", L"/f.txt", dest, FALSE, NULL, 0, NULL, NULL,
                               err_text, _countof(err_text));
        EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED && wcsstr(err_text, L"not a Windows program") != NULL &&
                    GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES,
                    "restic_dump_file refuses it too and writes nothing");
    }
    DeleteFileW(not_exe);

    /* A folder named like the binary passes the check and fails in CreateProcessW, with the reason */
    CreateDirectoryW(not_exe, NULL);
    res = restic_exec_command(not_exe, REPO(L"r", L"p"), L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED && wcsstr(out.error_message, not_exe) != NULL &&
                wcsstr(out.error_message, L"(Windows error ") != NULL && wcsstr(out.error_message, L"%1") == NULL,
                "A spawn failure names the binary and the Windows reason, with inserts filled in");
    RemoveDirectoryW(not_exe);
    res = restic_exec_command(L"restic.exe", REPO(L"r", L"p"), L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Bare executable names are refused (no cwd search)");
    res = restic_exec_command(L"", REPO(L"r", L"p"), L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Empty executable name");
    res = restic_exec_command(NULL, REPO(L"r", L"p"), L"version", NULL, &out);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL executable");
    res = restic_exec_command(L"C:\\x\\restic.exe", REPO(L"r", L"p"), L"version", NULL, NULL);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL result");

    WCHAR* huge = (WCHAR*)calloc(40000, sizeof(WCHAR));
    EXPECT_TRUE(huge != NULL, "Allocate");
    wmemset(huge, L'x', 39999);
    WCHAR cmd[MAX_PATH];
    EXPECT_TRUE(get_cmd_exe(cmd, MAX_PATH), "ComSpec available");
    res = restic_exec_command(cmd, REPO(L"r", L"p"), huge, NULL, &out);
    free(huge);
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Command line above the CreateProcess limit");

    restic_free_command_output(&out); /* Safe on a zeroed / already released struct */
    restic_free_command_output(NULL);
}

/* Large outputs (buffer growth) and long command lines are handled */
static void test_exec_large_output_and_long_args(void) {
    ResticCommandOutput out;

    ResticResult res = run_cmd(L"/c \"for /L %i in (1,1,4000) do @echo line_%i_0123456789_0123456789\"", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Large output command");
    EXPECT_TRUE(out.length > 100000, "More than 100 KB captured");
    EXPECT_TRUE(strlen(out.output) == out.length, "Length matches the NUL-terminated data");
    EXPECT_TRUE(strstr(out.output, "line_1_0123") != NULL && strstr(out.output, "line_4000_0123") != NULL,
                "First and last lines are present");
    restic_free_command_output(&out);

    WCHAR* args = (WCHAR*)calloc(6100, sizeof(WCHAR));
    EXPECT_TRUE(args != NULL, "Allocate");
    wcscpy_s(args, 6100, L"/c \"echo ");
    for (int i = 0; i < 6000; ++i) wcscat_s(args, 6100, L"x");
    wcscat_s(args, 6100, L"\"");
    res = run_cmd(args, NULL, &out);
    free(args);
    EXPECT_TRUE(res == RESTIC_OK && out.length >= 6000, "Command line beyond the old 2048-character limit");
    restic_free_command_output(&out);
}

/* The cache keeps what was read for one repository when another one is forgotten meanwhile */
static void test_repo_cache_generations(void) {
    const char* fmt = SNAPSHOT_TREE_DEFAULT_TIME_FORMAT;
    SnapshotTree *t1 = NULL, *t2 = NULL, *t3 = NULL, *t4 = NULL;
    EXPECT_TRUE(snapshot_tree_build("null", 4, NULL, &t1) == SNAPSHOT_TREE_OK && snapshot_tree_build("null", 4, NULL, &t2) == SNAPSHOT_TREE_OK &&
                snapshot_tree_build("null", 4, NULL, &t3) == SNAPSHOT_TREE_OK && snapshot_tree_build("null", 4, NULL, &t4) == SNAPSHOT_TREE_OK,
                "Trees to cache");
    repo_cache_clear();
    ULONGLONG age = 0, g = repo_cache_generation();
    repo_cache_forget(L"Other");                         /* E.g. its password timed out meanwhile */
    repo_cache_put_tree(L"RepoB", L"C:\\b", fmt, t1, g); /* Read for RepoB before that */
    repo_cache_put_tree(L"other", L"C:\\o", fmt, t2, g); /* Read for the forgotten one before that */
    const SnapshotTree* b = repo_cache_lock_tree(L"RepoB", L"C:\\b", fmt, &age);
    if (b) repo_cache_unlock();
    const SnapshotTree* o = repo_cache_lock_tree(L"Other", L"C:\\o", fmt, &age);
    if (o) repo_cache_unlock();
    EXPECT_TRUE(b == t1, "Forgetting one repository keeps what is read for another");
    EXPECT_TRUE(o == NULL, "...but discards what was read for the forgotten one (alias case ignored)");
    repo_cache_put_tree(L"Other", L"C:\\o", fmt, t3, repo_cache_generation());
    o = repo_cache_lock_tree(L"Other", L"C:\\o", fmt, &age);
    if (o) repo_cache_unlock();
    EXPECT_TRUE(o == t3, "Data read after the forget is kept");
    g = repo_cache_generation();
    repo_cache_clear();
    repo_cache_put_tree(L"RepoB", L"C:\\b", fmt, t4, g);
    b = repo_cache_lock_tree(L"RepoB", L"C:\\b", fmt, &age);
    if (b) repo_cache_unlock();
    EXPECT_TRUE(b == NULL, "Clearing discards what any repository read before it");
    repo_cache_clear();
}

static char* heap_text(const char* text) {
    size_t n = strlen(text) + 1;
    char* copy = (char*)malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}

/* The output of restic find kept per repository: the last few, by key, within their age */
static void test_repo_cache_histories(void) {
    repo_cache_clear();
    char* json = NULL;
    size_t len = 0;
    ULONGLONG g = repo_cache_generation();
    repo_cache_put_history(L"Repo", L"C:\\r", "pc\n/a", heap_text("[1]"), 3, g);
    EXPECT_TRUE(repo_cache_get_history(L"repo", L"C:\\r", "pc\n/a", 60000, &json, &len) && len == 3 &&
                    strcmp(json, "[1]") == 0,
                "A history cached and read back (alias case ignored)");
    free(json);
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\other", "pc\n/a", 60000, &json, &len) && !json &&
                    !repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/b", 60000, &json, &len),
                "...only for its repository and key");
    Sleep(20);
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/a", 0, &json, &len), "...within its age");
    repo_cache_put_history(L"Repo", L"C:\\r", "pc\n/a", heap_text("[22]"), 4, g);
    EXPECT_TRUE(repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/a", 60000, &json, &len) && strcmp(json, "[22]") == 0,
                "The same key again replaces it");
    free(json);
    for (int i = 0; i < 8; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "pc\n/%d", i);
        repo_cache_put_history(L"Repo", L"C:\\r", key, heap_text("[]"), 2, g);
    }
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/a", 60000, &json, &len) &&
                    repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/0", 60000, &json, &len),
                "Eight kept per repository, the oldest dropped");
    free(json);

    /* 32 MiB together: a third history of 12 MiB drops the oldest, and the oldest go first */
    const size_t big = 12u * 1024u * 1024u;
    for (int i = 0; i < 3; ++i) {
        char key[16];
        char* text = (char*)malloc(big + 1);
        if (text) {
            memset(text, '[', big);
            text[big] = '\0';
        }
        snprintf(key, sizeof(key), "pc\n/big%d", i);
        if (text) repo_cache_put_history(L"Repo", L"C:\\r", key, text, big, g);
    }
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/big0", 60000, &json, &len) &&
                    !repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/7", 60000, &json, &len),
                "Beyond 32 MiB the oldest dropped, and everything older than it");
    EXPECT_TRUE(repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/big2", 60000, &json, &len) && len == big,
                "...the newest kept");
    free(json);
    char* huge = (char*)malloc(33u * 1024u * 1024u + 1);
    if (huge) {
        memset(huge, '[', 33u * 1024u * 1024u);
        huge[33u * 1024u * 1024u] = '\0';
        repo_cache_put_history(L"Repo", L"C:\\r", "pc\n/huge", huge, 33u * 1024u * 1024u, g);
    }
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/huge", 60000, &json, &len) &&
                    repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/big1", 60000, &json, &len),
                "One history over 32 MiB is not kept, and drops nothing");
    free(json);
    repo_cache_forget(L"Repo");
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/big2", 60000, &json, &len),
                "Forgetting the repository drops them");
    repo_cache_put_history(L"Repo", L"C:\\r", "pc\n/a", heap_text("[]"), 2, g);
    EXPECT_TRUE(!repo_cache_get_history(L"Repo", L"C:\\r", "pc\n/a", 60000, &json, &len),
                "...and what was read before the forget is not kept");
    repo_cache_clear();
}

/* RepoSecretSource for the background loads of the live test */
static BOOL fixture_password(const WCHAR* alias, RepoSecrets* out) {
    (void)alias;
    memset(out, 0, sizeof(*out));
    return wcscpy_s(out->password, _countof(out->password), L"p@ss \"w0rd\" \u00fc %PATH% \u65e5\u672c") == 0;
}

/* A configured repository without [Repository: <alias>] settings, for the background loads */
static const ResticRepoEntry* live_entry(ResticRepoEntry* e, const WCHAR* alias, const WCHAR* target) {
    memset(e, 0, sizeof(*e));
    wcscpy_s(e->alias, MAX_PATH, alias);
    wcscpy_s(e->target, CONFIG_MAX_PATH, target);
    return e;
}

/* Counts the lines a line consumer receives, checking that they arrive whole and in order */
typedef struct {
    int lines;
    int in_order;
    int stop_after;   /* Stop the child after this many lines (0: never) */
} LineCount;

static ResticResult count_line(const char* line, size_t len, void* ctx) {
    LineCount* c = (LineCount*)ctx;
    char expected[64];
    int n = snprintf(expected, sizeof(expected), "line_%d_0123456789_0123456789 ", c->lines + 1);
    if (len < (size_t)n - 1 || memcmp(line, expected, (size_t)n - 1) != 0 || memchr(line, '\n', len) != NULL) {
        c->in_order = 0;
    }
    c->lines++;
    return c->stop_after && c->lines >= c->stop_after ? RESTIC_ERR_JSON_PARSE : RESTIC_OK;
}

/* ResticExecOptions.line_proc: stdout arrives line by line and is not kept */
static void test_exec_line_streaming(void) {
    ResticCommandOutput out;
    LineCount count = {0, 1, 0};
    ResticExecOptions opts = {0, NULL, NULL, count_line, &count};
    /*
     * 40 000 lines (~1.3 MB, more than one 1 MiB read slice), the last one without a line break.
     * `set /p` without input leaves ERRORLEVEL 1 in cmd.exe (not under Wine), hence `exit 0`.
     */
    ResticResult res = run_cmd(L"/c \"(for /L %i in (1,1,39999) do @echo line_%i_0123456789_0123456789 ) & "
                               L"(<nul set /p=line_40000_0123456789_0123456789 ) & exit 0\"", &opts, &out);
    size_t kept = out.length;
    restic_free_command_output(&out); /* Before the checks, which return early on failure */
    EXPECT_TRUE(res == RESTIC_OK, "Streamed command succeeds");
    EXPECT_TRUE(count.lines == 40000 && count.in_order, "Every line whole and in order, the last one too");
    EXPECT_TRUE(kept < 4096, "stdout was not accumulated");

    /* stderr beyond 1 MiB is read but not kept: a flood of warnings cannot fill memory */
    ResticResult flood = run_cmd(L"/c \"(for /L %i in (1,1,30000) do @echo warning_%i_0123456789_0123456789_0123456789 1>&2) & exit 0\"",
                                 NULL, &out);
    size_t err_kept = out.error_length;
    restic_free_command_output(&out);
    EXPECT_TRUE(flood == RESTIC_OK && err_kept >= 1024u * 1024u && err_kept < 1024u * 1024u + 128u * 1024u,
                "stderr is kept up to 1 MiB, the rest read and dropped");

    LineCount stop = {0, 1, 100};
    opts.line_ctx = &stop;
    res = run_cmd(L"/c \"for /L %i in (1,1,200000) do @echo line_%i_0123456789_0123456789 \"", &opts, &out);
    restic_free_command_output(&out);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE && stop.lines == 100, "A consumer error stops the child at once");

    /* A streamed line without end (17 MiB without a line break) fails instead of filling memory */
    WCHAR fake[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");
    LineCount endless = {0, 1, 0};
    opts.line_ctx = &endless;
    res = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/longline", &opts, &out);
    restic_free_command_output(&out);
    EXPECT_TRUE(res == RESTIC_ERR_JSON_PARSE && endless.lines == 0, "A streamed line over 16 MiB fails the command");
}

/* ============================================================================================ */
/* Live integration test against a real restic.exe and a self-built repository                  */
/* ============================================================================================ */

/*
 * Lists `subpath` both with restic and from the whole-snapshot index and requires identical
 * entries, then does the same for every subfolder. Returns the number of folders compared, or -1
 * on the first difference.
 */
static int compare_index_with_restic(const WCHAR* restic_bin, const WCHAR* repo, const WCHAR* password,
                                     const char* snap, const SnapshotIndex* index, const WCHAR* subpath) {
    WIN32_FIND_DATAW *a = NULL, *b = NULL;
    size_t na = 0, nb = 0;
    WCHAR err[512];
    ResticResult ra = restic_list_directory(restic_bin, REPO(repo, password), snap, subpath, NULL, &a, &na, NULL, err, _countof(err));
    ResticResult rb = restic_list_index_directory(index, subpath, &b, &nb);
    int folders = 1;
    if (ra != RESTIC_OK || rb != RESTIC_OK || na != nb || (na && memcmp(a, b, na * sizeof(*a)) != 0)) {
        printf("       differs: %ls (%d/%d, %u/%u entries)\n", subpath, (int)ra, (int)rb, (unsigned)na, (unsigned)nb);
        folders = -1;
    }
    for (size_t i = 0; folders > 0 && i < na; ++i) {
        if (!is_dir_entry(&a[i])) continue;
        static WCHAR child[64][1024]; /* One per depth level: static, /analyze caps a frame at 16 KB */
        static int depth = 0;
        if (depth >= 64) break;
        swprintf_s(child[depth], 1024, L"%ls\\%ls", subpath, a[i].cFileName);
        depth++;
        int sub = compare_index_with_restic(restic_bin, repo, password, snap, index, child[depth - 1]);
        depth--;
        folders = sub < 0 ? -1 : folders + sub;
    }
    free(a);
    free(b);
    return folders;
}

static void remove_tree(const WCHAR* dir) {
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    swprintf_s(pattern, MAX_PATH, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            WCHAR child[MAX_PATH];
            swprintf_s(child, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); /* Restic marks pack files read-only */
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static BOOL write_file(const WCHAR* dir, const WCHAR* name, DWORD size, const FILETIME* mtime) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char chunk[1024];
    memset(chunk, 'x', sizeof(chunk));
    for (DWORD left = size; left > 0;) {
        DWORD n = left < sizeof(chunk) ? left : (DWORD)sizeof(chunk), w = 0;
        if (!WriteFile(h, chunk, n, &w, NULL) || w != n) {
            CloseHandle(h);
            return FALSE;
        }
        left -= n;
    }
    if (mtime) SetFileTime(h, NULL, NULL, mtime);
    CloseHandle(h);
    return TRUE;
}

/* Deterministic binary payload containing 0x00, 0x0A, 0x0D and 0x1A: exposes any text-mode translation */
static unsigned char pattern_byte(unsigned long i) {
    return (unsigned char)((i * 7u + i / 251u) & 0xFFu);
}

static BOOL write_pattern(const WCHAR* dir, const WCHAR* name, DWORD size) {
    WCHAR path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BOOL ok = TRUE;
    for (DWORD i = 0; ok && i < size; ++i) {
        unsigned char b = pattern_byte(i);
        DWORD w = 0;
        ok = WriteFile(h, &b, 1, &w, NULL) && w == 1;
    }
    CloseHandle(h);
    return ok;
}

/* TRUE when the file holds exactly `size` bytes of the deterministic pattern */
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

/* TRUE when the file content equals `text` exactly */
static BOOL file_equals(const WCHAR* path, const char* text) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[256] = {0};
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got == strlen(text) &&
              memcmp(buf, text, got) == 0;
    CloseHandle(h);
    return ok;
}

/* Number of leftover "~rp*.tmp" temporary files in dir */
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

static BOOL make_dir(const WCHAR* parent, const WCHAR* name, WCHAR* out_path) {
    swprintf_s(out_path, MAX_PATH, L"%ls\\%ls", parent, name);
    return CreateDirectoryW(out_path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Builds the fixture tree backed up by the live test. Returns FALSE on any I/O failure. */
static BOOL create_fixture(const WCHAR* data) {
    SYSTEMTIME st = {2020, 2, 0, 29, 12, 34, 56, 0};
    FILETIME fixed_mtime;
    SystemTimeToFileTime(&st, &fixed_mtime);
    WCHAR sub[MAX_PATH], dir[MAX_PATH];

    BOOL ok = write_file(data, L"readme.txt", 5, &fixed_mtime) &&
              write_file(data, L"with space.txt", 5, NULL) &&
              write_file(data, L"\u00fcn\u00efc\u00f6d\u00e9_\u65e5\u672c\u8a9e.txt", 7, NULL) &&
              write_file(data, L"zero.bin", 0, NULL) &&
              write_file(data, L"big.bin", 100000, NULL) &&
              make_dir(data, L"sub", sub) && write_file(sub, L"inner.txt", 3, NULL) &&
              make_dir(sub, L"sub2", dir) && write_file(dir, L"deep.txt", 4, NULL) &&
              write_pattern(dir, L"binary.dat", 70000) &&
              make_dir(data, L"empty_dir", dir) &&
              make_dir(data, L"odd [dir] & (x) ^ 100% !", sub) && write_file(sub, L"odd_inner.txt", 9, NULL) &&
              make_dir(data, L"many", dir);
    for (int i = 1; ok && i <= 300; ++i) {
        WCHAR name[128];
        swprintf_s(name, _countof(name), L"file_with_a_reasonably_long_name_to_grow_the_buffer_%04d.txt", i);
        ok = write_file(dir, name, 1, NULL);
    }
    return ok;
}

/* "C:\a\b" -> "\C\a\b": how a Windows backup path appears below the snapshot root */
static void to_wfx_path(const WCHAR* abs_path, const WCHAR* leaf, WCHAR* out, size_t cap) {
    if (leaf && leaf[0] != L'\0') {
        swprintf_s(out, cap, L"\\%c%ls\\%ls", abs_path[0], abs_path + 2, leaf);
    } else {
        swprintf_s(out, cap, L"\\%c%ls", abs_path[0], abs_path + 2);
    }
}

static ULONGLONG g_last_dump_bytes = 0;
static BOOL      g_abort_dump      = FALSE;

static BOOL dump_progress_cb(ULONGLONG bytes_written, void* user_data) {
    (void)user_data;
    g_last_dump_bytes = bytes_written;
    return g_abort_dump && (bytes_written > 0);
}

static void test_live_restic_cli(void) {
    WCHAR restic_bin[MAX_PATH], base[MAX_PATH], data[MAX_PATH], repo[MAX_PATH];
    GetFullPathNameW(L"tmp\\restic.exe", MAX_PATH, restic_bin, NULL);
    if (GetFileAttributesW(restic_bin) == INVALID_FILE_ATTRIBUTES) {
        printf("[SKIP] tmp\\restic.exe not found: live Restic integration tests skipped.\n");
        return;
    }

    /* Fresh, deterministic fixture on every run (left on disk afterwards for inspection) */
    GetFullPathNameW(L"tmp\\test_fixture", MAX_PATH, base, NULL);
    remove_tree(base);
    EXPECT_TRUE(CreateDirectoryW(base, NULL), "Create fixture base directory");
    EXPECT_TRUE(make_dir(base, L"data", data) && make_dir(base, L"repo", repo), "Create data/repo directories");
    EXPECT_TRUE(create_fixture(data), "Create fixture files");

    const WCHAR* password = L"p@ss \"w0rd\" \u00fc %PATH% \u65e5\u672c";
    WCHAR err[512] = {0};
    WIN32_FIND_DATAW* items = NULL;
    size_t count = 0;
    ResticResult res;
    ResticCommandOutput out;

    unsigned live_version = 0;
    res = restic_read_version(restic_bin, NULL, &live_version, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && live_version >= RESTIC_MIN_VERSION, "restic version read; the live restic is recent enough");

    /* Error mapping before the repository exists */
    SnapshotTree* tree = NULL;
    res = restic_list_snapshots(restic_bin, REPO(repo, password), NULL, NULL, &tree, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND && tree == NULL, "Missing repository -> RESTIC_ERR_REPO_NOT_FOUND");
    EXPECT_TRUE(err[0] != L'\0', "Missing repository reports a reason");

    WCHAR* q_data = restic_quote_arg(data);
    WCHAR args[2048];
    char repo_id[RESTIC_REPOSITORY_ID_CHARS + 1];
    res = restic_read_repository_id(restic_bin, REPO(repo, password), NULL, repo_id, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND && repo_id[0] == '\0', "Repository ID of a missing repository");

    res = restic_exec_command(restic_bin, REPO(repo, password), L"init --json --repository-version 2", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "restic init with a password containing quotes, %PATH% and CJK text");
    /* {"message_type":"initialized","id":"<64 hex>",...} */
    char init_id[RESTIC_REPOSITORY_ID_CHARS + 1] = {0};
    const char* id_at = out.output ? strstr(out.output, "\"id\":\"") : NULL;
    if (id_at) memcpy(init_id, id_at + 6, RESTIC_REPOSITORY_ID_CHARS);
    restic_free_command_output(&out);

    res = restic_read_repository_id(restic_bin, REPO(repo, password), NULL, repo_id, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && strlen(repo_id) == RESTIC_REPOSITORY_ID_CHARS && strcmp(repo_id, init_id) == 0,
                "restic cat config gives the ID restic init reported");
    res = restic_read_repository_id(restic_bin, REPO(repo, L"wrong password"), NULL, repo_id, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD && repo_id[0] == '\0', "Repository ID with a wrong password");

    res = restic_list_snapshots(restic_bin, REPO(repo, L"wrong password"), NULL, NULL, &tree, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Wrong password -> RESTIC_ERR_WRONG_PASSWORD");
    EXPECT_TRUE(wcsstr(err, L"password") != NULL, "Wrong password reason mentions the password");

    res = restic_list_snapshots(restic_bin, REPO(repo, password), NULL, NULL, &tree, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && snapshot_tree_snapshot_count(tree) == 0, "Empty repository -> no snapshots");
    EXPECT_TRUE(snapshot_tree_root(tree)->child_count == 4, "...but the four restic mount folders");
    snapshot_tree_free(tree);

    /* Two snapshots, the second after changing one file */
    swprintf_s(args, _countof(args), L"backup %ls --json --quiet", q_data);
    res = restic_exec_command(restic_bin, REPO(repo, password), args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "First backup");
    restic_free_command_output(&out);
    EXPECT_TRUE(write_file(data, L"with space.txt", 12, NULL), "Modify fixture");
    res = restic_exec_command(restic_bin, REPO(repo, password), args, NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK, "Second backup");
    restic_free_command_output(&out);
    free(q_data);

    res = restic_list_snapshots(restic_bin, REPO(repo, password), NULL, NULL, &tree, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && snapshot_tree_snapshot_count(tree) == 2, "Two snapshots listed");
    const SnapshotNode* by_time = snapshot_tree_child(snapshot_tree_root(tree), "snapshots");
    EXPECT_TRUE(by_time && by_time->child_count == 5 && strcmp(by_time->children[2]->name, "latest") == 0,
                "snapshots/ holds both snapshots and latest");
    const SnapshotNode* latest = by_time->children[2];
    EXPECT_TRUE(latest->snapshot_id && strlen(latest->snapshot_id) == 64, "Full snapshot IDs");
    char snap1[65] = {0}, snap2[65] = {0};
    /* The second snapshot is latest; the first is whichever folder is not */
    strcpy_s(snap2, sizeof(snap2), latest->snapshot_id);
    strcpy_s(snap1, sizeof(snap1), strcmp(by_time->children[0]->snapshot_id, snap2) != 0 ? by_time->children[0]->snapshot_id
                                                                                       : by_time->children[1]->snapshot_id);
    EXPECT_TRUE(strcmp(snap1, snap2) != 0, "Two different snapshots");
    static char host[256]; /* Static: /analyze (C6262) caps a frame at 16 KB */
    const char* snap_host = snapshot_tree_snapshot_host(tree, snap2);
    EXPECT_TRUE(snap_host && snap_host[0] && strlen(snap_host) < sizeof(host), "The snapshots' host");
    if (snap_host) strncpy_s(host, sizeof(host), snap_host, _TRUNCATE);
    snapshot_tree_free(tree);

    /* Level 2: root of the snapshot holds only the drive letter */
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, L"\\", NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 1, "Snapshot root contains one entry");
    EXPECT_TRUE(is_dir_entry(&items[0]) && _wcsnicmp(items[0].cFileName, data, 1) == 0 && items[0].cFileName[1] == L'\0',
                "Root entry is the drive letter directory");
    free(items);

    WCHAR wfx[MAX_PATH * 2];
    to_wfx_path(data, NULL, wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "List fixture directory (snapshot 2)");
    EXPECT_TRUE(count == 9, "Nine direct children (nothing from deeper levels)");
    const WIN32_FIND_DATAW* e = find_entry(items, count, L"readme.txt");
    EXPECT_TRUE(e && !is_dir_entry(e) && file_size_of(e) == 5, "readme.txt size");
    FILETIME expected;
    restic_parse_iso8601_time("2020-02-29T12:34:56Z", &expected);
    EXPECT_TRUE(e && ft_u64(e->ftLastWriteTime) == ft_u64(expected), "mtime survives the local-offset -> UTC conversion");
    e = find_entry(items, count, L"with space.txt");
    EXPECT_TRUE(e && file_size_of(e) == 12, "Snapshot 2 shows the modified size");
    e = find_entry(items, count, L"\u00fcn\u00efc\u00f6d\u00e9_\u65e5\u672c\u8a9e.txt");
    EXPECT_TRUE(e && file_size_of(e) == 7, "Unicode file name");
    e = find_entry(items, count, L"zero.bin");
    EXPECT_TRUE(e && file_size_of(e) == 0, "Zero-byte file");
    e = find_entry(items, count, L"big.bin");
    EXPECT_TRUE(e && file_size_of(e) == 100000, "big.bin size");
    EXPECT_TRUE((e = find_entry(items, count, L"sub")) != NULL && is_dir_entry(e), "Subdirectory");
    EXPECT_TRUE((e = find_entry(items, count, L"empty_dir")) != NULL && is_dir_entry(e), "Empty subdirectory");
    EXPECT_TRUE((e = find_entry(items, count, L"many")) != NULL && is_dir_entry(e), "Directory with many files");
    EXPECT_TRUE((e = find_entry(items, count, L"odd [dir] & (x) ^ 100% !")) != NULL && is_dir_entry(e),
                "Directory name with shell/glob metacharacters");
    free(items);

    /* The older snapshot still reflects the original content */
    res = restic_list_directory(restic_bin, REPO(repo, password), snap1, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 9, "List fixture directory (snapshot 1)");
    e = find_entry(items, count, L"with space.txt");
    EXPECT_TRUE(e && file_size_of(e) == 5, "Snapshot 1 keeps the original size");
    free(items);

    /* The versions of a file: restic find for exactly its path, in the snapshots of one host */
    static const struct {
        const WCHAR* leaf;
        const char* in_snapshot; /* NULL: snap2 */
        const char* host;        /* NULL: the snapshots' host */
        FileHistoryStatus status;
        size_t versions;
        const char* what;
    } finds[] = {
        {L"with space.txt", NULL, NULL, FILE_HISTORY_OK, 2, "The modified file has two versions"},
        {L"readme.txt", NULL, NULL, FILE_HISTORY_OK, 1, "An unchanged file has one, in both snapshots"},
        {L"odd [dir] & (x) ^ 100% !\\odd_inner.txt", NULL, NULL, FILE_HISTORY_OK, 1,
         "A path with wildcard characters is matched literally"},
        {L"sub", NULL, NULL, FILE_HISTORY_NOT_A_FILE, 0, "A folder has no versions (what lies below it is ignored)"},
        {L"no such file", NULL, NULL, FILE_HISTORY_NOT_A_FILE, 0, "A missing file has none"},
        {L"readme.txt", NULL, "no-such-host", FILE_HISTORY_NOT_A_FILE, 0, "Only the snapshots of the host searched"},
        {L"readme.txt", NULL, "", FILE_HISTORY_OK, 1, "An empty host: every host"},
    };
    for (size_t i = 0; i < _countof(finds); ++i) {
        char* found      = NULL;
        size_t found_len = 0;
        static char path_utf8[1024];
        to_wfx_path(data, finds[i].leaf, wfx, _countof(wfx));
        res = restic_find_file(restic_bin, REPO(repo, password), finds[i].host ? finds[i].host : host, wfx, NULL,
                               &found, &found_len, err, _countof(err));
        FileHistory* history = NULL;
        FileHistoryStatus st = FILE_HISTORY_BAD_JSON;
        if (res == RESTIC_OK && restic_normalize_filter_path(wfx, path_utf8, sizeof(path_utf8))) {
            st = file_history_build(found, found_len, path_utf8, snap2, NULL, &history);
        }
        EXPECT_TRUE(res == RESTIC_OK && st == finds[i].status && file_history_count(history) == finds[i].versions,
                    finds[i].what);
        if (i == 0 && history) {
            const FileVersion* old = file_history_version(history, 0);
            const FileVersion* now = file_history_version(history, 1);
            EXPECT_TRUE(old && now && old->size == 5 && strcmp(old->snapshot_id, snap1) == 0 && now->size == 12 &&
                            strcmp(now->snapshot_id, snap2) == 0 && strstr(now->name, " with space.txt") != NULL,
                        "...the older from snapshot 1, the newer from snapshot 2, named by their times");
        }
        if (i == 1 && history) {
            EXPECT_TRUE(file_history_version(history, 0)->snapshot_count == 2, "...held by both snapshots");
        }
        file_history_free(history);
        free(found);
    }
    char* nothing = NULL;
    size_t nothing_len = 0;
    EXPECT_TRUE(restic_find_file(restic_bin, REPO(repo, password), host, L"\\", NULL, &nothing, &nothing_len, err,
                                 _countof(err)) == RESTIC_ERR_INVALID_ARG && !nothing,
                "The snapshot root is no file");
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    EXPECT_TRUE(restic_find_file(restic_bin, REPO(repo, password), NULL, wfx, NULL, &nothing, &nothing_len, err,
                                 _countof(err)) == RESTIC_ERR_INVALID_ARG && !nothing,
                "No host: refused, not every host");
    EXPECT_TRUE(restic_find_file(restic_bin, REPO(repo, L"wrong password"), host, wfx, NULL, &nothing, &nothing_len,
                                 err, _countof(err)) == RESTIC_ERR_WRONG_PASSWORD && !nothing,
                "Wrong password reported as for any other command");

    /* The whole snapshot read at once (restic ls without a path) lists every folder as restic does */
    SnapshotIndex* index = snapshot_index_new(0);
    SnapshotIndexStatus index_status = SNAPSHOT_INDEX_BAD_JSON;
    res = restic_list_snapshot_index(restic_bin, REPO(repo, password), snap2, NULL, index, &index_status, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && index_status == SNAPSHOT_INDEX_OK && snapshot_index_entry_count(index) > 300,
                "Whole snapshot read into an index");
    int compared = compare_index_with_restic(restic_bin, repo, password, snap2, index, L"\\");
    EXPECT_TRUE(compared > 8, "Every folder of the snapshot lists identically from the index and from restic");
    WIN32_FIND_DATAW* none = NULL;
    size_t none_count = 0;
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    EXPECT_TRUE(restic_list_index_directory(index, wfx, &none, &none_count) == RESTIC_ERR_PATH_NOT_FOUND &&
                restic_list_index_directory(index, L"\\no\\such", &none, &none_count) == RESTIC_ERR_PATH_NOT_FOUND &&
                none == NULL, "A file or a missing path is not a folder of the index either");
    snapshot_index_free(index);

    index = snapshot_index_new(4096);
    res = restic_list_snapshot_index(restic_bin, REPO(repo, password), snap2, NULL, index, &index_status, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_OUT_OF_MEMORY && index_status == SNAPSHOT_INDEX_TOO_LARGE,
                "An index over its memory limit stops restic and says so");
    snapshot_index_free(index);
    index = snapshot_index_new(0);
    res = restic_list_snapshot_index(restic_bin, REPO(repo, L"wrong password"), snap2, NULL, index, &index_status, err,
                                     _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Wrong password reported as for any other command");
    EXPECT_TRUE(restic_list_snapshot_index(restic_bin, REPO(repo, password), "--help", NULL, index, &index_status, err,
                                           _countof(err)) == RESTIC_ERR_INVALID_ARG, "Non-hex snapshot ID refused");
    snapshot_index_free(index);

    /* Background loads: one at a time, and the snapshot being read makes room within SnapshotCacheMB */
    WCHAR live_repo[MAX_PATH];
    wcscpy_s(live_repo, MAX_PATH, repo);
    static ResticRepoEntry live, live2;
    live_entry(&live, L"Live", live_repo);
    live_entry(&live2, L"Live2", live_repo);
    repo_cache_clear();
    repo_cache_set_index_limit(64u * 1024u * 1024u);
    repo_cache_load_index(&live, restic_bin, snap1, fixture_password);
    repo_cache_load_index(&live, restic_bin, snap2, fixture_password); /* Not while snap1 loads */
    EXPECT_TRUE(repo_cache_wait_idle(120000), "First background load finishes");
    const SnapshotIndex* one = repo_cache_lock_index(L"Live", live_repo, snap1);
    size_t one_bytes = one ? snapshot_index_memory(one) : 0;
    if (one) repo_cache_unlock();
    const SnapshotIndex* two = repo_cache_lock_index(L"Live", live_repo, snap2);
    if (two) repo_cache_unlock();
    EXPECT_TRUE(one_bytes > 0 && two == NULL, "Snapshot 1 read; snapshot 2 was not started while it loaded");
    repo_cache_set_index_limit(one_bytes + one_bytes / 2); /* Room for one listing, not for two */
    repo_cache_load_index(&live, restic_bin, snap2, fixture_password);
    EXPECT_TRUE(repo_cache_wait_idle(120000), "Second background load finishes");
    one = repo_cache_lock_index(L"Live", live_repo, snap1);
    if (one) repo_cache_unlock();
    two = repo_cache_lock_index(L"Live", live_repo, snap2);
    if (two) repo_cache_unlock();
    EXPECT_TRUE(one == NULL && two != NULL, "Reading snapshot 2 dropped snapshot 1 to stay within the limit");
    repo_cache_clear();
    /* Two repositories (here two aliases of the same one) are read side by side */
    repo_cache_set_index_limit(64u * 1024u * 1024u);
    repo_cache_load_index(&live, restic_bin, snap1, fixture_password);
    repo_cache_load_index(&live2, restic_bin, snap2, fixture_password);
    EXPECT_TRUE(repo_cache_wait_idle(120000), "Both background loads finish");
    one = repo_cache_lock_index(L"Live", live_repo, snap1);
    if (one) repo_cache_unlock();
    two = repo_cache_lock_index(L"Live2", live_repo, snap2);
    if (two) repo_cache_unlock();
    EXPECT_TRUE(one != NULL && two != NULL, "Different repositories are read side by side");
    repo_cache_clear();
    repo_cache_set_index_limit(512u * 1024u * 1024u);

    /* Output larger than the initial 4 KB / typical buffers: 300 NDJSON lines */
    to_wfx_path(data, L"many", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 300, "300 entries (~100 KB of NDJSON) listed");
    EXPECT_TRUE(find_entry(items, count, L"file_with_a_reasonably_long_name_to_grow_the_buffer_0300.txt") != NULL,
                "Last entry present");
    free(items);

    /* Names containing metacharacters travel through the command line intact */
    to_wfx_path(data, L"odd [dir] & (x) ^ 100% !", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 1 && wcscmp(items[0].cFileName, L"odd_inner.txt") == 0,
                "Directory with metacharacters lists its content");
    free(items);

    to_wfx_path(data, L"empty_dir", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && count == 0, "Existing empty directory is not an error");

    /* Missing paths, files used as directories, quote injection attempts */
    BOOL not_a_folder = TRUE;
    to_wfx_path(data, L"does_not_exist", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, &not_a_folder, err,
                                _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && items == NULL && !not_a_folder,
                "Missing directory -> RESTIC_ERR_PATH_NOT_FOUND, nothing there");
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, &not_a_folder, err,
                                _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && not_a_folder,
                "File used as directory -> RESTIC_ERR_PATH_NOT_FOUND, but a file is there");
    to_wfx_path(data, L"no\"such\" --help \"dir", wfx, _countof(wfx));
    res = restic_list_directory(restic_bin, REPO(repo, password), snap2, wfx, NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "Embedded quotes cannot inject extra arguments");

    /* Snapshot ID handling */
    res = restic_list_directory(restic_bin, REPO(repo, password), "deadbeef", L"\\", NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && wcsstr(err, L"deadbeef") != NULL, "Unknown snapshot ID reports Restic's reason");
    res = restic_list_directory(restic_bin, REPO(repo, password), "--help", L"\\", NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Option-like snapshot ID rejected before spawning");
    res = restic_list_directory(restic_bin, REPO(repo, password), "abc def", L"\\", NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Snapshot ID with whitespace rejected");
    res = restic_list_directory(restic_bin, REPO(repo, password), NULL, L"\\", NULL, &items, &count, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL snapshot ID rejected");

    /* File extraction via restic_dump_file */
    WCHAR dump_dest[MAX_PATH];
    swprintf_s(dump_dest, _countof(dump_dest), L"%ls\\dumped_file.bin", base);
    DeleteFileW(dump_dest);

    /* 1. Missing snapshot file or wrong password never creates the local file */
    to_wfx_path(data, L"no_such_file.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "Missing file in snapshot -> RESTIC_ERR_PATH_NOT_FOUND");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Local file not created when remote path is missing");

    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, L"wrong_pwd"), snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Wrong password on dump -> RESTIC_ERR_WRONG_PASSWORD");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Local file not created when password fails");

    /* 2. Extract readme.txt (5 bytes) and preserve mtime */
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, FALSE, &expected, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "Dump readme.txt succeeds");
    HANDLE hf = CreateFileW(dump_dest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(hf != INVALID_HANDLE_VALUE, "Extracted readme.txt exists");
    char buf[16] = {0};
    DWORD got = 0;
    FILETIME actual_ft = {0};
    BOOL read_ok = ReadFile(hf, buf, 10, &got, NULL);
    GetFileTime(hf, NULL, NULL, &actual_ft);
    CloseHandle(hf);
    EXPECT_TRUE(read_ok && got == 5 && memcmp(buf, "xxxxx", 5) == 0, "Extracted content matches readme.txt");
    EXPECT_TRUE(ft_u64(actual_ft) == ft_u64(expected), "Extracted file preserves snapshot LastWriteTime");

    /* 3. overwrite = FALSE refuses to clobber an existing file; overwrite = TRUE succeeds even on READONLY */
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Refuses to overwrite when overwrite is FALSE");
    SetFileAttributesW(dump_dest, FILE_ATTRIBUTE_READONLY);
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_last_dump_bytes = 0;
    g_abort_dump      = FALSE;
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK, "Streaming 100 KB file over multi-chunk pipe succeeds");
    WIN32_FILE_ATTRIBUTE_DATA big_fad;
    EXPECT_TRUE(GetFileAttributesExW(dump_dest, GetFileExInfoStandard, &big_fad) &&
                big_fad.nFileSizeLow == 100000 && big_fad.nFileSizeHigh == 0 &&
                (big_fad.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0,
                "100 KB file is complete and the READONLY target was replaced");
    EXPECT_TRUE(g_last_dump_bytes > 0 && g_last_dump_bytes <= 100000ULL, "Progress callback reported a plausible byte count");

    /* 4. Zero-byte file extraction */
    DeleteFileW(dump_dest);
    to_wfx_path(data, L"zero.bin", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(res == RESTIC_OK && GetFileAttributesExW(dump_dest, GetFileExInfoStandard, &fad) &&
                fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0, "Zero-byte file created on disk");

    /* 5. User abort during transfer leaves no local file (the target did not exist before) */
    DeleteFileW(dump_dest);
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_abort_dump = TRUE;
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    g_abort_dump = FALSE;
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "Dump cancelled via progress callback");
    EXPECT_TRUE(GetFileAttributesW(dump_dest) == INVALID_FILE_ATTRIBUTES, "Incomplete local file removed on abort");
    EXPECT_TRUE(count_partial_files(base) == 0, "No temporary ~rp*.tmp file is left behind after an abort");

    /* 6. Binary fidelity: 0x00/0x0A/0x0D/0x1A bytes survive the pipe byte for byte (no text-mode translation) */
    to_wfx_path(data, L"sub\\sub2\\binary.dat", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && verify_pattern_file(dump_dest, 70000), "Binary file extracted byte for byte");

    /* 7. A failed overwrite never destroys the existing target (data goes to a temporary file first) */
    to_wfx_path(data, L"no_such_file.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND && verify_pattern_file(dump_dest, 70000),
                "Missing remote file keeps the existing target intact");
    to_wfx_path(data, L"big.bin", wfx, _countof(wfx));
    g_abort_dump = TRUE;
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, dump_progress_cb, NULL, err, _countof(err));
    g_abort_dump = FALSE;
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED && verify_pattern_file(dump_dest, 70000) && count_partial_files(base) == 0,
                "Cancelled overwrite keeps the existing target intact and removes the temporary file");
    res = restic_dump_file(restic_bin, REPO(repo, L"wrong_pwd"), snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD && verify_pattern_file(dump_dest, 70000),
                "Wrong password keeps the existing target intact");

    /* 8. Local-side failures */
    to_wfx_path(data, L"readme.txt", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, dump_dest, FALSE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE && verify_pattern_file(dump_dest, 70000),
                "overwrite=FALSE on an existing target fails before touching it");
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, base, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "A directory as the local target is refused");
    WCHAR bad_dest[MAX_PATH];
    swprintf_s(bad_dest, _countof(bad_dest), L"%ls\\no_such_dir\\out.bin", base);
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, bad_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Non-existent local directory -> RESTIC_ERR_LOCAL_WRITE (non-empty file)");
    to_wfx_path(data, L"zero.bin", wfx, _countof(wfx));
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, bad_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_LOCAL_WRITE, "Non-existent local directory -> RESTIC_ERR_LOCAL_WRITE (zero-byte file)");
    EXPECT_TRUE(count_partial_files(base) == 0, "No temporary file after local failures");

    /* 9. Argument validation (nothing is spawned) */
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, L"\\", dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Dumping the snapshot root is refused");
    res = restic_dump_file(restic_bin, REPO(repo, password), "--help", wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Option-like snapshot ID refused");
    res = restic_dump_file(restic_bin, REPO(repo, password), NULL, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "NULL snapshot ID refused");
    res = restic_dump_file(restic_bin, REPO(repo, password), snap2, wfx, L"", TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_INVALID_ARG, "Empty local path refused");
    res = restic_dump_file(L"restic.exe", REPO(repo, password), snap2, wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_SPAWN_FAILED, "Bare executable name refused (cwd is never searched)");
    res = restic_dump_file(restic_bin, REPO(repo, password), "deadbeef", wfx, dump_dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && verify_pattern_file(dump_dest, 70000),
                "Unknown snapshot -> CLI failure, target intact");
    DeleteFileW(dump_dest);
}

/*
 * restic_dump_file against fake_restic.exe (src/tests/fake_restic.c, built next to this test):
 * deterministic coverage of exit codes, stderr classification, partial output followed by failure,
 * environment injection and the INACTIVITY timeout, none of which can be provoked reliably with the
 * real binary. The scenario is selected by the last command-line argument (the snapshot path).
 */
static void test_dump_with_fake_restic(void) {
    WCHAR dir[MAX_PATH], fake[MAX_PATH], dest[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");
    EXPECT_TRUE(GetFileAttributesW(fake) != INVALID_FILE_ATTRIBUTES, "fake_restic.exe is built next to the test");
    GetTempPathW(MAX_PATH, dir);
    swprintf_s(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"restic_fake_%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    swprintf_s(dest, MAX_PATH, L"%ls\\out.bin", dir);

    WCHAR err[512];
    ResticResult res;
    DeleteFileW(dest);
#define FAKE_DUMP(path, overwrite, timeout) \
    restic_dump_file(fake, REPO(L"repo-url", L"s3cret"), "abcd1234", path, dest, overwrite, NULL, timeout, NULL, NULL, err, _countof(err))

    res = FAKE_DUMP(L"\\ok", FALSE, 0);
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "hello"), "Fake restic: stdout bytes are stored verbatim");
    EXPECT_TRUE(count_partial_files(dir) == 0, "No temporary file after success");

    res = FAKE_DUMP(L"\\env", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "repo-url|s3cret"),
                "RESTIC_REPOSITORY and RESTIC_PASSWORD reach the child through the environment");

    res = FAKE_DUMP(L"\\empty", TRUE, 0);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    EXPECT_TRUE(res == RESTIC_OK && GetFileAttributesExW(dest, GetFileExInfoStandard, &fad) && fad.nFileSizeLow == 0,
                "Exit code 0 without output replaces the target with an empty file");

    /* Failures after partial output must keep the previous target and leave nothing behind */
    DeleteFileW(dest);
    EXPECT_TRUE(FAKE_DUMP(L"\\ok", FALSE, 0) == RESTIC_OK, "Re-create target");
    res = FAKE_DUMP(L"\\partial", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && wcsstr(err, L"boom") != NULL,
                "Partial output + exit code 1 -> RESTIC_ERR_CLI_FAILED with Restic's message");
    EXPECT_TRUE(file_equals(dest, "hello") && count_partial_files(dir) == 0,
                "Partial data never replaces the target and the temporary file is removed");
    DeleteFileW(dest);
    res = FAKE_DUMP(L"\\partial", FALSE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES,
                "Failed extraction leaves no target file at all");

    res = FAKE_DUMP(L"\\missing", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_PATH_NOT_FOUND, "'not found in snapshot' on stderr -> RESTIC_ERR_PATH_NOT_FOUND");
    res = FAKE_DUMP(L"\\silent", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED && err[0] == L'\0', "Exit code 1 without any output is handled (empty reason)");
    res = FAKE_DUMP(L"\\norepo", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_NOT_FOUND && err[0] != L'\0', "Exit code 10 -> RESTIC_ERR_REPO_NOT_FOUND");
    res = FAKE_DUMP(L"\\locked", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_REPO_LOCKED, "Exit code 11 -> RESTIC_ERR_REPO_LOCKED");
    res = FAKE_DUMP(L"\\badpw", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_WRONG_PASSWORD, "Exit code 12 -> RESTIC_ERR_WRONG_PASSWORD");
    res = FAKE_DUMP(L"\\unknown", TRUE, 0);
    EXPECT_TRUE(res == RESTIC_ERR_CLI_FAILED, "Other exit codes -> RESTIC_ERR_CLI_FAILED");

    /* Inactivity timeout: silence is punished, steady progress is not */
    ULONGLONG t0 = GetTickCount64();
    res = FAKE_DUMP(L"\\hang", TRUE, 700);
    ULONGLONG hang_ms = GetTickCount64() - t0;
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT && hang_ms < 5000, "Silent child is killed by the inactivity timeout");
    t0 = GetTickCount64();
    res = FAKE_DUMP(L"\\slow", TRUE, 1800);
    ULONGLONG slow_ms = GetTickCount64() - t0;
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "abc"), "Child that keeps producing bytes survives the inactivity timeout");
    EXPECT_TRUE(slow_ms > 1800, "The whole transfer lasted longer than the timeout value (it is not a total-time limit)");
#undef FAKE_DUMP

    /*
     * Extended options go before the command, one -o each, one argument each; restic reads an -o
     * value as CSV, so one with a quote or a comma is a quoted CSV field
     */
    ResticRepo with_options = {.target = L"repo-url", .password = L"pw",
                               .options = L"s3.region=eu-west-1\0sftp.command=\"C:\\x y\\ssh.exe\" -s sftp\0"
                                          L"s3.storage-class=a,b\0"};
    ResticCommandOutput out;
    res = restic_exec_command(fake, &with_options, L"snapshots /args", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && strcmp(out.output, "-o|s3.region=eu-west-1|-o|\"sftp.command=\"\"C:\\x y\\ssh.exe\"\" -s sftp\"|"
                                                       "-o|\"s3.storage-class=a,b\"|snapshots|/args|") == 0,
                "Options as -o arguments before the command, each one argument, a CSV field where restic needs one");
    restic_free_command_output(&out);
    res = restic_dump_file(fake, &with_options, "abcd1234", L"\\args", dest, TRUE, NULL, 0, NULL, NULL, err, _countof(err));
    EXPECT_TRUE(res == RESTIC_OK && file_equals(dest, "-o|s3.region=eu-west-1|-o|\"sftp.command=\"\"C:\\x y\\ssh.exe\"\" -s sftp\"|"
                                                      "-o|\"s3.storage-class=a,b\"|dump|--no-lock|abcd1234|/args|"),
                "restic dump gets the options too");
    res = restic_exec_command(fake, REPO(L"repo-url", L"pw"), L"snapshots /args", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && strcmp(out.output, "snapshots|/args|") == 0, "No options: nothing added");
    restic_free_command_output(&out);
    ResticRepo empty_option = {.target = L"repo-url", .password = L"pw", .options = L"rest.connections=\0s3.region=x\0"};
    res = restic_exec_command(fake, &empty_option, L"snapshots /args", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && strcmp(out.output, "-o|s3.region=x|snapshots|/args|") == 0,
                "An option written empty in the INI is not passed");
    restic_free_command_output(&out);

    DeleteFileW(dest);
    RemoveDirectoryW(dir);
}

/* TRUE when the process with the ID printed at the start of `text` has ended (or never existed) */
static BOOL printed_process_ended(const char* text) {
    unsigned long pid = text ? strtoul(text, NULL, 10) : 0;
    if (pid == 0) return FALSE;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return TRUE;
    BOOL ended = WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return ended;
}

/*
 * Processes restic starts (fake_restic's /child-* modes start a helper and print its ID) end with
 * it: on a timeout or ESC together with restic, after a normal exit when restic leaves them behind
 */
static void test_process_tree_ends(void) {
    WCHAR fake[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");

    ResticExecOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = 3000;
    ResticCommandOutput out;
    ResticResult res = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/child-hang", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_TIMEOUT && out.output && out.output[0] >= '1' && out.output[0] <= '9',
                "A hanging restic that started a helper times out");
    EXPECT_TRUE(printed_process_ended(out.output), "The helper is ended together with restic");
    restic_free_command_output(&out);

    memset(&opts, 0, sizeof(opts));
    opts.cancel_proc = cancel_after_first_poll;
    res = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/child-hang", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CANCELLED, "ESC ends a restic that started a helper");
    restic_free_command_output(&out);

    res = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/child-exit", NULL, &out);
    EXPECT_TRUE(res == RESTIC_OK && printed_process_ended(out.output),
                "A helper restic leaves behind is ended when restic has exited");
    restic_free_command_output(&out);
}

/* ResticNoticeProc of test_refused_credentials: counts the lines and keeps the last */
static int  g_notices = 0;
static char g_last_notice[1024];
static void count_notice(const char* line, void* ctx) {
    (void)ctx;
    g_notices++;
    strncpy_s(g_last_notice, sizeof(g_last_notice), line, _TRUNCATE);
}

/*
 * restic retries a storage that refuses the credentials for 15 minutes; its "retrying" line with
 * such a reason ends it at once (fake_restic.exe /retry-auth), while other retries are only
 * reported (/retry-net)
 */
static void test_refused_credentials(void) {
    WCHAR fake[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");

    ResticExecOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.timeout_ms  = 15000;
    opts.notice_proc = count_notice;
    g_notices        = 0;
    ResticCommandOutput out;
    ULONGLONG t0     = GetTickCount64();
    ResticResult res = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/retry-auth", &opts, &out);
    EXPECT_TRUE(res == RESTIC_ERR_CREDENTIALS_REFUSED && GetTickCount64() - t0 < 10000,
                "A refused secret key ends restic at once instead of after its 15 minutes of retries");
    EXPECT_TRUE(wcsstr(out.error_message, L"refused the credentials: Stat: The request signature we calculated") != NULL,
                "... with the storage's reason");
    EXPECT_TRUE(g_notices == 1 && strstr(g_last_notice, "retrying after") != NULL, "... which was also reported");
    restic_free_command_output(&out);

    g_notices = 0;
    res       = restic_exec_command(fake, REPO(L"repo", L"pw"), L"/retry-net", &opts, &out);
    EXPECT_TRUE(res == RESTIC_OK && g_notices == 1 && strstr(g_last_notice, "i/o timeout") != NULL,
                "Another retry is reported and restic left alone");
    restic_free_command_output(&out);

    WCHAR dest[MAX_PATH], err[512];
    GetTempPathW(MAX_PATH, dest);
    wcscat_s(dest, MAX_PATH, L"restic_refused_dump.bin");
    DeleteFileW(dest);
    t0  = GetTickCount64();
    res = restic_dump_file(fake, REPO(L"repo", L"pw"), "abcd1234", L"\\retry-auth", dest, TRUE, NULL, 15000, NULL, NULL,
                           err, _countof(err));
    EXPECT_TRUE(res == RESTIC_ERR_CREDENTIALS_REFUSED && GetTickCount64() - t0 < 10000 &&
                    wcsstr(err, L"refused the credentials") != NULL && GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES,
                "restic dump is ended the same way, writing nothing");
}

/* restic_read_version against fake_restic.exe, which prints FAKE_RESTIC_VERSION as restic would */
static void test_read_version(void) {
    WCHAR fake[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, fake, MAX_PATH);
    WCHAR* slash = (len > 0 && len < MAX_PATH) ? wcsrchr(fake, L'\\') : NULL;
    EXPECT_TRUE(slash != NULL, "Locate the test's own folder");
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - fake), L"fake_restic.exe");
    static const struct {
        const WCHAR* printed; /* NULL: the fake's default */
        BOOL text;            /* Printed as text, as restic before 0.17.0 does */
        ResticResult result;
        unsigned version;
    } cases[] = {
        {NULL, FALSE, RESTIC_OK, RESTIC_VERSION(0, 19, 1)},
        {L"0.17.0", FALSE, RESTIC_OK, RESTIC_VERSION(0, 17, 0)},
        {L"0.17.1", FALSE, RESTIC_OK, RESTIC_MIN_VERSION},
        {L"1.2.3-dev (compiled manually)", FALSE, RESTIC_OK, RESTIC_VERSION(1, 2, 3)},
        {L"0.17", FALSE, RESTIC_ERR_JSON_PARSE, 0},
        {L"v0.18.0", FALSE, RESTIC_ERR_JSON_PARSE, 0},
        {L"0.18.99999999", FALSE, RESTIC_ERR_JSON_PARSE, 0},
        {L"0.16.4", TRUE, RESTIC_ERR_JSON_PARSE, 0}, /* Text, however recent it says it is */
        {L"0.19.1", TRUE, RESTIC_ERR_JSON_PARSE, 0},
    };
    for (size_t i = 0; i < _countof(cases); ++i) {
        SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION", cases[i].printed);
        SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION_TEXT", cases[i].text ? L"1" : NULL);
        unsigned version = 12345;
        WCHAR err[512];
        ResticResult res = restic_read_version(fake, NULL, &version, err, _countof(err));
        EXPECT_TRUE(res == cases[i].result && version == cases[i].version, "restic version parsed, or refused as not one");
    }
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION", NULL);
    SetEnvironmentVariableW(L"FAKE_RESTIC_VERSION_TEXT", NULL);
    EXPECT_TRUE(RESTIC_VERSION(0, 17, 0) < RESTIC_MIN_VERSION && RESTIC_VERSION(0, 17, 1) == RESTIC_MIN_VERSION &&
                    RESTIC_VERSION(0, 9, 999) < RESTIC_VERSION(0, 10, 0),
                "Versions compare as numbers");
}

int main(void) {
    /* No modal system dialog (crash, missing disk, failed start) may stop an unattended CI run */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); /* Invalid parameters, e.g. an overflowing wcscat_s */
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    printf("Running Restic CLI & JSON Parser Unit/Integration Tests...\n");

    test_iso8601_parsing();
    test_path_and_quote_helpers();
    test_describe_password();
    test_parse_ls_ndjson();
    test_exec_streams_and_exit_codes();
    test_exec_environment();
    test_exec_environment_credentials();
    test_exec_limits_and_failures();
    test_exec_large_output_and_long_args();
    test_exec_line_streaming();
    test_repo_cache_generations();
    test_repo_cache_histories();
    test_dump_with_fake_restic();
    test_read_version();
    test_process_tree_ends();
    test_refused_credentials();
    test_live_restic_cli();

#if defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
    if (!_CrtCheckMemory()) {
        printf("[FAIL] CRT heap integrity check failed.\n");
        g_tests_failed++;
    }
#endif

    if (g_tests_failed == 0) {
        printf("[PASS] All %d assertions succeeded.\n", g_tests_run);
        return 0;
    }
    printf("[FAIL] %d of %d assertions failed.\n", g_tests_failed, g_tests_run);
    return 1;
}
