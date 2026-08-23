/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-frame-cache.h"

#include <libavutil/imgutils.h>
#include <util/bmem.h>

#include <limits.h>
#include <string.h>

static size_t frame_bytes(const AVFrame *frame)
{
	if (!frame || frame->width <= 0 || frame->height <= 0)
		return 0;

	const int size = av_image_get_buffer_size((enum AVPixelFormat)frame->format, frame->width, frame->height, 1);
	if (size > 0)
		return (size_t)size;

	/* Decoder output is normally 4:2:0 software video. Keep a conservative
	 * fallback for unusual formats rather than disabling the cache entirely. */
	const uint64_t fallback = (uint64_t)frame->width * (uint64_t)frame->height * 4ULL;
	return fallback > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)fallback;
}

static void remove_entry(struct sr_frame_cache *cache, size_t index)
{
	if (!cache || index >= cache->count)
		return;

	struct sr_frame_cache_entry *entry = &cache->entries[index];
	if (entry->bytes <= cache->bytes)
		cache->bytes -= entry->bytes;
	else
		cache->bytes = 0;
	av_frame_free(&entry->frame);

	cache->count--;
	if (index != cache->count)
		cache->entries[index] = cache->entries[cache->count];
	memset(&cache->entries[cache->count], 0, sizeof(cache->entries[cache->count]));
}

static size_t lru_index(const struct sr_frame_cache *cache)
{
	size_t oldest = 0;
	for (size_t i = 1; i < cache->count; i++) {
		if (cache->entries[i].last_use < cache->entries[oldest].last_use)
			oldest = i;
	}
	return oldest;
}

static bool reserve_entry(struct sr_frame_cache *cache)
{
	if (cache->count < cache->capacity)
		return true;

	const size_t next_capacity = cache->capacity ? cache->capacity * 2 : 16;
	if (next_capacity < cache->capacity || next_capacity > SIZE_MAX / sizeof(*cache->entries))
		return false;

	struct sr_frame_cache_entry *next = brealloc(cache->entries, next_capacity * sizeof(*cache->entries));
	if (!next)
		return false;

	memset(next + cache->capacity, 0, (next_capacity - cache->capacity) * sizeof(*next));
	cache->entries = next;
	cache->capacity = next_capacity;
	return true;
}

void sr_frame_cache_init(struct sr_frame_cache *cache, size_t max_bytes)
{
	if (!cache)
		return;
	memset(cache, 0, sizeof(*cache));
	cache->max_bytes = max_bytes;
}

void sr_frame_cache_clear(struct sr_frame_cache *cache)
{
	if (!cache)
		return;

	for (size_t i = 0; i < cache->count; i++)
		av_frame_free(&cache->entries[i].frame);
	cache->count = 0;
	cache->bytes = 0;
	cache->clock = 0;
	cache->hits = 0;
	cache->misses = 0;
	cache->stores = 0;
	cache->evictions = 0;
}

void sr_frame_cache_free(struct sr_frame_cache *cache)
{
	if (!cache)
		return;
	sr_frame_cache_clear(cache);
	bfree(cache->entries);
	memset(cache, 0, sizeof(*cache));
}

AVFrame *sr_frame_cache_find(struct sr_frame_cache *cache, uint64_t key)
{
	if (!cache)
		return NULL;

	for (size_t i = 0; i < cache->count; i++) {
		if (cache->entries[i].key != key)
			continue;
		cache->entries[i].last_use = ++cache->clock;
		cache->hits++;
		return cache->entries[i].frame;
	}

	cache->misses++;
	return NULL;
}

bool sr_frame_cache_store(struct sr_frame_cache *cache, uint64_t key, const AVFrame *frame)
{
	if (!cache || !frame || !cache->max_bytes)
		return false;

	const size_t bytes = frame_bytes(frame);
	if (!bytes || bytes > cache->max_bytes)
		return false;

	AVFrame *copy = av_frame_clone(frame);
	if (!copy)
		return false;

	for (size_t i = 0; i < cache->count; i++) {
		if (cache->entries[i].key != key)
			continue;

		if (cache->entries[i].bytes <= cache->bytes)
			cache->bytes -= cache->entries[i].bytes;
		else
			cache->bytes = 0;
		av_frame_free(&cache->entries[i].frame);

		while (cache->count > 1 && cache->bytes + bytes > cache->max_bytes) {
			size_t evict = lru_index(cache);
			if (evict == i)
				evict = (i == 0) ? 1 : 0;
			remove_entry(cache, evict);
			cache->evictions++;
			if (evict < i)
				i--;
		}

		cache->entries[i].frame = copy;
		cache->entries[i].bytes = bytes;
		cache->entries[i].last_use = ++cache->clock;
		cache->bytes += bytes;
		cache->stores++;
		return true;
	}

	while (cache->count && cache->bytes + bytes > cache->max_bytes) {
		remove_entry(cache, lru_index(cache));
		cache->evictions++;
	}

	if (!reserve_entry(cache)) {
		av_frame_free(&copy);
		return false;
	}

	struct sr_frame_cache_entry *entry = &cache->entries[cache->count++];
	entry->key = key;
	entry->frame = copy;
	entry->bytes = bytes;
	entry->last_use = ++cache->clock;
	cache->bytes += bytes;
	cache->stores++;
	return true;
}
