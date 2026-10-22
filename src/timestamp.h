#pragma once
#include <stdint.h>

/*
 * RFC 3339 timestamps as restic writes them ("2026-10-18T16:28:15.182004+02:00").
 *
 * Portable C11 with no Win32 dependency, so the snapshot tree built on it (snapshot_tree.c) can be
 * tested on any platform. The calendar is checked here rather than by SystemTimeToFileTime, so
 * every platform rejects the same dates.
 */

/* 100-nanosecond ticks between 1601-01-01 (the FILETIME epoch) and 1970-01-01 */
#define TIMESTAMP_UNIX_EPOCH_TICKS 116444736000000000ULL

typedef struct {
    int year, month, day;      /* Wall-clock date as written, in the timestamp's own zone */
    int hour, minute, second;  /* Wall-clock time as written (a leap second stays 60) */
    int offset_minutes;        /* Zone offset east of UTC; 0 for "Z" or no zone */
    uint32_t nanosecond;       /* Fraction of the second, truncated to nanoseconds */
    uint64_t utc_ticks;        /* The instant in 100 ns ticks since 1601-01-01 UTC (a FILETIME) */
} Timestamp;

/*
 * Parses a complete RFC 3339 timestamp: "YYYY-MM-DDTHH:MM:SS" ('t' or ' ' also separate date and
 * time), an optional fraction of at least one digit, and an optional zone "Z", "+HH", "+HHMM" or
 * "+HH:MM"; nothing may follow. Rejects invalid calendar dates (February 30, February 29 of a
 * common year), years before 1601 and instants before the FILETIME epoch. A leap second (:60) is
 * accepted and counted as :59. Returns 1 on success, 0 otherwise (`*out` is then zeroed).
 */
int timestamp_parse(const char* text, Timestamp* out);

/* Orders two parsed timestamps by the instant they denote (nanosecond precision): <0, 0, >0 */
int timestamp_compare(const Timestamp* a, const Timestamp* b);
