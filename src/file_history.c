#include "file_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "snapshot_tree.h"

/* Longest name Windows accepts for one path component, in UTF-16 units */
#define MAX_COMPONENT_UTF16 255

/* One snapshot holding a version, in restic's order (youngest snapshot first) */
typedef struct {
    char id[FILE_HISTORY_ID_CHARS + 1];
    size_t version;
} HistoryEntry;

struct FileHistory {
    FileVersion* versions; /* No pointer of their own to free: the names live in `names` */
    size_t count;
    HistoryEntry* entries;
    size_t entry_count;
    char* names;           /* Every version's name, NUL-terminated one after the other */
};

static int same_version(const FileVersion* v, const char* type, uint64_t size, const Timestamp* mtime) {
    return v->size == size && v->mtime.utc_ticks == mtime->utc_ticks && v->mtime.nanosecond == mtime->nanosecond &&
           strcmp(v->type, type) == 0;
}

static int compare_versions(const void* a, const void* b) {
    const FileVersion* x = (const FileVersion*)a;
    const FileVersion* y = (const FileVersion*)b;
    int c = timestamp_compare(&x->mtime, &y->mtime);
    if (c != 0) return c;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return strcmp(x->type, y->type);
}

/* Length of the UTF-8 sequence at s (1-4), or 1 for a byte that does not start one */
static size_t utf8_length(const unsigned char* s) {
    if (s[0] >= 0xF0 && s[1] && s[2] && s[3]) return 4;
    if (s[0] >= 0xE0 && s[1] && s[2]) return 3;
    if (s[0] >= 0xC0 && s[1]) return 2;
    return 1;
}

/* UTF-16 units of UTF-8 text */
static size_t utf16_units(const char* text) {
    size_t units = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p;) {
        size_t n = utf8_length(p);
        units += n == 4 ? 2 : 1;
        p += n;
    }
    return units;
}

/* Bytes of `text` that fit into `units` UTF-16 units, cut between characters */
static size_t utf8_prefix(const char* text, size_t units) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        size_t n = utf8_length(p);
        size_t u = n == 4 ? 2 : 1;
        if (u > units) break;
        units -= u;
        p += n;
    }
    return (size_t)(p - (const unsigned char*)text);
}

static void free_parts(FileHistory* h) {
    free(h->versions);
    free(h->entries);
    free(h->names);
    free(h);
}

/* Names the sorted versions "<time>[-n] (<short ID>) <file name>" into one buffer; 0 when out of memory */
static int name_versions(FileHistory* h, const char* file_name, const char* time_format) {
    size_t name_units = utf16_units(file_name);
    size_t cap        = 1;
    for (size_t i = 0; i < h->count; ++i) cap += SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 40 + strlen(file_name) + 1;
    h->names = (char*)malloc(cap);
    if (!h->names) return 0;
    size_t* offsets = (size_t*)malloc((h->count ? h->count : 1) * sizeof(size_t));
    if (!offsets) return 0;
    size_t used = 0;
    for (size_t i = 0; i < h->count; ++i) {
        char when[SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 1], prefix[SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 40];
        snapshot_tree_format_time(&h->versions[i].mtime, time_format, when);
        const char* id = h->versions[i].snapshot_id; /* At least SNAPSHOT_TREE_SHORT_ID_CHARS: checked */
        for (unsigned n = 0;; ++n) {
            if (n == 0) {
                snprintf(prefix, sizeof(prefix), "%s (%.*s) ", when, SNAPSHOT_TREE_SHORT_ID_CHARS, id);
            } else {
                snprintf(prefix, sizeof(prefix), "%s-%u (%.*s) ", when, n, SNAPSHOT_TREE_SHORT_ID_CHARS, id);
            }
            size_t prefix_units = utf16_units(prefix);
            size_t keep         = prefix_units + name_units <= MAX_COMPONENT_UTF16
                                      ? strlen(file_name)
                                      : utf8_prefix(file_name, MAX_COMPONENT_UTF16 - prefix_units);
            char* candidate = h->names + used;
            size_t len      = strlen(prefix);
            memcpy(candidate, prefix, len);
            memcpy(candidate + len, file_name, keep);
            candidate[len + keep] = '\0';
            int taken = 0;
            for (size_t j = 0; j < i && !taken; ++j) taken = strcmp(h->names + offsets[j], candidate) == 0;
            if (!taken) {
                offsets[i] = used;
                used += len + keep + 1;
                break;
            }
        }
    }
    for (size_t i = 0; i < h->count; ++i) h->versions[i].name = h->names + offsets[i];
    free(offsets);
    return 1;
}

FileHistoryStatus file_history_build(const char* json, size_t len, const char* path, const char* in_snapshot,
                                     const char* time_format, FileHistory** out) {
    if (!out) return FILE_HISTORY_BAD_JSON;
    *out = NULL;
    if (!json || !path || path[0] != '/') return FILE_HISTORY_BAD_JSON;
    if (!time_format || !snapshot_tree_time_format_valid(time_format)) time_format = SNAPSHOT_TREE_DEFAULT_TIME_FORMAT;
    JsonDoc* doc = json_parse(json, len);
    if (!doc) return FILE_HISTORY_BAD_JSON;
    const JsonValue* root = json_root(doc);
    if (root->type != JSON_ARRAY) {
        json_free(doc);
        return FILE_HISTORY_BAD_JSON;
    }

    size_t snapshots = 0;
    for (const JsonValue* s = root->child; s; s = s->next) ++snapshots;
    FileHistory* h = (FileHistory*)calloc(1, sizeof(FileHistory));
    if (h && snapshots) {
        h->versions = (FileVersion*)calloc(snapshots, sizeof(FileVersion));
        h->entries  = (HistoryEntry*)calloc(snapshots, sizeof(HistoryEntry));
    }
    if (!h || (snapshots && (!h->versions || !h->entries))) {
        if (h) free_parts(h);
        json_free(doc);
        return FILE_HISTORY_NO_MEMORY;
    }

    FileHistoryStatus status = FILE_HISTORY_OK;
    int found_in_snapshot    = 0;
    for (const JsonValue* s = root->child; s && status == FILE_HISTORY_OK; s = s->next) {
        const char* id        = json_string(json_object_get(s, "snapshot"));
        const JsonValue* list = json_object_get(s, "matches");
        size_t id_len         = id ? strlen(id) : 0;
        if (s->type != JSON_OBJECT || !list || list->type != JSON_ARRAY || id_len < SNAPSHOT_TREE_SHORT_ID_CHARS ||
            id_len > FILE_HISTORY_ID_CHARS || strspn(id, "0123456789abcdef") != id_len) {
            status = FILE_HISTORY_BAD_JSON;
            break;
        }
        /* The match at exactly `path` (restic lists one per snapshot; what lies below a folder is not it) */
        const JsonValue* match = NULL;
        for (const JsonValue* m = list->child; m && !match; m = m->next) {
            const char* p = json_string(json_object_get(m, "path"));
            if (m->type == JSON_OBJECT && p && strcmp(p, path) == 0) match = m;
        }
        const char* type = match ? json_string(json_object_get(match, "type")) : NULL;
        Timestamp mtime;
        unsigned long long size = 0;
        if (!type || strcmp(type, "dir") == 0 || strlen(type) >= sizeof(h->versions[0].type) ||
            !timestamp_parse(json_string(json_object_get(match, "mtime")), &mtime)) {
            continue; /* A folder there, or nothing usable */
        }
        json_u64(json_object_get(match, "size"), &size); /* Missing for some node types: 0 */
        if (in_snapshot && strcmp(id, in_snapshot) == 0) found_in_snapshot = 1;

        size_t v = 0;
        while (v < h->count && !same_version(&h->versions[v], type, (uint64_t)size, &mtime)) ++v;
        if (v == h->count) {
            FileVersion* nv = &h->versions[h->count++];
            nv->size        = (uint64_t)size;
            nv->mtime       = mtime;
            memcpy(nv->type, type, strlen(type) + 1); /* Shorter than the field: checked above */
        }
        memcpy(h->versions[v].snapshot_id, id, id_len + 1); /* The last seen is the oldest */
        h->versions[v].snapshot_count++;
        HistoryEntry* e = &h->entries[h->entry_count++];
        memcpy(e->id, id, id_len + 1);
        e->version = v;
    }
    if (status == FILE_HISTORY_OK && in_snapshot && !found_in_snapshot) status = FILE_HISTORY_NOT_A_FILE;
    if (status == FILE_HISTORY_OK && h->count == 0 && !in_snapshot) status = FILE_HISTORY_NOT_A_FILE;

    if (status == FILE_HISTORY_OK) {
        /* Sort the versions by time, then point the entries at their new places */
        size_t* order = (size_t*)malloc((h->count ? h->count : 1) * sizeof(size_t));
        FileVersion* sorted = (FileVersion*)malloc((h->count ? h->count : 1) * sizeof(FileVersion));
        if (!order || !sorted) {
            status = FILE_HISTORY_NO_MEMORY;
        } else {
            memcpy(sorted, h->versions, h->count * sizeof(FileVersion));
            qsort(sorted, h->count, sizeof(FileVersion), compare_versions);
            for (size_t i = 0; i < h->count; ++i) {
                for (size_t j = 0; j < h->count; ++j) {
                    if (same_version(&sorted[j], h->versions[i].type, h->versions[i].size, &h->versions[i].mtime)) {
                        order[i] = j;
                    }
                }
            }
            for (size_t k = 0; k < h->entry_count; ++k) h->entries[k].version = order[h->entries[k].version];
            memcpy(h->versions, sorted, h->count * sizeof(FileVersion));
        }
        free(order);
        free(sorted);
    }
    if (status == FILE_HISTORY_OK) {
        const char* slash = strrchr(path, '/');
        if (!name_versions(h, slash ? slash + 1 : path, time_format)) status = FILE_HISTORY_NO_MEMORY;
    }
    json_free(doc);
    if (status != FILE_HISTORY_OK) {
        free_parts(h);
        return status;
    }
    *out = h;
    return FILE_HISTORY_OK;
}

void file_history_free(FileHistory* history) {
    if (history) free_parts(history);
}

size_t file_history_count(const FileHistory* history) {
    return history ? history->count : 0;
}

const FileVersion* file_history_version(const FileHistory* history, size_t i) {
    return history && i < history->count ? &history->versions[i] : NULL;
}

const FileVersion* file_history_find(const FileHistory* history, const char* name) {
    for (size_t i = 0; history && name && i < history->count; ++i) {
        if (strcmp(history->versions[i].name, name) == 0) return &history->versions[i];
    }
    return NULL;
}

const char* file_history_snapshot(const FileHistory* history, size_t i, size_t k) {
    for (size_t e = 0; history && e < history->entry_count; ++e) {
        if (history->entries[e].version != i) continue;
        if (k == 0) return history->entries[e].id;
        --k;
    }
    return NULL;
}

size_t file_history_pattern(const char* path, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = path; *p; ++p) {
        const char* with = *p == '*' ? "[*]" : *p == '?' ? "[?]" : *p == '[' ? "[[]" : NULL;
        size_t n         = with ? 3 : 1;
        if (o + n + 1 > cap) return 0;
        if (with) {
            memcpy(out + o, with, 3);
        } else {
            out[o] = *p;
        }
        o += n;
    }
    if (o + 1 > cap) return 0;
    out[o] = '\0';
    return o;
}
