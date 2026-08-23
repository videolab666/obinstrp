/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "sr-audio-format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_master_audio_reader;

struct sr_master_audio_segment_info {
	enum AVCodecID codec_id;
	uint32_t sequence;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bit_rate;
	uint32_t segment_flags;
	uint64_t segment_start_ns;
	const uint8_t *extradata;
	int extradata_size;
	size_t indexed_packets;
};

struct sr_master_audio_reader *sr_master_audio_reader_open(const char *audio_path, const char *index_path);
void sr_master_audio_reader_close(struct sr_master_audio_reader *reader);

/* Re-snapshots a growing .sraidx.part. A trailing partial record is ignored,
 * exactly like the video SegmentReader. */
bool sr_master_audio_reader_refresh_index(struct sr_master_audio_reader *reader);

bool sr_master_audio_reader_get_info(const struct sr_master_audio_reader *reader,
				     struct sr_master_audio_segment_info *info);
size_t sr_master_audio_reader_entry_count(const struct sr_master_audio_reader *reader);
bool sr_master_audio_reader_entry_at(const struct sr_master_audio_reader *reader, size_t position,
				     struct sr_audio_index_entry *entry);

/* Finds the newest indexed AAC packet whose media timestamp is <= target. */
bool sr_master_audio_reader_find_position(const struct sr_master_audio_reader *reader, uint64_t timestamp_ns,
					  size_t *position, struct sr_audio_index_entry *entry);

/* Returns an owned AVPacket; release it with av_packet_free(). */
bool sr_master_audio_reader_read_packet(struct sr_master_audio_reader *reader, const struct sr_audio_index_entry *entry,
					AVPacket **packet, uint64_t *timestamp_ns);

#ifdef __cplusplus
}
#endif
