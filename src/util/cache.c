/**
 * cache.c — Offline media cache on the SD card
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "util/cache.h"
#include "util/log.h"

/* In-memory index of completed cache files ("<id>.<ext>" basenames).
 * Jellyfin item ids are 32 hex chars; exts are "ts"/"mp3". */
#define CACHE_MAX_ENTRIES 256
#define CACHE_NAME_LEN    80

/* Must fit a UUID-style id (36 chars) + '.' + ext + NUL, in case Jellyfin
 * ever emits dashed GUIDs instead of 32-char hex ids. */
_Static_assert(CACHE_NAME_LEN >= 44, "CACHE_NAME_LEN too small for UUID.ext");

static char s_index[CACHE_MAX_ENTRIES][CACHE_NAME_LEN];
static int  s_index_count;

static void make_name(const char *item_id, const char *ext,
                      char *out, size_t out_len)
{
    snprintf(out, out_len, "%s.%s", item_id, ext);
}

static int index_find(const char *name)
{
    for (int i = 0; i < s_index_count; i++)
        if (strcmp(s_index[i], name) == 0)
            return i;
    return -1;
}

void cache_init(void)
{
    s_index_count = 0;

    /* Make sure the directory chain exists (fresh install may not have
     * saved a config yet) */
    mkdir("sdmc:/3ds", 0755);
    mkdir("sdmc:/3ds/jellyfin-3ds", 0755);
    mkdir(CACHE_DIR, 0755);

    DIR *d = opendir(CACHE_DIR);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_index_count < CACHE_MAX_ENTRIES) {
        const char *n = ent->d_name;
        if (n[0] == '.') continue;
        size_t len = strlen(n);
        /* Skip in-progress leftovers from interrupted downloads */
        if (len > 5 && strcmp(n + len - 5, ".part") == 0) {
            char stale[512];
            snprintf(stale, sizeof(stale), CACHE_DIR "/%s", n);
            remove(stale);
            log_write("CACHE: removed stale partial %s", n);
            continue;
        }
        if (len < CACHE_NAME_LEN) {
            snprintf(s_index[s_index_count], CACHE_NAME_LEN, "%s", n);
            s_index_count++;
        }
    }
    closedir(d);
    log_write("CACHE: indexed %d cached item(s)", s_index_count);
}

bool cache_path(const char *item_id, const char *ext,
                char *out, size_t out_len)
{
    int n = snprintf(out, out_len, CACHE_DIR "/%s.%s", item_id, ext);
    return n > 0 && (size_t)n < out_len;
}

bool cache_part_path(const char *item_id, const char *ext,
                     char *out, size_t out_len)
{
    int n = snprintf(out, out_len, CACHE_DIR "/%s.%s.part", item_id, ext);
    return n > 0 && (size_t)n < out_len;
}

bool cache_has(const char *item_id, const char *ext)
{
    char name[CACHE_NAME_LEN];
    make_name(item_id, ext, name, sizeof(name));
    return index_find(name) >= 0;
}

void cache_index_add(const char *item_id, const char *ext)
{
    char name[CACHE_NAME_LEN];
    make_name(item_id, ext, name, sizeof(name));
    if (index_find(name) >= 0) return;
    if (s_index_count >= CACHE_MAX_ENTRIES) return;
    snprintf(s_index[s_index_count], CACHE_NAME_LEN, "%s", name);
    s_index_count++;
}

bool cache_remove(const char *item_id, const char *ext)
{
    char name[CACHE_NAME_LEN];
    make_name(item_id, ext, name, sizeof(name));
    int i = index_find(name);
    if (i < 0)
        return false; /* not in our index — don't touch the filesystem */

    s_index[i][0] = '\0';
    /* compact: move last entry into the hole */
    if (i != s_index_count - 1)
        memcpy(s_index[i], s_index[s_index_count - 1], CACHE_NAME_LEN);
    s_index_count--;

    char path[512];
    if (!cache_path(item_id, ext, path, sizeof(path)))
        return false;
    return remove(path) == 0;
}

bool cache_is_full(void)
{
    return s_index_count >= CACHE_MAX_ENTRIES;
}

uint64_t cache_total_bytes(void)
{
    uint64_t total = 0;
    DIR *d = opendir(CACHE_DIR);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), CACHE_DIR "/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0)
            total += (uint64_t)st.st_size;
    }
    closedir(d);
    return total;
}

int cache_clear(void)
{
    int removed = 0;
    DIR *d = opendir(CACHE_DIR);
    if (!d) return 0;

    /* Collect names first — deleting while iterating readdir is
     * undefined on some devoptab implementations */
    static char names[CACHE_MAX_ENTRIES][CACHE_NAME_LEN];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < CACHE_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') continue;
        snprintf(names[count], CACHE_NAME_LEN, "%s", ent->d_name);
        count++;
    }
    closedir(d);

    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), CACHE_DIR "/%s", names[i]);
        if (remove(path) == 0)
            removed++;
    }

    s_index_count = 0;
    log_write("CACHE: cleared %d file(s)", removed);
    return removed;
}
