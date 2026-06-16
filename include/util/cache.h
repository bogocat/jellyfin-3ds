/**
 * cache.h — Offline media cache on the SD card
 *
 * Completed downloads live at sdmc:/3ds/jellyfin-3ds/cache/<item_id>.<ext>
 * (.ts for video, .mp3 for audio). In-progress downloads use a .part
 * suffix and are renamed on completion, so a file with its final name is
 * always complete.
 *
 * An in-memory index (loaded once at startup, updated on add/remove)
 * backs the per-frame "is this item cached?" checks in the browse list —
 * stat() on FAT32 is far too slow to call 9x per frame.
 */

#ifndef JFIN_CACHE_H
#define JFIN_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CACHE_DIR "sdmc:/3ds/jellyfin-3ds/cache"

/** Scan the cache dir and build the in-memory index. Call once at boot. */
void cache_init(void);

/** Build the final path for an item. Returns false if it doesn't fit. */
bool cache_path(const char *item_id, const char *ext,
                char *out, size_t out_len);

/** Build the .part path used while downloading. */
bool cache_part_path(const char *item_id, const char *ext,
                     char *out, size_t out_len);

/** True if a completed cache file exists (in-memory check, no I/O). */
bool cache_has(const char *item_id, const char *ext);

/** Register a completed download in the index (call after rename). */
void cache_index_add(const char *item_id, const char *ext);

/** Delete an item's cache file and drop it from the index. */
bool cache_remove(const char *item_id, const char *ext);

/** True if the in-memory index is at capacity (no more items can be added). */
bool cache_is_full(void);

/** Total bytes of all files in the cache dir (walks the dir — not per-frame). */
uint64_t cache_total_bytes(void);

/** Delete every file in the cache dir. Returns number of files removed. */
int cache_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_CACHE_H */
