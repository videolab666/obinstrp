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

#define SR_SEGMENT_FORMAT_VERSION 1u
#define SR_SEGMENT_MAGIC "SRSEG01"
#define SR_INDEX_MAGIC "SRIDX01"
#define SR_PACKET_RECORD_MAGIC 0x5352504Bu /* 'SRPK' */

#define SR_SEGMENT_FLAG_DISCONTINUITY 0x00000001u

#define SR_PACKET_FLAG_KEYFRAME 0x01u
#define SR_PACKET_FLAG_DISCONTINUITY 0x02u

enum sr_segment_packet_type {
	SR_SEGMENT_PACKET_VIDEO = 1,
	SR_SEGMENT_PACKET_AUDIO = 2,
};

/*
 * Internal replay storage is deliberately little and simple: fixed-size,
 * versioned records followed by encoded packet payloads. Version 1 is stored
 * in the host's little-endian representation; the first supported platform
 * is Windows x64. Future cross-endian support must introduce a new format
 * version instead of silently changing these layouts.
 */
#pragma pack(push, 1)
struct sr_segment_file_header {
	char magic[8];
	uint32_t version;
	uint32_t camera_hash;
	uint32_t sequence;
	uint32_t codec_id;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t segment_start_ns;
	uint32_t extradata_size;
	uint32_t flags;
};

struct sr_segment_packet_header {
	uint32_t magic;
	uint32_t payload_size;
	uint8_t type;
	uint8_t flags;
	uint16_t reserved;
	uint64_t timestamp_ns;
	int64_t pts;
	int64_t dts;
	int64_t duration;
};

struct sr_index_file_header {
	char magic[8];
	uint32_t version;
	uint32_t camera_hash;
	uint32_t sequence;
	uint32_t reserved;
	uint64_t segment_start_ns;
};

struct sr_index_entry {
	uint64_t timestamp_ns;
	uint64_t file_offset;
	uint32_t packet_size;
	uint32_t frame_number;
	uint8_t keyframe;
	uint8_t reserved[7];
};
#pragma pack(pop)

_Static_assert(sizeof(struct sr_segment_file_header) == 60, "unexpected sr_segment_file_header layout");
_Static_assert(sizeof(struct sr_segment_packet_header) == 44, "unexpected sr_segment_packet_header layout");
_Static_assert(sizeof(struct sr_index_file_header) == 32, "unexpected sr_index_file_header layout");
_Static_assert(sizeof(struct sr_index_entry) == 32, "unexpected sr_index_entry layout");
