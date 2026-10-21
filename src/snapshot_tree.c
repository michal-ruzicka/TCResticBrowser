#include "snapshot_tree.h"
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
};

/* One usable entry of the snapshots array */
typedef struct {
    char id[SNAPSHOT_TREE_ID_CHARS + 1];
    Timestamp time;
    const char* hostname;             /* Points into the JSON document */
    const JsonValue* tags;            /* JSON array or NULL */
} SnapshotRecord;

/* Open-addressing hash set of (parent, name) -> node, so inserting N snapshots stays O(N) */
typedef struct {
    BuildNode** slots;
    size_t cap, used;
} NodeIndex;

struct SnapshotTree {
    BuildNode* root;
    size_t snapshot_count;
    char** ids; /* Owned copies of the full IDs the nodes point to */
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
        n++;
    }
    qsort(records, n, sizeof(SnapshotRecord), compare_records);
    return n;
}

/* "2006-01-02_15-04-05": restic mount's colon-free time template, in the snapshot's own zone */
static void time_name(const Timestamp* t, char* out) {
    int v[6] = {t->year, t->month, t->day, t->hour, t->minute, t->second};
    static const char seps[6] = {'-', '-', '_', '-', '-', '\0'};
    char* p = out;
    for (int i = 0; i < 6; ++i) {
        int width = i == 0 ? 4 : 2;
        for (int d = width - 1; d >= 0; --d) {
            p[d] = (char)('0' + v[i] % 10);
            v[i] /= 10;
        }
        p += width;
        *p++ = seps[i];
    }
}

static SnapshotTreeStatus build_from_records(const SnapshotRecord* records, size_t count, SnapshotTree* tree) {
    NodeIndex idx = {NULL, 0, 0};
    SnapshotTreeStatus status = SNAPSHOT_TREE_NO_MEMORY;
    BuildNode* root = (BuildNode*)calloc(1, sizeof(BuildNode));
    tree->root = root;
    tree->ids = count ? (char**)calloc(count, sizeof(char*)) : NULL;
    if (!root || (count && !tree->ids) || !(root->name = (char*)calloc(1, 1))) goto done;
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
        tree->ids[tree->snapshot_count++] = id;

        char when[24], short_id[SNAPSHOT_TREE_SHORT_ID_CHARS + 1];
        time_name(&rec->time, when);
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
    if (finish_node(root)) status = SNAPSHOT_TREE_OK;
done:
    free(idx.slots);
    return status;
}

SnapshotTreeStatus snapshot_tree_build(const char* json, size_t len, SnapshotTree** out) {
    if (!out) return SNAPSHOT_TREE_BAD_JSON;
    *out = NULL;
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
    if (tree && (n == 0 || records)) {
        size_t count = n ? collect_records(root, records) : 0;
        status = build_from_records(records, count, tree);
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
    free(tree);
}

const SnapshotNode* snapshot_tree_root(const SnapshotTree* tree) {
    return tree ? &tree->root->pub : NULL;
}

size_t snapshot_tree_snapshot_count(const SnapshotTree* tree) {
    return tree ? tree->snapshot_count : 0;
}

static int ascii_casecmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb || ca == 0) return ca - cb;
    }
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
