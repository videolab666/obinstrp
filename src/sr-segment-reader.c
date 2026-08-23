/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-segment-reader.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stdio.h>
#include <string.h>

#define SR_MAX_EXTRADATA_SIZE (1024u * 1024u)
#define SR_MAX_PACKET_SIZE (256u * 1024u * 1024u)

struct sr_segment_reader {
	FILE *segment_file;
	FILE *index_file;

	struct sr_segment_file_header segment_header;
	struct sr_index_file_header index_header;

	uint8_t *extradata;
	struct sr_index_entry *entries;
	size_t entry_count;
};

static bool read_exact(FILE *f, void *data, size_t bytes)
{
	return bytes == 0 || (data && fread(data, 1, bytes, f) == bytes);
}

static bool valid_magic(const char actual[8], const char expected[8])
{
	return memcmp(actual, expected, 8) == 0;
}

static bool read_headers(struct sr_segment_reader *r)
{
	if (!read_exact(r->segment_file, &r->segment_header, sizeof(r->segment_header)))
		return false;
	if (!valid_magic(r->segment_header.magic, SR_SEGMENT_MAGIC) ||
	    r->segment_header.version != SR_SEGMENT_FORMAT_VERSION)
		return false;
	if (r->segment_header.extradata_size > SR_MAX_EXTRADATA_SIZE)
		return false;

	if (r->segment_header.extradata_size) {
		r->extradata = bmalloc(r->segment_header.extradata_size);
		if (!r->extradata || !read_exact(r->segment_file, r->extradata, r->segment_header.extradata_size))
			return false;
	}

	if (!read_exact(r->index_file, &r->index_header, sizeof(r->index_header)))
		return false;
	if (!valid_magic(r->index_header.magic, SR_INDEX_MAGIC) || r->index_header.version != SR_SEGMENT_FORMAT_VERSION)
		return false;
	if (r->index_header.camera_hash != r->segment_header.camera_hash ||
	    r->index_header.sequence != r->segment_header.sequence ||
	    r->index_header.segment_start_ns != r->segment_header.segment_start_ns)
		return false;

	return true;
}

bool sr_segment_reader_refresh_index(struct sr_segment_reader *r)
{
	if (!r || !r->index_file)
		return false;

	if (os_fseeki64(r->index_file, 0, SEEK_END) != 0)
		return false;
	const int64_t size = os_ftelli64(r->index_file);
	if (size < (int64_t)sizeof(struct sr_index_file_header))
		return false;

	const uint64_t payload = (uint64_t)size - sizeof(struct sr_index_file_header);
	const size_t count = (size_t)(payload / sizeof(struct sr_index_entry));

	struct sr_index_entry *fresh = NULL;
	if (count) {
		fresh = bmalloc(count * sizeof(*fresh));
		if (!fresh)
			return false;
	}

	if (os_fseeki64(r->index_file, (int64_t)sizeof(struct sr_index_file_header), SEEK_SET) != 0) {
		bfree(fresh);
		return false;
	}
	if (count && fread(fresh, sizeof(*fresh), count, r->index_file) != count) {
		bfree(fresh);
		return false;
	}

	/* Ignore any trailing partial index record from an interrupted write.
	 * Full records already read above remain a valid snapshot. */
	bfree(r->entries);
	r->entries = fresh;
	r->entry_count = count;
	return true;
}

struct sr_segment_reader *sr_segment_reader_open(const char *segment_path, const char *index_path)
{
	if (!segment_path || !*segment_path || !index_path || !*index_path)
		return NULL;

	struct sr_segment_reader *r = bzalloc(sizeof(*r));
	r->segment_file = os_fopen(segment_path, "rb");
	r->index_file = os_fopen(index_path, "rb");
	if (!r->segment_file || !r->index_file) {
		obs_log(LOG_WARNING, "Sports Replay: could not open replay segment/index '%s' / '%s'", segment_path,
			index_path);
		sr_segment_reader_close(r);
		return NULL;
	}

	if (!read_headers(r) || !sr_segment_reader_refresh_index(r)) {
		obs_log(LOG_WARNING, "Sports Replay: invalid or unsupported replay segment '%s'", segment_path);
		sr_segment_reader_close(r);
		return NULL;
	}

	return r;
}

void sr_segment_reader_close(struct sr_segment_reader *r)
{
	if (!r)
		return;
	if (r->segment_file)
		fclose(r->segment_file);
	if (r->index_file)
		fclose(r->index_file);
	bfree(r->extradata);
	bfree(r->entries);
	bfree(r);
}

bool sr_segment_reader_get_info(const struct sr_segment_reader *r, struct sr_segment_stream_info *info)
{
	if (!r || !info)
		return false;

	memset(info, 0, sizeof(*info));
	info->codec_id = (enum AVCodecID)r->segment_header.codec_id;
	info->camera_hash = r->segment_header.camera_hash;
	info->sequence = r->segment_header.sequence;
	info->width = r->segment_header.width;
	info->height = r->segment_header.height;
	info->fps_num = r->segment_header.fps_num;
	info->fps_den = r->segment_header.fps_den;
	info->segment_start_ns = r->segment_header.segment_start_ns;
	info->segment_flags = r->segment_header.flags;
	info->extradata = r->extradata;
	info->extradata_size = (int)r->segment_header.extradata_size;
	info->indexed_packets = r->entry_count;
	return true;
}

size_t sr_segment_reader_entry_count(const struct sr_segment_reader *r)
{
	return r ? r->entry_count : 0;
}

bool sr_segment_reader_entry_at(const struct sr_segment_reader *r, size_t position, struct sr_index_entry *entry)
{
	if (!r || !entry || position >= r->entry_count)
		return false;
	*entry = r->entries[position];
	return true;
}

bool sr_segment_reader_find_position(const struct sr_segment_reader *r, uint64_t timestamp_ns, bool keyframe_only,
				     size_t *position, struct sr_index_entry *entry)
{
	if (!r || !r->entry_count)
		return false;
	if (timestamp_ns < r->entries[0].timestamp_ns)
		return false;

	size_t lo = 0;
	size_t hi = r->entry_count;
	while (lo + 1 < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (r->entries[mid].timestamp_ns <= timestamp_ns)
			lo = mid;
		else
			hi = mid;
	}

	if (keyframe_only) {
		for (;;) {
			if (r->entries[lo].keyframe)
				break;
			if (lo == 0)
				return false;
			lo--;
		}
	}

	if (position)
		*position = lo;
	if (entry)
		*entry = r->entries[lo];
	return true;
}

bool sr_segment_reader_find(const struct sr_segment_reader *r, uint64_t timestamp_ns, bool keyframe_only,
			    struct sr_index_entry *entry)
{
	return entry && sr_segment_reader_find_position(r, timestamp_ns, keyframe_only, NULL, entry);
}

bool sr_segment_reader_read_video_packet(struct sr_segment_reader *r, const struct sr_index_entry *entry,
					 AVPacket **packet, uint64_t *timestamp_ns, uint8_t *record_flags)
{
	if (!r || !entry || !packet)
		return false;
	*packet = NULL;

	if (os_fseeki64(r->segment_file, (int64_t)entry->file_offset, SEEK_SET) != 0)
		return false;

	struct sr_segment_packet_header ph;
	if (!read_exact(r->segment_file, &ph, sizeof(ph)))
		return false;
	if (ph.magic != SR_PACKET_RECORD_MAGIC || ph.type != SR_SEGMENT_PACKET_VIDEO ||
	    ph.payload_size > SR_MAX_PACKET_SIZE || ph.payload_size != entry->packet_size)
		return false;
	if (ph.timestamp_ns != entry->timestamp_ns)
		return false;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;
	if (av_new_packet(pkt, (int)ph.payload_size) < 0) {
		av_packet_free(&pkt);
		return false;
	}
	if (!read_exact(r->segment_file, pkt->data, ph.payload_size)) {
		av_packet_free(&pkt);
		return false;
	}

	pkt->pts = ph.pts;
	pkt->dts = ph.dts;
	pkt->duration = ph.duration;
	if (ph.flags & SR_PACKET_FLAG_KEYFRAME)
		pkt->flags |= AV_PKT_FLAG_KEY;

	if (timestamp_ns)
		*timestamp_ns = ph.timestamp_ns;
	if (record_flags)
		*record_flags = ph.flags;
	*packet = pkt;
	return true;
}
