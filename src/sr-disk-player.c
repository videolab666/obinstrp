/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-disk-player.h"

#include "sr-codec.h"
#include "sr-frame-cache.h"
#include "sr-segment-catalog.h"
#include "sr-segment-reader.h"

#include <obs-module.h>

#include <string.h>

#define SR_DISK_PLAYER_FRAME_CACHE_BYTES (192ULL * 1024ULL * 1024ULL)

struct sr_disk_player {
	char *session_dir;
	char *camera_name;

	struct sr_segment_descriptor *segments;
	size_t segment_count;

	struct sr_segment_reader *reader;
	struct sr_decoder *decoder;
	struct sr_frame_cache frame_cache;
	uint32_t opened_sequence;
	bool opened_active;

	/* Decoder state can remain newer than the picture just returned from the
	 * frame cache. These fields therefore describe the decoder's warm reference
	 * chain, not the operator-visible playhead. */
	int64_t current_position;
	uint64_t current_timestamp_ns;
	AVFrame *current_frame;
};

static void close_stream(struct sr_disk_player *p)
{
	if (!p)
		return;

	sr_decoder_destroy(p->decoder);
	p->decoder = NULL;
	sr_segment_reader_close(p->reader);
	p->reader = NULL;
	p->opened_sequence = 0;
	p->opened_active = false;
	p->current_position = -1;
	p->current_timestamp_ns = 0;
	av_frame_free(&p->current_frame);
}

static bool open_segment(struct sr_disk_player *p, const struct sr_segment_descriptor *segment)
{
	if (!p || !segment)
		return false;

	if (p->reader && p->opened_sequence == segment->sequence && p->opened_active == segment->active) {
		if (segment->active)
			sr_segment_reader_refresh_index(p->reader);
		return true;
	}

	close_stream(p);

	p->reader = sr_segment_reader_open(segment->segment_path, segment->index_path);
	if (!p->reader)
		return false;

	if (segment->active)
		sr_segment_reader_refresh_index(p->reader);

	struct sr_segment_stream_info info;
	if (!sr_segment_reader_get_info(p->reader, &info)) {
		close_stream(p);
		return false;
	}

	p->decoder = sr_decoder_create_replay(info.codec_id, info.extradata, info.extradata_size);
	if (!p->decoder) {
		close_stream(p);
		return false;
	}

	p->opened_sequence = segment->sequence;
	p->opened_active = segment->active;
	p->current_position = -1;
	return true;
}

static const struct sr_segment_descriptor *find_segment(struct sr_disk_player *p, uint64_t target_ns)
{
	const struct sr_segment_descriptor *segment = sr_segment_catalog_find(p->segments, p->segment_count, target_ns);
	if (segment)
		return segment;

	/* The most common miss during live recording is that the active .part
	 * has grown beyond the end timestamp captured by the previous catalog
	 * scan. Refresh once before declaring the timestamp unavailable. */
	if (!sr_disk_player_refresh(p))
		return NULL;
	return sr_segment_catalog_find(p->segments, p->segment_count, target_ns);
}

struct sr_disk_player *sr_disk_player_create(const char *session_dir, const char *camera_name)
{
	if (!session_dir || !*session_dir || !camera_name || !*camera_name)
		return NULL;

	struct sr_disk_player *p = bzalloc(sizeof(*p));
	p->session_dir = bstrdup(session_dir);
	p->camera_name = bstrdup(camera_name);
	p->current_position = -1;
	sr_frame_cache_init(&p->frame_cache, (size_t)SR_DISK_PLAYER_FRAME_CACHE_BYTES);

	if (!sr_disk_player_refresh(p)) {
		sr_disk_player_destroy(p);
		return NULL;
	}
	return p;
}

void sr_disk_player_destroy(struct sr_disk_player *p)
{
	if (!p)
		return;

	close_stream(p);
	sr_frame_cache_free(&p->frame_cache);
	sr_segment_catalog_free(p->segments, p->segment_count);
	bfree(p->session_dir);
	bfree(p->camera_name);
	bfree(p);
}

bool sr_disk_player_refresh(struct sr_disk_player *p)
{
	if (!p)
		return false;

	struct sr_segment_descriptor *segments = NULL;
	size_t count = 0;
	if (!sr_segment_catalog_scan(p->session_dir, p->camera_name, &segments, &count))
		return false;

	/* If a segment that was active has just been finalized, its path changes
	 * from .part to the finalized pair. Drop the open reader so a subsequent
	 * seek reopens the canonical files rather than holding the old handles. */
	bool keep_open = false;
	if (p->reader && p->opened_sequence) {
		for (size_t i = 0; i < count; i++) {
			if (segments[i].sequence == p->opened_sequence && segments[i].active == p->opened_active) {
				keep_open = true;
				break;
			}
		}
	}
	if (p->reader && !keep_open)
		close_stream(p);

	sr_segment_catalog_free(p->segments, p->segment_count);
	p->segments = segments;
	p->segment_count = count;
	return true;
}

bool sr_disk_player_get_bounds(const struct sr_disk_player *p, uint64_t *first_ns, uint64_t *last_ns)
{
	if (!p || !p->segment_count)
		return false;

	uint64_t first = UINT64_MAX;
	uint64_t last = 0;
	for (size_t i = 0; i < p->segment_count; i++) {
		if (p->segments[i].start_ns < first)
			first = p->segments[i].start_ns;
		if (p->segments[i].end_ns > last)
			last = p->segments[i].end_ns;
	}
	if (first == UINT64_MAX)
		return false;

	if (first_ns)
		*first_ns = first;
	if (last_ns)
		*last_ns = last;
	return true;
}

static bool output_current_clone(struct sr_disk_player *p, AVFrame **frame, uint64_t *actual_timestamp_ns)
{
	if (!p->current_frame)
		return false;

	AVFrame *copy = av_frame_clone(p->current_frame);
	if (!copy)
		return false;

	*frame = copy;
	if (actual_timestamp_ns)
		*actual_timestamp_ns = p->current_timestamp_ns;
	return true;
}

static bool cache_key(uint32_t sequence, size_t position, uint64_t *key)
{
	if (!key || position > UINT32_MAX)
		return false;
	*key = ((uint64_t)sequence << 32) | (uint64_t)(uint32_t)position;
	return true;
}

static bool output_cached_clone(struct sr_disk_player *p, size_t position, uint64_t timestamp_ns, AVFrame **frame,
				uint64_t *actual_timestamp_ns)
{
	uint64_t key = 0;
	if (!cache_key(p->opened_sequence, position, &key))
		return false;

	AVFrame *cached = sr_frame_cache_find(&p->frame_cache, key);
	if (!cached)
		return false;

	AVFrame *copy = av_frame_clone(cached);
	if (!copy)
		return false;

	*frame = copy;
	if (actual_timestamp_ns)
		*actual_timestamp_ns = timestamp_ns;
	return true;
}

bool sr_disk_player_decode_at(struct sr_disk_player *p, uint64_t target_ns, AVFrame **frame,
			      uint64_t *actual_timestamp_ns)
{
	if (!p || !frame)
		return false;
	*frame = NULL;
	if (actual_timestamp_ns)
		*actual_timestamp_ns = 0;

	const struct sr_segment_descriptor *segment = find_segment(p, target_ns);
	if (!segment)
		return false;
	if (!open_segment(p, segment))
		return false;

	/* Refresh the live index again after opening/choosing the current segment.
	 * This is cheap compared with decoding and lets a long-lived player track
	 * newly appended packets without rescanning the whole directory each tick. */
	if (segment->active)
		sr_segment_reader_refresh_index(p->reader);

	size_t target_pos = 0;
	struct sr_index_entry target_entry;
	if (!sr_segment_reader_find_position(p->reader, target_ns, false, &target_pos, &target_entry)) {
		/* The catalog may have described a live range that changed while files
		 * were rotated. One refresh/reopen attempt is safe and bounded. */
		if (!sr_disk_player_refresh(p))
			return false;
		segment = sr_segment_catalog_find(p->segments, p->segment_count, target_ns);
		if (!segment || !open_segment(p, segment) ||
		    !sr_segment_reader_find_position(p->reader, target_ns, false, &target_pos, &target_entry))
			return false;
	}

	if (p->current_position >= 0 && (size_t)p->current_position == target_pos &&
	    p->current_timestamp_ns == target_entry.timestamp_ns)
		return output_current_clone(p, frame, actual_timestamp_ns);

	/* A cached picture can satisfy backward/jog/random access without moving
	 * decoder state backwards. Keeping the newer reference chain warm makes a
	 * direction change back to forward playback cheap. */
	if (output_cached_clone(p, target_pos, target_entry.timestamp_ns, frame, actual_timestamp_ns))
		return true;

	size_t start_pos = 0;
	bool sequential = false;

	/* Sequential forward playback can retain codec reference state. Any
	 * backward/random seek starts from a keyframe and flushes the decoder. */
	if (p->current_position >= 0 && (size_t)p->current_position < target_pos) {
		start_pos = (size_t)p->current_position + 1;
		sequential = true;
	} else {
		if (!sr_segment_reader_find_position(p->reader, target_entry.timestamp_ns, true, &start_pos, NULL))
			return false;
		sr_decoder_flush(p->decoder);
		p->current_position = -1;
		p->current_timestamp_ns = 0;
		av_frame_free(&p->current_frame);
	}

	for (size_t pos = start_pos; pos <= target_pos; pos++) {
		struct sr_index_entry entry;
		if (!sr_segment_reader_entry_at(p->reader, pos, &entry))
			return false;

		AVPacket *packet = NULL;
		uint64_t packet_ts = 0;
		uint8_t record_flags = 0;
		if (!sr_segment_reader_read_video_packet(p->reader, &entry, &packet, &packet_ts, &record_flags))
			return false;

		/* A discontinuity inside an otherwise sequential run means packet
		 * reference history is no longer trustworthy. Writers currently roll to
		 * a keyframe after queue loss; enforce that invariant here as well. */
		if (sequential && (record_flags & SR_PACKET_FLAG_DISCONTINUITY)) {
			if (!(record_flags & SR_PACKET_FLAG_KEYFRAME)) {
				av_packet_free(&packet);
				blog(LOG_WARNING,
				     "Pitel Instant Replay: non-keyframe discontinuity in disk replay for '%s'",
				     p->camera_name);
				return false;
			}
			sr_decoder_flush(p->decoder);
		}

		AVFrame *decoded = NULL;
		const bool got_frame = sr_decoder_decode(p->decoder, packet, &decoded);
		av_packet_free(&packet);
		if (!got_frame || !decoded)
			continue;

		uint64_t key = 0;
		if (cache_key(p->opened_sequence, pos, &key))
			sr_frame_cache_store(&p->frame_cache, key, decoded);

		AVFrame *copy = av_frame_clone(decoded);
		if (!copy)
			return false;
		av_frame_free(&p->current_frame);
		p->current_frame = copy;
		p->current_position = (int64_t)pos;
		p->current_timestamp_ns = packet_ts;
	}

	return output_current_clone(p, frame, actual_timestamp_ns);
}

static bool segment_neighbor_timestamp(const struct sr_segment_descriptor *segment, uint64_t current_ns, int direction,
				       uint64_t *timestamp_ns)
{
	struct sr_segment_reader *reader = sr_segment_reader_open(segment->segment_path, segment->index_path);
	if (!reader)
		return false;
	if (segment->active)
		sr_segment_reader_refresh_index(reader);

	const size_t count = sr_segment_reader_entry_count(reader);
	bool found = false;
	if (count) {
		if (direction > 0) {
			struct sr_index_entry first;
			if (sr_segment_reader_entry_at(reader, 0, &first) && first.timestamp_ns > current_ns) {
				*timestamp_ns = first.timestamp_ns;
				found = true;
			} else {
				size_t pos = 0;
				if (sr_segment_reader_find_position(reader, current_ns, false, &pos, NULL)) {
					for (size_t i = pos + 1; i < count; i++) {
						struct sr_index_entry entry;
						if (sr_segment_reader_entry_at(reader, i, &entry) &&
						    entry.timestamp_ns > current_ns) {
							*timestamp_ns = entry.timestamp_ns;
							found = true;
							break;
						}
					}
				}
			}
		} else {
			struct sr_index_entry last;
			if (sr_segment_reader_entry_at(reader, count - 1, &last) && last.timestamp_ns < current_ns) {
				*timestamp_ns = last.timestamp_ns;
				found = true;
			} else {
				size_t pos = 0;
				struct sr_index_entry entry;
				if (sr_segment_reader_find_position(reader, current_ns, false, &pos, &entry)) {
					if (entry.timestamp_ns < current_ns) {
						*timestamp_ns = entry.timestamp_ns;
						found = true;
					} else {
						while (pos > 0) {
							pos--;
							if (sr_segment_reader_entry_at(reader, pos, &entry) &&
							    entry.timestamp_ns < current_ns) {
								*timestamp_ns = entry.timestamp_ns;
								found = true;
								break;
							}
						}
					}
				}
			}
		}
	}

	sr_segment_reader_close(reader);
	return found;
}

bool sr_disk_player_neighbor_timestamp(struct sr_disk_player *p, uint64_t current_ns, int direction,
				       uint64_t *timestamp_ns)
{
	if (!p || !timestamp_ns || (direction != -1 && direction != 1))
		return false;
	if (!sr_disk_player_refresh(p) || !p->segment_count)
		return false;

	if (direction > 0) {
		for (size_t i = 0; i < p->segment_count; i++) {
			if (p->segments[i].end_ns <= current_ns)
				continue;
			if (segment_neighbor_timestamp(&p->segments[i], current_ns, direction, timestamp_ns))
				return true;
		}
	} else {
		for (size_t i = p->segment_count; i > 0; i--) {
			if (p->segments[i - 1].start_ns >= current_ns)
				continue;
			if (segment_neighbor_timestamp(&p->segments[i - 1], current_ns, direction, timestamp_ns))
				return true;
		}
	}
	return false;
}
