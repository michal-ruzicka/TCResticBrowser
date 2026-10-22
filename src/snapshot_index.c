#include "snapshot_index.h"
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "timestamp.h"

typedef struct {
    size_t len;    /* Length of the folder's path, a prefix of SnapshotIndex.path */
    uint32_t node;
} StackItem;

typedef struct {
    uint32_t parent;   /* Index of the folder holding it (the root holds itself) */
    uint32_t name;     /* Offset of the NUL-terminated name in the name pool */
    uint64_t size;
    uint64_t mtime, atime, ctime;
    uint8_t is_dir;
} Node;

struct SnapshotIndex {
    size_t max_bytes;
    SnapshotIndexStatus status;
    int finished;

    Node* nodes;
    size_t node_count, node_cap;
    char* pool;
    size_t pool_len, pool_cap;

    /* While adding: the folders from the root to the last folder seen (restic walks depth first) */
    char* path;              /* That last folder's path */
    size_t path_cap;
    StackItem* stack;
    size_t depth, stack_cap;

    /* After finishing: the children of node i are child_list[child_start[i] .. child_start[i + 1]) */
    uint32_t* child_start;
    uint32_t* child_list;
    int sorted;              /* Every folder's names strictly ascending: binary search allowed */
};

static size_t memory_of(const SnapshotIndex* x) {
    return sizeof(*x) + x->node_cap * sizeof(Node) + x->pool_cap + x->path_cap +
           x->stack_cap * sizeof(StackItem) +
           (x->child_start ? (x->node_count + 1) * sizeof(uint32_t) : 0) +
           (x->child_list ? x->node_count * sizeof(uint32_t) : 0);
}

/* Grows a buffer to hold at least `need` elements; 0 when out of memory or over the limit */
static int grow(SnapshotIndex* x, void** buf, size_t* cap, size_t need, size_t elem) {
    if (need <= *cap) return 1;
    size_t cap_new = *cap ? *cap : 64;
    while (cap_new < need) cap_new *= 2;
    if (x->max_bytes && memory_of(x) + (cap_new - *cap) * elem > x->max_bytes) {
        x->status = SNAPSHOT_INDEX_TOO_LARGE;
        return 0;
    }
    void* p = realloc(*buf, cap_new * elem);
    if (!p) {
        x->status = SNAPSHOT_INDEX_NO_MEMORY;
        return 0;
    }
    *buf = p;
    *cap = cap_new;
    return 1;
}

static int add_name(SnapshotIndex* x, const char* name, size_t len, uint32_t* out_offset) {
    if (x->pool_len + len + 1 > UINT32_MAX) {
        x->status = SNAPSHOT_INDEX_TOO_LARGE;
        return 0;
    }
    if (!grow(x, (void**)&x->pool, &x->pool_cap, x->pool_len + len + 1, 1)) return 0;
    memcpy(x->pool + x->pool_len, name, len);
    x->pool[x->pool_len + len] = '\0';
    *out_offset = (uint32_t)x->pool_len;
    x->pool_len += len + 1;
    return 1;
}

static uint64_t node_time(const JsonValue* obj, const char* key) {
    Timestamp t;
    return timestamp_parse(json_string(json_object_get(obj, key)), &t) ? t.utc_ticks : SNAPSHOT_INDEX_NO_TIME;
}

SnapshotIndex* snapshot_index_new(size_t max_bytes) {
    SnapshotIndex* x = (SnapshotIndex*)calloc(1, sizeof(SnapshotIndex));
    if (!x) return NULL;
    x->max_bytes = max_bytes;
    /* Node 0: the snapshot root "/" */
    uint32_t name = 0;
    if (!grow(x, (void**)&x->nodes, &x->node_cap, 1, sizeof(Node)) || !add_name(x, "", 0, &name)) {
        snapshot_index_free(x);
        return NULL;
    }
    memset(&x->nodes[0], 0, sizeof(Node));
    x->nodes[0].is_dir = 1;
    x->nodes[0].mtime = x->nodes[0].atime = x->nodes[0].ctime = SNAPSHOT_INDEX_NO_TIME;
    x->node_count = 1;
    return x;
}

/*
 * The folder holding `path` (whose parent part is its first parent_len bytes), found on the stack
 * of folders on the way from the root to the last one restic listed; the folders restic has
 * finished with are dropped. -1 if the parent is not there, which leaves the stack as it was.
 */
static long long find_parent(SnapshotIndex* x, const char* path, size_t parent_len) {
    if (parent_len == 0) { /* "/name": restic is back in the root */
        x->depth = 0;
        return 0;
    }
    for (size_t k = x->depth; k > 0; --k) {
        size_t len = x->stack[k - 1].len; /* Every stacked path is a prefix of x->path */
        if (len > parent_len || memcmp(x->path, path, len) != 0 || path[len] != '/') continue;
        if (len < parent_len) return -1; /* An ancestor, but the parent itself was never listed */
        x->depth = k;
        return x->stack[k - 1].node;
    }
    return -1;
}

SnapshotIndexStatus snapshot_index_add_line(SnapshotIndex* x, const char* line, size_t len) {
    if (!x || !line) return SNAPSHOT_INDEX_BAD_JSON;
    if (x->status != SNAPSHOT_INDEX_OK) return x->status;
    if (x->finished) return x->status = SNAPSHOT_INDEX_BAD_JSON;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        len--;
    }
    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }
    if (len == 0) return SNAPSHOT_INDEX_OK;

    JsonDoc* doc = json_parse(line, len);
    const JsonValue* obj = json_root(doc);
    if (!obj || obj->type != JSON_OBJECT) {
        json_free(doc);
        return x->status = SNAPSHOT_INDEX_BAD_JSON;
    }
    const char* msg_type = json_string(json_object_get(obj, "message_type"));
    if (!msg_type || strcmp(msg_type, "node") != 0) { /* The snapshot line, or anything newer */
        json_free(doc);
        return SNAPSHOT_INDEX_OK;
    }

    const char* path = json_string(json_object_get(obj, "path"));
    const char* name = json_string(json_object_get(obj, "name"));
    const char* type = json_string(json_object_get(obj, "type"));
    size_t path_len  = path ? strlen(path) : 0;
    if (path_len < 2 || path[0] != '/' || path[path_len - 1] == '/') {
        json_free(doc);
        return x->status = SNAPSHOT_INDEX_BAD_JSON;
    }
    size_t parent_len = path_len;
    while (path[parent_len - 1] != '/') parent_len--;
    parent_len--; /* Without the separator: "" for "/name" */
    if (!name) name = path + parent_len + 1;

    long long parent = find_parent(x, path, parent_len);
    if (parent < 0 || !x->nodes[parent].is_dir) { /* Never happens with restic's walk order */
        json_free(doc);
        return SNAPSHOT_INDEX_OK;
    }
    if (x->node_count >= UINT32_MAX) {
        json_free(doc);
        return x->status = SNAPSHOT_INDEX_TOO_LARGE;
    }

    Node n;
    memset(&n, 0, sizeof(n));
    n.parent = (uint32_t)parent;
    n.is_dir = type && strcmp(type, "dir") == 0;
    unsigned long long size = 0;
    if (!n.is_dir && json_u64(json_object_get(obj, "size"), &size)) n.size = size;
    n.mtime = node_time(obj, "mtime");
    n.atime = node_time(obj, "atime");
    n.ctime = node_time(obj, "ctime");
    if (!add_name(x, name, strlen(name), &n.name) ||
        !grow(x, (void**)&x->nodes, &x->node_cap, x->node_count + 1, sizeof(Node))) {
        json_free(doc);
        return x->status;
    }
    x->nodes[x->node_count] = n;

    if (n.is_dir) { /* Its children follow: remember it on the way down */
        if (!grow(x, (void**)&x->path, &x->path_cap, path_len + 1, 1) ||
            !grow(x, (void**)&x->stack, &x->stack_cap, x->depth + 1, sizeof(StackItem))) {
            json_free(doc);
            return x->status;
        }
        memcpy(x->path, path, path_len + 1);
        x->stack[x->depth].len  = path_len;
        x->stack[x->depth].node = (uint32_t)x->node_count;
        x->depth++;
    }
    x->node_count++;
    json_free(doc);
    return SNAPSHOT_INDEX_OK;
}

SnapshotIndexStatus snapshot_index_finish(SnapshotIndex* x) {
    if (!x) return SNAPSHOT_INDEX_BAD_JSON;
    if (x->status != SNAPSHOT_INDEX_OK || x->finished) return x->status;

    /* The walk state is no longer needed */
    free(x->path);
    free(x->stack);
    x->path  = NULL;
    x->stack = NULL;
    x->path_cap = x->stack_cap = x->depth = 0;

    size_t n = x->node_count;
    if (x->max_bytes && memory_of(x) + (2 * n + 1) * sizeof(uint32_t) > x->max_bytes) {
        return x->status = SNAPSHOT_INDEX_TOO_LARGE;
    }
    x->child_start = (uint32_t*)calloc(n + 1, sizeof(uint32_t));
    x->child_list  = (uint32_t*)malloc(n * sizeof(uint32_t));
    uint32_t* fill = (uint32_t*)calloc(n, sizeof(uint32_t));
    if (!x->child_start || !x->child_list || !fill) {
        free(fill);
        return x->status = SNAPSHOT_INDEX_NO_MEMORY;
    }
    /* Counting sort by parent keeps every folder's entries in restic's order */
    for (size_t i = 1; i < n; ++i) x->child_start[x->nodes[i].parent + 1]++;
    for (size_t i = 0; i < n; ++i) x->child_start[i + 1] += x->child_start[i];
    for (size_t i = 1; i < n; ++i) {
        uint32_t p = x->nodes[i].parent;
        x->child_list[x->child_start[p] + fill[p]++] = (uint32_t)i;
    }
    free(fill);

    x->sorted = 1;
    for (size_t d = 0; d < n && x->sorted; ++d) {
        for (uint32_t k = x->child_start[d] + 1; k < x->child_start[d + 1]; ++k) {
            if (strcmp(x->pool + x->nodes[x->child_list[k - 1]].name, x->pool + x->nodes[x->child_list[k]].name) >= 0) {
                x->sorted = 0;
                break;
            }
        }
    }
    x->finished = 1;
    return SNAPSHOT_INDEX_OK;
}

size_t snapshot_index_memory(const SnapshotIndex* x) {
    return x ? memory_of(x) : 0;
}

size_t snapshot_index_entry_count(const SnapshotIndex* x) {
    return x ? x->node_count - 1 : 0;
}

void snapshot_index_free(SnapshotIndex* x) {
    if (!x) return;
    free(x->nodes);
    free(x->pool);
    free(x->path);
    free(x->stack);
    free(x->child_start);
    free(x->child_list);
    free(x);
}

/* The child of folder `dir` named exactly name[0..len), or -1 */
static long long find_child(const SnapshotIndex* x, uint32_t dir, const char* name, size_t len) {
    uint32_t lo = x->child_start[dir], hi = x->child_start[dir + 1];
    if (x->sorted) {
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            const char* candidate = x->pool + x->nodes[x->child_list[mid]].name;
            int c = strncmp(name, candidate, len);
            if (c == 0) c = candidate[len] == '\0' ? 0 : -1;
            if (c == 0) return x->child_list[mid];
            if (c < 0) hi = mid;
            else lo = mid + 1;
        }
        return -1;
    }
    for (uint32_t k = lo; k < hi; ++k) {
        const char* candidate = x->pool + x->nodes[x->child_list[k]].name;
        if (strncmp(name, candidate, len) == 0 && candidate[len] == '\0') return x->child_list[k];
    }
    return -1;
}

long long snapshot_index_find_dir(const SnapshotIndex* x, const char* path) {
    if (!x || !x->finished || !path || path[0] != '/') return -1;
    long long node = 0;
    const char* p = path + 1;
    while (*p) {
        size_t len = strcspn(p, "/");
        if (len == 0) return -1;
        node = find_child(x, (uint32_t)node, p, len);
        if (node < 0 || !x->nodes[node].is_dir) return -1;
        p += len;
        if (*p == '/' && *++p == '\0') return -1; /* A trailing separator */
    }
    return node;
}

size_t snapshot_index_child_count(const SnapshotIndex* x, long long dir) {
    if (!x || !x->finished || dir < 0 || (size_t)dir >= x->node_count) return 0;
    return x->child_start[dir + 1] - x->child_start[dir];
}

SnapshotIndexEntry snapshot_index_child(const SnapshotIndex* x, long long dir, size_t i) {
    SnapshotIndexEntry e;
    memset(&e, 0, sizeof(e));
    if (i >= snapshot_index_child_count(x, dir)) return e;
    const Node* n = &x->nodes[x->child_list[x->child_start[dir] + i]];
    e.name   = x->pool + n->name;
    e.is_dir = n->is_dir;
    e.size   = n->size;
    e.mtime  = n->mtime;
    e.atime  = n->atime;
    e.ctime  = n->ctime;
    return e;
}
