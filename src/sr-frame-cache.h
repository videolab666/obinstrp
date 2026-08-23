/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_frame_cache_entry {
	uint64_t key;
	AVFrame *frame;
	size_t bytes;
	uint64_t last_use;
};

struct sr_frame_cache {
	struct sr_frame_cache_entry *entries;
	size_t count;
	size_t capacity;
	size_t bytes;
	size_t max_bytes;
	uint64_t clock;
	uint64_t hits;
	uint64_t misses;
	uint64_t stores;
	uint64_t evictions;
};

/* Bounded LRU cache for decoded software AVFrames. Stored frames are cloned
 * with FFmpeg reference-counted buffers, so the decoder can continue reusing
 * its own AVFrame while cached pictures remain valid. max_bytes is a hard
 * memory budget; a single frame larger than the budget is simply not cached. */
void sr_frame_cache_init(struct sr_frame_cache *cache, size_t max_bytes);
void sr_frame_cache_clear(struct sr_frame_cache *cache);
void sr_frame_cache_free(struct sr_frame_cache *cache);

/* Returns a cache-owned frame that remains valid until the next cache mutation
 * (store/clear/free). The caller must not free or modify it. */
AVFrame *sr_frame_cache_find(struct sr_frame_cache *cache, uint64_t key);

/* Clones frame into the cache. Existing keys are replaced. */
bool sr_frame_cache_store(struct sr_frame_cache *cache, uint64_t key, const AVFrame *frame);

#ifdef __cplusplus
}
#endif
