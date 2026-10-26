#include "snapshot_tree.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "timestamp.h"

/* Longest name Windows accepts for one path component, in UTF-16 units */
#define MAX_COMPONENT_UTF16 255

/* A node while the tree is built; the public SnapshotNode is its first member */
typedef struct BuildNode BuildNode;
struct BuildNode {
    SnapshotNode pub;
    char* name;
    BuildNode* parent;
    BuildNode** kids;
    size_t kid_count, kid_cap;
    const SnapshotNode** view; /* pub.children: the kids as SnapshotNode pointers */
    const BuildNode* target;   /* "latest": the snapshot folder it stands for (restic's link target) */
};

/* One usable entry of the snapshots array */
typedef struct {
    char id[SNAPSHOT_TREE_ID_CHARS + 1];
    Timestamp time;
    const char* hostname;             /* Points into the JSON document */
    const JsonValue* tags;            /* JSON array or NULL */
    const JsonValue* paths;           /* JSON array or NULL */
    int has_size;                     /* The snapshot has a summary (restic 0.17 and later) */
    uint64_t size;                    /* summary.total_bytes_processed, what restic shows as Size */
    const JsonValue* object;          /* The whole entry, for the details (new_info) */
} SnapshotRecord;

/*
 * What snapshot_tree_csv() lists of a snapshot, as restic reported it. One allocation, the strings
 * right after the structure, so a single free() releases it (and /analyze, which does not trust
 * calloc'd pointer members, sees nothing left to read before freeing it).
 */
typedef struct {
    const char* id;    /* The tree's copy of the full ID */
    Timestamp time;
    const char* host;  /* These three point just past the structure */
    const char* paths; /* The backed-up paths, one per line */
    const char* tags;  /* The tags, one per line */
    int has_size;      /* 0: no summary, the size is not known */
    uint64_t size;     /* Bytes the backup processed */
    /* The rest for snapshot_tree_describe(); strings "" when restic did not record them */
    const char* username;
    const char* program_version;
    const char* parent; /* The parent snapshot's ID */
    int has_counts;     /* The summary's file and folder counts and data added are known */
    uint64_t files_new, files_changed, files_unmodified, dirs_new, dirs_changed, dirs_unmodified;
    uint64_t data_added, data_added_packed, total_files;
    int has_backup_times;
    Timestamp backup_start, backup_end;
} SnapshotInfo;

/* Open-addressing hash set of (parent, name) -> node, so inserting N snapshots stays O(N) */
typedef struct {
    BuildNode** slots;
    size_t cap, used;
} NodeIndex;

struct SnapshotTree {
    BuildNode* root;
    size_t snapshot_count;
    char** ids;          /* Owned copies of the full IDs the nodes point to */
    SnapshotInfo** info; /* snapshot_count entries, in the order of the IDs */
    uint64_t listed_at;  /* snapshot_tree_set_listed_at() */
    char* json;          /* A copy of what the tree was built from (snapshot_tree_json) */
    size_t json_len;
};

static uint64_t hash_key(const BuildNode* parent, const char* name) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)(uintptr_t)parent;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static BuildNode** index_slot(NodeIndex* idx, const BuildNode* parent, const char* name) {
    size_t mask = idx->cap - 1;
    for (size_t i = (size_t)hash_key(parent, name) & mask;; i = (i + 1) & mask) {
        BuildNode* n = idx->slots[i];
        if (!n || (n->parent == parent && strcmp(n->name, name) == 0)) return &idx->slots[i];
    }
}

static int index_grow(NodeIndex* idx) {
    size_t cap = idx->cap ? idx->cap * 2 : 1024;
    BuildNode** old = idx->slots;
    size_t old_cap = idx->cap;
    idx->slots = (BuildNode**)calloc(cap, sizeof(BuildNode*));
    if (!idx->slots) {
        idx->slots = old;
        return 0;
    }
    idx->cap = cap;
    for (size_t i = 0; i < old_cap; ++i) {
        if (old[i]) *index_slot(idx, old[i]->parent, old[i]->name) = old[i];
    }
    free(old);
    return 1;
}

static void free_node(BuildNode* n) {
    if (!n) return;
    for (size_t i = 0; i < n->kid_count; ++i) free_node(n->kids[i]);
    free(n->kids);
    free((void*)n->view);
    free(n->name);
    free(n);
}

/* The child of `parent` named `name`, created when missing; NULL only when out of memory */
static BuildNode* get_child(NodeIndex* idx, BuildNode* parent, const char* name, int* created) {
    if (created) *created = 0;
    if ((idx->used + 1) * 2 > idx->cap && !index_grow(idx)) return NULL;
    BuildNode** slot = index_slot(idx, parent, name);
    if (*slot) return *slot;

    if (parent->kid_count == parent->kid_cap) {
        size_t cap = parent->kid_cap ? parent->kid_cap * 2 : 4;
        BuildNode** kids = (BuildNode**)realloc(parent->kids, cap * sizeof(BuildNode*));
        if (!kids) return NULL;
        parent->kids = kids;
        parent->kid_cap = cap;
    }
    BuildNode* n = (BuildNode*)calloc(1, sizeof(BuildNode));
    if (!n) return NULL;
    size_t len = strlen(name);
    char* copy = (char*)malloc(len + 1);
    if (!copy) {
        free(n);
        return NULL;
    }
    memcpy(copy, name, len + 1);
    n->name = copy;
    n->pub.name = copy;
    n->parent = parent;
    parent->kids[parent->kid_count++] = n;
    *slot = n;
    idx->used++;
    if (created) *created = 1;
    return n;
}

/* Length of the valid UTF-8 sequence at s (1-4), or 0 if the bytes there are not one */
static size_t utf8_sequence_length(const unsigned char* s) {
    if (s[0] < 0x80) return 1;
    size_t n;
    unsigned min;
    unsigned cp;
    if ((s[0] & 0xE0) == 0xC0) {
        n = 2, cp = s[0] & 0x1Fu, min = 0x80;
    } else if ((s[0] & 0xF0) == 0xE0) {
        n = 3, cp = s[0] & 0x0Fu, min = 0x800;
    } else if ((s[0] & 0xF8) == 0xF0) {
        n = 4, cp = s[0] & 0x07u, min = 0x10000;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return n;
}

/*
 * A host name or tag as one Windows-safe path component (see snapshot_tree.h). Returns a heap
 * string, or NULL when out of memory.
 */
static char* component_name(const char* raw) {
    if (!raw) raw = "";
    size_t len = strlen(raw);
    char* out = (char*)malloc(len + 2);
    if (!out) return NULL;
    size_t o = 0, utf16 = 0;
    for (const unsigned char* p = (const unsigned char*)raw; *p;) {
        size_t n = utf8_sequence_length(p);
        size_t units = n == 4 ? 2 : 1;
        if (utf16 + units > MAX_COMPONENT_UTF16) break;
        if (n == 0) {
            out[o++] = '_'; /* Invalid UTF-8 could not come back from Total Commander unchanged */
            p++;
        } else if (n == 1) {
            char c = (char)*p++;
            out[o++] = ((unsigned char)c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c)) ? '_' : c;
        } else {
            memcpy(out + o, p, n);
            o += n;
            p += n;
        }
        utf16 += units;
    }
    /* Win32 strips trailing dots and spaces; "", ".", ".." become "_", "_", "__" as in restic */
    for (size_t i = o; i > 0 && (out[i - 1] == '.' || out[i - 1] == ' '); --i) out[i - 1] = '_';
    if (o == 0) out[o++] = '_';
    out[o] = '\0';
    return out;
}

static int compare_records(const void* a, const void* b) {
    const SnapshotRecord* x = (const SnapshotRecord*)a;
    const SnapshotRecord* y = (const SnapshotRecord*)b;
    int c = timestamp_compare(&x->time, &y->time);
    return c != 0 ? c : strcmp(x->id, y->id); /* Equal-length lowercase hex: byte order of the IDs */
}

static int compare_nodes(const void* a, const void* b) {
    return strcmp((*(const BuildNode* const*)a)->name, (*(const BuildNode* const*)b)->name);
}

/* Freezes the build nodes into the public view, children sorted by name; 0 when out of memory */
static int finish_node(BuildNode* n) {
    if (n->kid_count == 0) return 1;
    qsort(n->kids, n->kid_count, sizeof(BuildNode*), compare_nodes);
    n->view = (const SnapshotNode**)malloc(n->kid_count * sizeof(SnapshotNode*));
    if (!n->view) return 0;
    for (size_t i = 0; i < n->kid_count; ++i) {
        if (!finish_node(n->kids[i])) return 0;
        n->view[i] = &n->kids[i]->pub;
    }
    n->pub.children = n->view;
    n->pub.child_count = n->kid_count;
    return 1;
}

/* Newest time along the path from the root, as restic shows the latest change of a folder */
static void propagate_time(BuildNode* n, uint64_t time) {
    for (; n; n = n->parent) {
        if (n->pub.time < time) n->pub.time = time;
    }
}

/*
 * restic's uniqueName + mount for one snapshot below `dir`: `base` is the time or short ID; the
 * first free name of base, base-1, base-2, ... becomes the snapshot's folder
 */
static int add_snapshot(NodeIndex* idx, BuildNode* dir, const char* base, const SnapshotRecord* rec,
                        const char* id, int with_latest) {
    char name[MAX_COMPONENT_UTF16 + 32];
    BuildNode* leaf = NULL;
    for (unsigned i = 0; !leaf; ++i) {
        size_t len = strlen(base);
        if (len > MAX_COMPONENT_UTF16) len = MAX_COMPONENT_UTF16;
        memcpy(name, base, len);
        name[len] = '\0';
        if (i > 0) snprintf(name + len, sizeof(name) - len, "-%u", i);
        int created = 0;
        BuildNode* n = get_child(idx, dir, name, &created);
        if (!n) return 0;
        if (created) leaf = n;
    }
    leaf->pub.snapshot_id = id;
    propagate_time(leaf, rec->time.utc_ticks);
    if (with_latest) {
        /* Inserted in ascending time order: the snapshot inserted last is always the latest */
        BuildNode* latest = get_child(idx, dir, "latest", NULL);
        if (!latest) return 0;
        latest->pub.snapshot_id = id;
        latest->pub.time = rec->time.utc_ticks;
        latest->target = leaf;
    }
    return 1;
}

/* Reads the usable entries of the snapshots array into records[] (sorted); returns their count */
static size_t collect_records(const JsonValue* array, SnapshotRecord* records) {
    size_t n = 0;
    for (const JsonValue* v = array->child; v; v = v->next) {
        if (v->type != JSON_OBJECT) continue;
        const char* id   = json_string(json_object_get(v, "id"));
        const char* time = json_string(json_object_get(v, "time"));
        size_t id_len    = id ? strlen(id) : 0;
        if (id_len < SNAPSHOT_TREE_SHORT_ID_CHARS || id_len > SNAPSHOT_TREE_ID_CHARS) continue;
        if (strspn(id, "0123456789abcdef") != id_len) continue;
        SnapshotRecord* r = &records[n];
        if (!timestamp_parse(time, &r->time)) continue;
        memcpy(r->id, id, id_len + 1);
        r->hostname = json_string(json_object_get(v, "hostname"));
        const JsonValue* tags = json_object_get(v, "tags");
        r->tags = tags && tags->type == JSON_ARRAY ? tags : NULL;
        const JsonValue* paths = json_object_get(v, "paths");
        r->paths = paths && paths->type == JSON_ARRAY ? paths : NULL;
        /* restic snapshots shows a summary without the field as 0 B, a snapshot without one with no size */
        const JsonValue* summary = json_object_get(v, "summary");
        const JsonValue* bytes   = json_object_get(summary, "total_bytes_processed");
        unsigned long long size  = 0;
        r->has_size = summary && summary->type == JSON_OBJECT && (!bytes || json_u64(bytes, &size));
        r->size     = r->has_size ? (uint64_t)size : 0;
        r->object   = v;
        n++;
    }
    qsort(records, n, sizeof(SnapshotRecord), compare_records);
    return n;
}

/*
 * The strings of a JSON array (other elements skipped) joined by line breaks, NUL-terminated, into
 * `out` unless it is NULL; returns the bytes it takes, NUL included
 */
static size_t join_lines(const JsonValue* array, char* out) {
    size_t o = 0;
    for (const JsonValue* v = array ? array->child : NULL; v; v = v->next) {
        if (v->type != JSON_STRING) continue;
        size_t n = strlen(json_string(v));
        if (out && o > 0) out[o - 1] = '\n'; /* Over the NUL of the previous line */
        if (out) memcpy(out + o, json_string(v), n + 1);
        o += n + 1;
    }
    if (o == 0) {
        if (out) out[0] = '\0';
        o = 1;
    }
    return o;
}

/* A string member of a snapshot's JSON object, "" when it is missing or not a string */
static const char* member_text(const JsonValue* object, const char* key) {
    const char* text = json_string(json_object_get(object, key));
    return text ? text : "";
}

/* The summary counts of a snapshot; 0 when any is missing (an older restic recorded fewer) */
static int read_counts(const JsonValue* summary, SnapshotInfo* info) {
    static const char* const keys[] = {"files_new", "files_changed", "files_unmodified", "dirs_new", "dirs_changed",
                                       "dirs_unmodified", "data_added", "data_added_packed", "total_files_processed"};
    uint64_t* fields[] = {&info->files_new, &info->files_changed, &info->files_unmodified, &info->dirs_new,
                          &info->dirs_changed, &info->dirs_unmodified, &info->data_added, &info->data_added_packed,
                          &info->total_files};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        unsigned long long value = 0;
        if (!json_u64(json_object_get(summary, keys[i]), &value)) return 0;
        *fields[i] = (uint64_t)value;
    }
    return 1;
}

/* The snapshot's SnapshotInfo, its strings in the same allocation; NULL when out of memory */
static SnapshotInfo* new_info(const SnapshotRecord* rec, const char* id) {
    const char* host    = rec->hostname ? rec->hostname : "";
    const char* user    = member_text(rec->object, "username");
    const char* program = member_text(rec->object, "program_version");
    const char* parent  = member_text(rec->object, "parent");
    size_t host_len     = strlen(host) + 1;
    size_t paths_len    = join_lines(rec->paths, NULL);
    size_t tags_len     = join_lines(rec->tags, NULL);
    size_t user_len = strlen(user) + 1, program_len = strlen(program) + 1, parent_len = strlen(parent) + 1;
    SnapshotInfo* info = (SnapshotInfo*)calloc(1, sizeof(SnapshotInfo) + host_len + paths_len + tags_len + user_len +
                                                      program_len + parent_len);
    if (!info) return NULL;
    char* text = (char*)(info + 1);
    memcpy(text, host, host_len);
    join_lines(rec->paths, text + host_len);
    join_lines(rec->tags, text + host_len + paths_len);
    char* more = text + host_len + paths_len + tags_len;
    memcpy(more, user, user_len);
    memcpy(more + user_len, program, program_len);
    memcpy(more + user_len + program_len, parent, parent_len);
    info->id    = id;
    info->time  = rec->time;
    info->host  = text;
    info->paths = text + host_len;
    info->tags  = text + host_len + paths_len;
    info->has_size        = rec->has_size;
    info->size            = rec->size;
    info->username        = more;
    info->program_version = more + user_len;
    info->parent          = more + user_len + program_len;
    const JsonValue* summary = json_object_get(rec->object, "summary");
    if (summary && summary->type == JSON_OBJECT) {
        info->has_counts       = read_counts(summary, info);
        info->has_backup_times = timestamp_parse(json_string(json_object_get(summary, "backup_start")),
                                                 &info->backup_start) &&
                                 timestamp_parse(json_string(json_object_get(summary, "backup_end")),
                                                 &info->backup_end);
    }
    return info;
}

/* The placeholders of a time format, longest first where one is a prefix of another */
typedef enum { TF_YEAR4, TF_YEAR2, TF_MONTH, TF_DAY, TF_HOUR, TF_MINUTE, TF_SECOND, TF_ZONE, TF_NONE } TimeField;

static const struct {
    const char* text;
    TimeField field;
} TIME_PLACEHOLDERS[] = {
    {"YYYY", TF_YEAR4}, {"YY", TF_YEAR2}, {"MM", TF_MONTH}, {"DD", TF_DAY},
    {"hh", TF_HOUR},    {"mm", TF_MINUTE}, {"ss", TF_SECOND}, {"zzzz", TF_ZONE},
};

/* The placeholder at `p`, its length in *len; TF_NONE for a literal character */
static TimeField placeholder_at(const char* p, size_t* len) {
    for (size_t i = 0; i < sizeof(TIME_PLACEHOLDERS) / sizeof(TIME_PLACEHOLDERS[0]); ++i) {
        size_t n = strlen(TIME_PLACEHOLDERS[i].text);
        if (strncmp(p, TIME_PLACEHOLDERS[i].text, n) == 0) {
            *len = n;
            return TIME_PLACEHOLDERS[i].field;
        }
    }
    *len = 1;
    return TF_NONE;
}

int snapshot_tree_time_format_valid(const char* format) {
    if (!format || format[0] == '\0' || strlen(format) > SNAPSHOT_TREE_TIME_FORMAT_MAX) return 0;
    int placeholders = 0;
    for (const char* p = format; *p;) {
        size_t len = 0;
        if (placeholder_at(p, &len) != TF_NONE) {
            placeholders++;
            p += len;
            continue;
        }
        size_t n = utf8_sequence_length((const unsigned char*)p);
        if (n == 0) return 0;
        if (n == 1 && ((unsigned char)*p < 0x20 || *p == 0x7F || strchr("\\/:*?\"<>|", *p))) return 0;
        p += n;
    }
    char last = format[strlen(format) - 1];
    return placeholders > 0 && last != '.' && last != ' ';
}

static char* put_digits(char* p, int value, int width) {
    for (int d = width - 1; d >= 0; --d) {
        p[d] = (char)('0' + value % 10);
        value /= 10;
    }
    return p + width;
}

/* The snapshot's own wall-clock time written by `format` (placeholders as in snapshot_tree.h) */
static void time_name(const Timestamp* t, const char* format, char* out) {
    char* o = out;
    for (const char* p = format; *p;) {
        size_t len = 0;
        switch (placeholder_at(p, &len)) {
            case TF_YEAR4: o = put_digits(o, t->year, 4); break;
            case TF_YEAR2: o = put_digits(o, t->year % 100, 2); break;
            case TF_MONTH: o = put_digits(o, t->month, 2); break;
            case TF_DAY: o = put_digits(o, t->day, 2); break;
            case TF_HOUR: o = put_digits(o, t->hour, 2); break;
            case TF_MINUTE: o = put_digits(o, t->minute, 2); break;
            case TF_SECOND: o = put_digits(o, t->second, 2); break;
            case TF_ZONE: {
                int off = t->offset_minutes < 0 ? -t->offset_minutes : t->offset_minutes;
                *o++ = t->offset_minutes < 0 ? '-' : '+';
                o = put_digits(o, off / 60, 2);
                o = put_digits(o, off % 60, 2);
                break;
            }
            default: *o++ = *p; break; /* A literal byte (multi-byte characters byte by byte) */
        }
        p += len;
    }
    *o = '\0';
}

/* A folder "latest → <what>" in `dir` that shows the same snapshot as `latest` */
static int add_latest_alias(NodeIndex* idx, BuildNode* dir, const BuildNode* latest, const char* what, size_t len) {
    char name[sizeof(SNAPSHOT_TREE_LATEST_LINK_PREFIX) + MAX_COMPONENT_UTF16 + 32];
    size_t prefix = sizeof(SNAPSHOT_TREE_LATEST_LINK_PREFIX) - 1;
    if (len > sizeof(name) - prefix - 1) len = sizeof(name) - prefix - 1;
    memcpy(name, SNAPSHOT_TREE_LATEST_LINK_PREFIX, prefix);
    memcpy(name + prefix, what, len);
    name[prefix + len] = '\0';
    BuildNode* link = get_child(idx, dir, name, NULL); /* An existing one (same text) is the same snapshot */
    if (!link) return 0;
    link->pub.snapshot_id = latest->pub.snapshot_id;
    link->pub.time        = latest->pub.time;
    return 1;
}

/*
 * Next to the latest folder of `dir`, if it has one, the same snapshot as "latest → <folder>" and
 * "latest → <short id>": restic mount's latest is a symlink to the folder <folder>, whose target
 * `ls -l` shows, and Total Commander shows no link targets, so the names say it
 */
static int add_latest_link(NodeIndex* idx, BuildNode* dir) {
    BuildNode** slot = index_slot(idx, dir, "latest");
    const BuildNode* latest = *slot;
    if (!latest || !latest->pub.snapshot_id || !latest->target) return 1;
    return add_latest_alias(idx, dir, latest, latest->target->name, strlen(latest->target->name)) &&
           add_latest_alias(idx, dir, latest, latest->pub.snapshot_id, SNAPSHOT_TREE_SHORT_ID_CHARS);
}

static SnapshotTreeStatus build_from_records(const SnapshotRecord* records, size_t count, const char* time_format,
                                             SnapshotTree* tree) {
    NodeIndex idx = {NULL, 0, 0};
    SnapshotTreeStatus status = SNAPSHOT_TREE_NO_MEMORY;
    BuildNode* root = (BuildNode*)calloc(1, sizeof(BuildNode));
    tree->root = root;
    tree->ids = count ? (char**)calloc(count, sizeof(char*)) : NULL;
    tree->info = count ? (SnapshotInfo**)calloc(count, sizeof(SnapshotInfo*)) : NULL;
    if (!root || (count && (!tree->ids || !tree->info)) || !(root->name = (char*)calloc(1, 1))) goto done;
    root->pub.name = root->name;

    BuildNode *ids, *snapshots, *hosts, *tags;
    if (!(hosts = get_child(&idx, root, "hosts", NULL)) || !(ids = get_child(&idx, root, "ids", NULL)) ||
        !(snapshots = get_child(&idx, root, "snapshots", NULL)) || !(tags = get_child(&idx, root, "tags", NULL))) {
        goto done;
    }

    for (size_t i = 0; i < count; ++i) {
        const SnapshotRecord* rec = &records[i];
        size_t id_len = strlen(rec->id);
        char* id = (char*)malloc(id_len + 1);
        if (!id) goto done;
        memcpy(id, rec->id, id_len + 1);
        SnapshotInfo* info = new_info(rec, id);
        if (!info) {
            free(id);
            goto done;
        }
        tree->info[tree->snapshot_count] = info;
        tree->ids[tree->snapshot_count++] = id;

        char when[SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 1], short_id[SNAPSHOT_TREE_SHORT_ID_CHARS + 1];
        time_name(&rec->time, time_format, when);
        memcpy(short_id, id, SNAPSHOT_TREE_SHORT_ID_CHARS);
        short_id[SNAPSHOT_TREE_SHORT_ID_CHARS] = '\0';

        /* The order of restic's default templates: ids/%i, snapshots/%T, hosts/%h/%T, tags/%t/%T */
        if (!add_snapshot(&idx, ids, short_id, rec, id, 0) || !add_snapshot(&idx, snapshots, when, rec, id, 1)) {
            goto done;
        }
        char* host_name = component_name(rec->hostname);
        BuildNode* host = host_name ? get_child(&idx, hosts, host_name, NULL) : NULL;
        free(host_name);
        if (!host || !add_snapshot(&idx, host, when, rec, id, 1)) goto done;
        for (const JsonValue* t = rec->tags ? rec->tags->child : NULL; t; t = t->next) {
            if (t->type != JSON_STRING) continue;
            char* tag_name = component_name(json_string(t));
            BuildNode* tag = tag_name ? get_child(&idx, tags, tag_name, NULL) : NULL;
            free(tag_name);
            if (!tag || !add_snapshot(&idx, tag, when, rec, id, 1)) goto done;
        }
    }
    /* Only now is every latest final */
    if (!add_latest_link(&idx, snapshots)) goto done;
    for (size_t i = 0; i < hosts->kid_count; ++i) {
        if (!add_latest_link(&idx, hosts->kids[i])) goto done;
    }
    for (size_t i = 0; i < tags->kid_count; ++i) {
        if (!add_latest_link(&idx, tags->kids[i])) goto done;
    }
    if (finish_node(root)) status = SNAPSHOT_TREE_OK;
done:
    free(idx.slots);
    return status;
}

SnapshotTreeStatus snapshot_tree_build(const char* json, size_t len, const char* time_format, SnapshotTree** out) {
    if (!out) return SNAPSHOT_TREE_BAD_JSON;
    *out = NULL;
    if (!time_format) time_format = SNAPSHOT_TREE_DEFAULT_TIME_FORMAT;
    if (!snapshot_tree_time_format_valid(time_format)) return SNAPSHOT_TREE_BAD_FORMAT;
    if (!json) return SNAPSHOT_TREE_BAD_JSON;
    JsonDoc* doc = json_parse(json, len);
    if (!doc) return SNAPSHOT_TREE_BAD_JSON;
    const JsonValue* root = json_root(doc);
    if (root->type != JSON_ARRAY && root->type != JSON_NULL) {
        json_free(doc);
        return SNAPSHOT_TREE_BAD_JSON;
    }

    size_t n = 0;
    for (const JsonValue* v = root->type == JSON_ARRAY ? root->child : NULL; v; v = v->next) ++n;
    SnapshotRecord* records = n ? (SnapshotRecord*)calloc(n, sizeof(SnapshotRecord)) : NULL;
    SnapshotTree* tree = (SnapshotTree*)calloc(1, sizeof(SnapshotTree));
    SnapshotTreeStatus status = SNAPSHOT_TREE_NO_MEMORY;
    if (tree && (tree->json = (char*)malloc(len + 1)) != NULL) {
        memcpy(tree->json, json, len);
        tree->json[len] = '\0';
        tree->json_len  = len;
    }
    if (tree && tree->json && (n == 0 || records)) {
        size_t count = n ? collect_records(root, records) : 0;
        status = build_from_records(records, count, time_format, tree);
    }
    free(records);
    json_free(doc);
    if (status != SNAPSHOT_TREE_OK) {
        snapshot_tree_free(tree);
        return status;
    }
    *out = tree;
    return SNAPSHOT_TREE_OK;
}

void snapshot_tree_free(SnapshotTree* tree) {
    if (!tree) return;
    free_node(tree->root);
    for (size_t i = 0; i < tree->snapshot_count; ++i) free(tree->ids[i]);
    free(tree->ids);
    for (size_t i = 0; i < tree->snapshot_count; ++i) free(tree->info[i]);
    free(tree->info);
    free(tree->json);
    free(tree);
}

const SnapshotNode* snapshot_tree_root(const SnapshotTree* tree) {
    return tree ? &tree->root->pub : NULL;
}

size_t snapshot_tree_snapshot_count(const SnapshotTree* tree) {
    return tree ? tree->snapshot_count : 0;
}

void snapshot_tree_set_listed_at(SnapshotTree* tree, uint64_t time) {
    if (tree) tree->listed_at = time;
}

uint64_t snapshot_tree_listed_at(const SnapshotTree* tree) {
    return tree ? tree->listed_at : 0;
}

const char* snapshot_tree_json(const SnapshotTree* tree, size_t* out_len) {
    if (out_len) *out_len = tree ? tree->json_len : 0;
    return tree ? tree->json : NULL;
}

static int ascii_casecmp(const char* a, const char* b);

/* A growable text buffer for snapshot_tree_csv(); `failed` once out of memory */
typedef struct {
    char* data;
    size_t len, cap;
    int failed;
} CsvText;

static void csv_put(CsvText* t, const char* text, size_t len) {
    if (t->failed) return;
    if (t->len + len + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->len + len + 1) cap *= 2;
        char* p = (char*)realloc(t->data, cap);
        if (!p) {
            t->failed = 1;
            return;
        }
        t->data = p;
        t->cap  = cap;
    }
    memcpy(t->data + t->len, text, len);
    t->len += len;
    t->data[t->len] = '\0';
}

/* A character a spreadsheet takes as the start of a formula when a cell begins with it */
static int formula_start(char c) {
    return c != '\0' && strchr("=+-@\t\r", c) != NULL;
}

/*
 * One CSV field (RFC 4180): quoted when it holds the separator, a quote or a line break, quotes
 * doubled. Host names, paths and tags come from the backed-up systems, so as OWASP advises against
 * CSV injection a formula character gets an apostrophe before it where a cell could begin with it:
 * at the start of the value, and also after any character a reader with another separator or a
 * looser parser might split cells or records at (, ; | tab, line breaks, quote) - Excel opening the
 * file with the list separator of other regional settings, say.
 */
static void csv_field(CsvText* t, const char* value, char separator) {
    int quote = strchr(value, separator) || strchr(value, '"') || strchr(value, '\n') || strchr(value, '\r');
    if (quote) csv_put(t, "\"", 1);
    for (const char* p = value; *p; ++p) {
        if (formula_start(*p) && (p == value || strchr(",;|\t\n\r\"", p[-1]))) csv_put(t, "'", 1);
        csv_put(t, p, 1);
        if (*p == '"') csv_put(t, "\"", 1);
    }
    if (quote) csv_put(t, "\"", 1);
}

/*
 * A snapshot's size as `restic snapshots` prints it (ui.FormatBytes): "<n> B" below 1 KiB, else three
 * decimals of the largest of KiB, MiB, GiB, TiB it reaches, rounded half to even like Go's "%.3f".
 * Integer arithmetic, so it is exact and the decimal point never follows the C locale.
 */
static void size_text(uint64_t bytes, char* out, size_t cap) {
    static const char* const units[] = {"KiB", "MiB", "GiB", "TiB"};
    int u = bytes >= (1ull << 40) ? 3 : bytes >= (1ull << 30) ? 2 : bytes >= (1ull << 20) ? 1 : bytes >= (1ull << 10) ? 0 : -1;
    if (u < 0) {
        snprintf(out, cap, "%llu B", (unsigned long long)bytes);
        return;
    }
    unsigned shift  = 10u * (unsigned)(u + 1);
    uint64_t mask   = (1ull << shift) - 1;
    uint64_t whole  = bytes >> shift;
    uint64_t scaled = (bytes & mask) * 1000u; /* Below 2^40 * 1000: no overflow */
    uint64_t frac   = scaled >> shift, rest = scaled & mask, half = 1ull << (shift - 1);
    if (rest > half || (rest == half && (frac & 1u))) ++frac;
    if (frac == 1000) {
        ++whole;
        frac = 0;
    }
    snprintf(out, cap, "%llu.%03u %s", (unsigned long long)whole, (unsigned)frac, units[u]);
}

static int compare_info(const void* a, const void* b) {
    const SnapshotInfo* x = *(const SnapshotInfo* const*)a;
    const SnapshotInfo* y = *(const SnapshotInfo* const*)b;
    int c = ascii_casecmp(x->host, y->host); /* "PC" and "pc" next to each other */
    if (c == 0) c = ascii_casecmp(x->paths, y->paths);
    if (c == 0) c = timestamp_compare(&x->time, &y->time);
    return c != 0 ? c : strcmp(x->id, y->id);
}

char* snapshot_tree_csv(const SnapshotTree* tree, char separator, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!tree || separator == '\0' || separator == '"' || separator == '\n' || separator == '\r') return NULL;
    size_t n = tree->snapshot_count;
    const SnapshotInfo** order = (const SnapshotInfo**)malloc((n ? n : 1) * sizeof(SnapshotInfo*));
    if (!order) return NULL;
    for (size_t i = 0; i < n; ++i) order[i] = tree->info[i];
    qsort(order, n, sizeof(SnapshotInfo*), compare_info);

    CsvText t = {NULL, 0, 0, 0};
    const char sep[2] = {separator, '\0'};
    static const char* const header[] = {"short ID", "host", "paths", "size", "timestamp", "tags", "full ID"};
    csv_put(&t, "\xEF\xBB\xBF", 3); /* UTF-8 byte-order mark: spreadsheets then read the file as UTF-8 */
    for (size_t c = 0; c < sizeof(header) / sizeof(header[0]); ++c) {
        if (c) csv_put(&t, sep, 1);
        csv_put(&t, header[c], strlen(header[c]));
    }
    csv_put(&t, "\r\n", 2);
    for (size_t i = 0; i < n; ++i) {
        const SnapshotInfo* s = order[i];
        char short_id[SNAPSHOT_TREE_SHORT_ID_CHARS + 1], when[SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 1], size[32] = "";
        memcpy(short_id, s->id, SNAPSHOT_TREE_SHORT_ID_CHARS);
        short_id[SNAPSHOT_TREE_SHORT_ID_CHARS] = '\0';
        time_name(&s->time, "YYYY-MM-DD hh:mm:ss", when); /* A date and time spreadsheets recognise */
        if (s->has_size) size_text(s->size, size, sizeof(size));
        csv_field(&t, short_id, separator);
        csv_put(&t, sep, 1);
        csv_field(&t, s->host, separator);
        csv_put(&t, sep, 1);
        csv_field(&t, s->paths, separator);
        csv_put(&t, sep, 1);
        csv_field(&t, size, separator);
        csv_put(&t, sep, 1);
        csv_field(&t, when, separator);
        csv_put(&t, sep, 1);
        csv_field(&t, s->tags, separator);
        csv_put(&t, sep, 1);
        /* The full ID as well: a spreadsheet takes a short one like 01234567 or 123456e7 for a number */
        csv_field(&t, s->id, separator);
        csv_put(&t, "\r\n", 2);
    }
    free(order);
    if (t.failed) {
        free(t.data);
        return NULL;
    }
    if (out_len) *out_len = t.len;
    return t.data;
}

void snapshot_tree_format_time(const Timestamp* t, const char* format, char* out) {
    time_name(t, format, out);
}

void snapshot_tree_format_size(uint64_t bytes, char* out, size_t cap) {
    size_text(bytes, out, cap);
}

/* The SnapshotInfo of the snapshot with this full ID, or NULL */
static const SnapshotInfo* find_info(const SnapshotTree* tree, const char* id) {
    for (size_t i = 0; tree && id && i < tree->snapshot_count; ++i) {
        if (strcmp(tree->info[i]->id, id) == 0) return tree->info[i];
    }
    return NULL;
}

int snapshot_tree_snapshot_time(const SnapshotTree* tree, const char* id, Timestamp* out) {
    const SnapshotInfo* info = find_info(tree, id);
    if (info && out) *out = info->time;
    return info != NULL;
}

const char* snapshot_tree_snapshot_host(const SnapshotTree* tree, const char* id) {
    const SnapshotInfo* info = find_info(tree, id);
    return info ? info->host : NULL;
}

/* printf into a CsvText, at most 1023 bytes: only for text of a known, short length */
static void text_printf(CsvText* t, const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0) csv_put(t, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

/*
 * `len` bytes of repository content, a control character as '?': a line break in a host or user
 * name must not make a line of its own that looks like one of the plugin's ("Size: ...")
 */
static void text_clean(CsvText* t, const char* text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        csv_put(t, c < 0x20 || c == 0x7F ? "?" : text + i, 1);
    }
}

/* "Label: ", a value of any length and the end of the line */
static void text_value(CsvText* t, const char* label, const char* value) {
    csv_put(t, label, strlen(label));
    text_clean(t, value, strlen(value));
    csv_put(t, "\n", 1);
}

/* "Label: " and a text of several lines (LF-separated), each further line indented below the first */
static void text_lines(CsvText* t, const char* label, const char* lines) {
    csv_put(t, label, strlen(label));
    for (const char* p = lines; *p;) {
        size_t n = strcspn(p, "\n");
        text_clean(t, p, n);
        p += n;
        if (*p == '\n') {
            csv_put(t, "\n    ", 5);
            ++p;
        }
    }
    csv_put(t, "\n", 1);
}

/* A duration in 100 ns ticks as "0.7 s", "2 min 5 s" or "1 h 2 min" */
static void duration_text(uint64_t ticks, char* out, size_t cap) {
    uint64_t tenths = ticks / 1000000u, seconds = ticks / 10000000u;
    if (seconds < 60) {
        snprintf(out, cap, "%u.%u s", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
    } else if (seconds < 3600) {
        snprintf(out, cap, "%u min %u s", (unsigned)(seconds / 60u), (unsigned)(seconds % 60u));
    } else {
        snprintf(out, cap, "%llu h %u min", (unsigned long long)(seconds / 3600u), (unsigned)(seconds / 60u % 60u));
    }
}

char* snapshot_tree_describe(const SnapshotTree* tree, const char* id) {
    const SnapshotInfo* s = find_info(tree, id);
    if (!s) return NULL;
    CsvText t = {NULL, 0, 0, 0};
    char when[SNAPSHOT_TREE_TIME_FORMAT_MAX * 2 + 1], size[32], packed[32], took[32];
    time_name(&s->time, "YYYY-MM-DD hh:mm:ss zzzz", when);
    text_printf(&t, "Snapshot %.8s of ", s->id); /* The ID is lowercase hexadecimal: checked when built */
    text_value(&t, "", s->host[0] ? s->host : "(no host)");
    text_printf(&t, "ID: %s\n", s->id);
    text_printf(&t, "Time: %s\n", when);
    if (s->username[0]) text_value(&t, "User: ", s->username);
    if (s->paths[0]) text_lines(&t, "Paths: ", s->paths);
    if (s->tags[0]) text_lines(&t, "Tags: ", s->tags);
    if (s->has_size) {
        size_text(s->size, size, sizeof(size));
        if (s->has_counts) {
            text_printf(&t, "Size: %s in %llu files\n", size, (unsigned long long)s->total_files);
        } else {
            text_printf(&t, "Size: %s\n", size);
        }
    }
    if (s->has_counts) {
        text_printf(&t, "Files: %llu new, %llu changed, %llu unmodified\n", (unsigned long long)s->files_new,
                    (unsigned long long)s->files_changed, (unsigned long long)s->files_unmodified);
        text_printf(&t, "Folders: %llu new, %llu changed, %llu unmodified\n", (unsigned long long)s->dirs_new,
                    (unsigned long long)s->dirs_changed, (unsigned long long)s->dirs_unmodified);
        size_text(s->data_added, size, sizeof(size));
        size_text(s->data_added_packed, packed, sizeof(packed));
        text_printf(&t, "Added to the repository: %s (%s stored)\n", size, packed);
    }
    if (s->has_backup_times && s->backup_end.utc_ticks >= s->backup_start.utc_ticks) {
        duration_text(s->backup_end.utc_ticks - s->backup_start.utc_ticks, took, sizeof(took));
        text_printf(&t, "Backup took: %s\n", took);
    }
    if (s->parent[0]) {
        char parent[SNAPSHOT_TREE_SHORT_ID_CHARS + 1];
        snprintf(parent, sizeof(parent), "%s", s->parent); /* restic's short form; not checked, so cleaned */
        text_value(&t, "Parent: ", parent);
    }
    if (s->program_version[0]) text_value(&t, "Program: ", s->program_version);
    if (t.failed) {
        free(t.data);
        return NULL;
    }
    return t.data;
}

static int ascii_casecmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || ca == 0) return ca - cb;
    }
}

/* Bytes of text, not NUL-terminated */
typedef struct {
    const char* p;
    size_t len;
} Span;

/* `a` and `b` byte for byte (`exact`) or ignoring ASCII case */
static int span_eq(Span a, Span b, int exact) {
    if (a.len != b.len) return 0;
    if (exact) return memcmp(a.p, b.p, a.len) == 0;
    for (size_t i = 0; i < a.len; ++i) {
        int ca = (unsigned char)a.p[i], cb = (unsigned char)b.p[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return 0;
    }
    return 1;
}

/* A network share volume at the start of a path */
typedef struct {
    Span server, share;
    size_t len; /* The whole volume, prefix included */
} UncVolume;

/*
 * The server or share name at p[*i], up to a separator or the end, *i moved past it: 0 when it
 * cannot name a folder (empty, only dots and spaces, a control character or one of : * ? " < > |,
 * which no Windows server or share name has)
 */
static int unc_component(const char* p, size_t len, size_t* i, Span* out) {
    out->p    = p + *i;
    int plain = 0;
    while (*i < len && p[*i] != '\\' && p[*i] != '/') {
        unsigned char c = (unsigned char)p[*i];
        if (c < 0x20 || c == 0x7F || strchr(":*?\"<>|", c)) return 0;
        if (c != '.' && c != ' ') plain = 1;
        ++*i;
    }
    out->len = (size_t)(p + *i - out->p);
    return plain;
}

/*
 * The "\\server\share" volume `p` (`len` bytes) starts with, as Go's filepath.VolumeName finds it in
 * the backslash form restic stores, followed by a separator or nothing: its length, or 0. A device
 * path ("\\?\UNC\server\share", "\\.\UNC\...") is none: its server "?" or "." is refused
 */
static size_t parse_unc(const char* p, size_t len, UncVolume* v) {
    if (len < 3 || p[0] != '\\' || p[1] != '\\') return 0;
    size_t i = 2;
    if (!unc_component(p, len, &i, &v->server) || i >= len || p[i] != '\\') return 0;
    ++i;
    if (!unc_component(p, len, &i, &v->share)) return 0;
    v->len = i;
    return i;
}

size_t snapshot_tree_unc_volume(const char* path, size_t len) {
    UncVolume v;
    return path ? parse_unc(path, len, &v) : 0;
}

/*
 * The next network share volume of a snapshot's paths (one per line) from *line on, *line moved
 * past it; *seen counts the volumes found, none beyond SNAPSHOT_TREE_UNC_MAX_VOLUMES
 */
static int next_unc_volume(const char** line, UncVolume* v, const char** text, size_t* seen) {
    while (**line != '\0' && *seen < SNAPSHOT_TREE_UNC_MAX_VOLUMES) {
        const char* start = *line;
        size_t n          = strcspn(start, "\n");
        *line             = start + n + (start[n] == '\n');
        if (parse_unc(start, n, v)) {
            *text = start;
            ++*seen;
            return 1;
        }
    }
    return 0;
}

/*
 * The first volume of the snapshot's paths on `server` (any when its p is NULL) with `share` (any
 * when its p is NULL), byte for byte if there is one, else ignoring ASCII case; 0 when none
 */
static int find_unc(const SnapshotInfo* info, Span server, Span share, const char** text, size_t* len) {
    for (int exact = 1; exact >= 0; --exact) {
        const char* line = info->paths;
        size_t seen      = 0;
        UncVolume v;
        while (next_unc_volume(&line, &v, text, &seen)) {
            if ((!server.p || span_eq(v.server, server, exact)) && (!share.p || span_eq(v.share, share, exact))) {
                *len = v.len;
                return 1;
            }
        }
    }
    return 0;
}

/* The next component of a "/a/b" path at *p, *p moved past it; 0 at the end */
static int next_component(const char** p, Span* out) {
    if (**p != '/') return 0;
    out->p = ++*p;
    while (**p != '\0' && **p != '/') ++*p;
    out->len = (size_t)(*p - out->p);
    return out->len > 0;
}

/*
 * Splits a path below "/UNC" into the server and share it names (p NULL for those it does not);
 * 0 when it is not below "/UNC". *rest is what follows the share.
 */
static int split_unc_path(const char* path, Span* server, Span* share, const char** rest) {
    static const Span unc = {SNAPSHOT_TREE_UNC_NAME, sizeof(SNAPSHOT_TREE_UNC_NAME) - 1};
    Span first;
    const char* p = path;
    server->p = share->p = NULL;
    server->len = share->len = 0;
    if (!next_component(&p, &first) || !span_eq(first, unc, 0)) return 0;
    if (next_component(&p, server)) {
        if (!next_component(&p, share)) share->p = NULL;
    } else {
        server->p = NULL;
    }
    *rest = p;
    return 1;
}

SnapshotUncKind snapshot_tree_unc_path(const SnapshotTree* tree, const char* id, const char* path, char* out,
                                       size_t cap) {
    if (out && cap > 0) out[0] = '\0';
    const SnapshotInfo* info = find_info(tree, id);
    if (!info || !path || !out) return SNAPSHOT_UNC_NONE;
    static const Span any = {NULL, 0};
    const char* text = NULL;
    size_t len       = 0;
    if (strcmp(path, "/") == 0) return find_unc(info, any, any, &text, &len) ? SNAPSHOT_UNC_ROOT : SNAPSHOT_UNC_NONE;

    Span server, share;
    const char* rest = NULL;
    if (!split_unc_path(path, &server, &share, &rest) || !find_unc(info, server, share, &text, &len)) {
        return SNAPSHOT_UNC_NONE;
    }
    if (!share.p) return SNAPSHOT_UNC_FOLDER;
    size_t rest_len = strlen(rest);
    if (len + rest_len + 2 > cap) return SNAPSHOT_UNC_TOO_LONG;
    out[0] = '/';
    memcpy(out + 1, text, len);
    memcpy(out + 1 + len, rest, rest_len + 1);
    return SNAPSHOT_UNC_MAPPED;
}

size_t snapshot_tree_unc_list(const SnapshotTree* tree, const char* id, const char* path,
                              void (*add)(void* ctx, const char* name, size_t len), void* ctx) {
    const SnapshotInfo* info = find_info(tree, id);
    Span server, share;
    const char* rest = NULL;
    if (!info || !path || !split_unc_path(path, &server, &share, &rest) || share.p) return 0;
    /* Each name once, for the first volume of that name: quadratic in the names, hence their limit */
    Span* names = (Span*)malloc(SNAPSHOT_TREE_UNC_MAX_VOLUMES * sizeof(Span));
    if (!names) return 0;
    size_t count     = 0;
    size_t seen      = 0;
    const char* line = info->paths;
    const char* text = NULL;
    UncVolume v;
    while (next_unc_volume(&line, &v, &text, &seen)) {
        if (server.p && !span_eq(v.server, server, 0)) continue;
        Span name  = server.p ? v.share : v.server;
        int listed = 0;
        for (size_t i = 0; i < count && !listed; ++i) listed = span_eq(names[i], name, 0);
        if (listed) continue;
        names[count++] = name;
        if (add) add(ctx, name.p, name.len);
    }
    free(names);
    return count;
}

const SnapshotNode* snapshot_tree_child(const SnapshotNode* dir, const char* name) {
    if (!dir || !name || dir->snapshot_id) return NULL;
    size_t lo = 0, hi = dir->child_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, dir->children[mid]->name);
        if (c == 0) return dir->children[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    for (size_t i = 0; i < dir->child_count; ++i) {
        if (ascii_casecmp(name, dir->children[i]->name) == 0) return dir->children[i];
    }
    return NULL;
}
