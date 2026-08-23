/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdint.h>

#define SR_AUDIO_FORMAT_VERSION 1u
#define SR_AUDIO_MAGIC "SRAUD01"
#define SR_AUDIO_INDEX_MAGIC "SRAIX01"
#define SR_AUDIO_PACKET_MAGIC 0x53524150u /* 'SRAP' */

#define SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY 0x00000001u

/* Master replay audio is stored separately from camera video so changing the
 * replay angle never changes the audio timeline. v1 stores AAC-LC packets and
 * a compact timestamp/offset index. Like the video container, v1 is a
 * little-endian internal format; any future cross-endian implementation must
 * introduce a new version rather than changing these layouts in-place. */
#pragma pack(push, 1)
struct sr_audio_file_header {
	char magic[8];
	uint32_t version;
	uint32_t codec_id;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t bit_rate;
	uint32_t flags;
	uint32_t sequence;
	uint64_t segment_start_ns;
	uint32_t extradata_size;
	uint32_t reserved;
};

struct sr_audio_packet_header {
	uint32_t magic;
	uint32_t payload_size;
	uint64_t timestamp_ns;
	int64_t pts;
	int64_t dts;
	int64_t duration;
};

struct sr_audio_index_header {
	char magic[8];
	uint32_t version;
	uint32_t sequence;
	uint64_t segment_start_ns;
};

struct sr_audio_index_entry {
	uint64_t timestamp_ns;
	uint64_t file_offset;
	uint32_t packet_size;
	uint32_t samples;
};
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(struct sr_audio_file_header) == 52, "unexpected sr_audio_file_header layout");
static_assert(sizeof(struct sr_audio_packet_header) == 40, "unexpected sr_audio_packet_header layout");
static_assert(sizeof(struct sr_audio_index_header) == 24, "unexpected sr_audio_index_header layout");
static_assert(sizeof(struct sr_audio_index_entry) == 24, "unexpected sr_audio_index_entry layout");
#else
_Static_assert(sizeof(struct sr_audio_file_header) == 52, "unexpected sr_audio_file_header layout");
_Static_assert(sizeof(struct sr_audio_packet_header) == 40, "unexpected sr_audio_packet_header layout");
_Static_assert(sizeof(struct sr_audio_index_header) == 24, "unexpected sr_audio_index_header layout");
_Static_assert(sizeof(struct sr_audio_index_entry) == 24, "unexpected sr_audio_index_entry layout");
#endif
