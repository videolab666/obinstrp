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

#include <libavcodec/avcodec.h>
#include <plugin-support.h>

#include "sr-segment-format.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sr_segment_reader;

struct sr_segment_stream_info {
	enum AVCodecID codec_id;
	uint32_t camera_hash;
	uint32_t sequence;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t segment_start_ns;
	uint32_t segment_flags;
	const uint8_t *extradata; /* borrowed; valid while reader lives */
	int extradata_size;
	size_t indexed_packets;
};

/* Opens a finalized .srseg/.sridx pair or the active .part pair. The index is
 * loaded as a snapshot; call refresh_index to see records appended later. */
struct sr_segment_reader *sr_segment_reader_open(const char *segment_path, const char *index_path);
void sr_segment_reader_close(struct sr_segment_reader *reader);

bool sr_segment_reader_get_info(const struct sr_segment_reader *reader, struct sr_segment_stream_info *info);
bool sr_segment_reader_refresh_index(struct sr_segment_reader *reader);
size_t sr_segment_reader_entry_count(const struct sr_segment_reader *reader);
bool sr_segment_reader_entry_at(const struct sr_segment_reader *reader, size_t position, struct sr_index_entry *entry);

/* Finds the newest indexed packet at/before timestamp and returns both the
 * entry and its position. If keyframe_only is true, walks back to the nearest
 * IDR/key packet. Either output pointer may be NULL. */
bool sr_segment_reader_find_position(const struct sr_segment_reader *reader, uint64_t timestamp_ns,
				     bool keyframe_only, size_t *position, struct sr_index_entry *entry);

/* Convenience wrapper when the caller does not need the position. */
bool sr_segment_reader_find(const struct sr_segment_reader *reader, uint64_t timestamp_ns, bool keyframe_only,
			    struct sr_index_entry *entry);

/* Reads and reconstructs the encoded video AVPacket referenced by an index
 * entry. The returned packet is owned by the caller and must be freed with
 * av_packet_free(). */
bool sr_segment_reader_read_video_packet(struct sr_segment_reader *reader, const struct sr_index_entry *entry,
					 AVPacket **packet, uint64_t *timestamp_ns, uint8_t *record_flags);

#ifdef __cplusplus
}
#endif
