#include "timestamp.h"
#include <string.h>

#define TICKS_PER_SECOND 10000000ULL

/* Value of `count` ASCII digits at `s`, or -1 if any of them is not a digit */
static int fixed_digits(const char* s, int count) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap_year(year) ? 29 : days[month - 1];
}

/* Days from 1601-01-01 to the given proleptic Gregorian date (year >= 1601) */
static int64_t days_since_1601(int year, int month, int day) {
    int64_t y = year - 1601;
    int64_t days = y * 365 + y / 4 - y / 100 + y / 400;
    for (int m = 1; m < month; ++m) days += days_in_month(year, m);
    return days + (day - 1);
}

int timestamp_parse(const char* text, Timestamp* out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!text || strlen(text) < 19) return 0;
    if (text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't' && text[10] != ' ') ||
        text[13] != ':' || text[16] != ':') {
        return 0;
    }

    Timestamp ts;
    memset(&ts, 0, sizeof(ts));
    ts.year   = fixed_digits(text + 0, 4);
    ts.month  = fixed_digits(text + 5, 2);
    ts.day    = fixed_digits(text + 8, 2);
    ts.hour   = fixed_digits(text + 11, 2);
    ts.minute = fixed_digits(text + 14, 2);
    ts.second = fixed_digits(text + 17, 2);
    if (ts.year < 1601 || ts.month < 1 || ts.month > 12 || ts.day < 1 ||
        ts.day > days_in_month(ts.year, ts.month) || ts.hour < 0 || ts.hour > 23 || ts.minute < 0 ||
        ts.minute > 59 || ts.second < 0 || ts.second > 60) {
        return 0;
    }

    /* Optional fraction: nanoseconds for ordering, the FILETIME keeps 100 ns of it */
    const char* p = text + 19;
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return 0; /* "." needs at least one digit */
        uint32_t scale = 100000000u;
        while (*p >= '0' && *p <= '9') {
            ts.nanosecond += (uint32_t)(*p - '0') * scale;
            scale /= 10u;
            ++p;
        }
    }

    if (*p == 'Z' || *p == 'z') {
        ++p;
    } else if (*p == '+' || *p == '-') {
        int sign = *p == '+' ? 1 : -1;
        ++p;
        int tz_h = fixed_digits(p, 2);
        if (tz_h < 0 || tz_h > 23) return 0;
        p += 2;
        int tz_m = 0;
        if (*p != '\0') {
            if (*p == ':') ++p;
            tz_m = fixed_digits(p, 2);
            if (tz_m < 0 || tz_m > 59) return 0;
            p += 2;
        }
        ts.offset_minutes = sign * (tz_h * 60 + tz_m);
    }
    if (*p != '\0') return 0; /* Trailing characters: not a timestamp restic writes */

    int64_t local_seconds = days_since_1601(ts.year, ts.month, ts.day) * 86400 + ts.hour * 3600 +
                            ts.minute * 60 + (ts.second == 60 ? 59 : ts.second);
    int64_t utc_seconds = local_seconds - (int64_t)ts.offset_minutes * 60;
    if (utc_seconds < 0) return 0; /* Would precede the FILETIME epoch */
    ts.utc_ticks = (uint64_t)utc_seconds * TICKS_PER_SECOND + ts.nanosecond / 100u;

    *out = ts;
    return 1;
}

int timestamp_compare(const Timestamp* a, const Timestamp* b) {
    uint64_t sa = a->utc_ticks / TICKS_PER_SECOND, sb = b->utc_ticks / TICKS_PER_SECOND;
    if (sa != sb) return sa < sb ? -1 : 1;
    if (a->nanosecond != b->nanosecond) return a->nanosecond < b->nanosecond ? -1 : 1;
    return 0;
}
