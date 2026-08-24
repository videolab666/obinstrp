/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-master-audio-reader.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stdio.h>
#include <string.h>

#define SR_AUDIO_MAX_EXTRADATA_SIZE (1024u * 1024u)
#define SR_AUDIO_MAX_PACKET_SIZE (16u * 1024u * 1024u)
#define SR_AUDIO_MAX_CHANNELS 8u
#define SR_AUDIO_MAX_SAMPLE_RATE 384000u

struct sr_master_audio_reader {
	FILE *audio_file;
	FILE *index_file;
	struct sr_audio_file_header audio_header;
	struct sr_audio_index_header index_header;
	uint8_t *extradata;
	struct sr_audio_index_entry *entries;
	size_t entry_count;
};

static bool read_exact(FILE *file, void *data, size_t bytes)
{
	return bytes == 0 || (data && fread(data, 1, bytes, file) == bytes);
}

static bool valid_magic(const char actual[8], const char expected[8])
{
	return memcmp(actual, expected, 8) == 0;
}

static bool read_headers(struct sr_master_audio_reader *reader)
{
	if (!read_exact(reader->audio_file, &reader->audio_header, sizeof(reader->audio_header)))
		return false;
	if (!valid_magic(reader->audio_header.magic, SR_AUDIO_MAGIC) ||
	    reader->audio_header.version != SR_AUDIO_FORMAT_VERSION)
		return false;
	if (reader->audio_header.extradata_size > SR_AUDIO_MAX_EXTRADATA_SIZE || !reader->audio_header.sample_rate ||
	    reader->audio_header.sample_rate > SR_AUDIO_MAX_SAMPLE_RATE || !reader->audio_header.channels ||
	    reader->audio_header.channels > SR_AUDIO_MAX_CHANNELS)
		return false;

	if (reader->audio_header.extradata_size) {
		reader->extradata = bmalloc(reader->audio_header.extradata_size);
		if (!reader->extradata ||
		    !read_exact(reader->audio_file, reader->extradata, reader->audio_header.extradata_size))
			return false;
	}

	if (!read_exact(reader->index_file, &reader->index_header, sizeof(reader->index_header)))
		return false;
	if (!valid_magic(reader->index_header.magic, SR_AUDIO_INDEX_MAGIC) ||
	    reader->index_header.version != SR_AUDIO_FORMAT_VERSION)
		return false;
	if (reader->index_header.sequence != reader->audio_header.sequence ||
	    reader->index_header.segment_start_ns != reader->audio_header.segment_start_ns)
		return false;
	return true;
}

bool sr_master_audio_reader_refresh_index(struct sr_master_audio_reader *reader)
{
	if (!reader || !reader->index_file)
		return false;

	if (os_fseeki64(reader->index_file, 0, SEEK_END) != 0)
		return false;
	const int64_t size = os_ftelli64(reader->index_file);
	if (size < (int64_t)sizeof(struct sr_audio_index_header))
		return false;

	const uint64_t payload = (uint64_t)size - sizeof(struct sr_audio_index_header);
	const size_t count = (size_t)(payload / sizeof(struct sr_audio_index_entry));
	struct sr_audio_index_entry *fresh = NULL;
	if (count) {
		fresh = bmalloc(count * sizeof(*fresh));
		if (!fresh)
			return false;
	}

	if (os_fseeki64(reader->index_file, (int64_t)sizeof(struct sr_audio_index_header), SEEK_SET) != 0) {
		bfree(fresh);
		return false;
	}
	if (count && fread(fresh, sizeof(*fresh), count, reader->index_file) != count) {
		bfree(fresh);
		return false;
	}

	bfree(reader->entries);
	reader->entries = fresh;
	reader->entry_count = count;
	return true;
}

struct sr_master_audio_reader *sr_master_audio_reader_open(const char *audio_path, const char *index_path)
{
	if (!audio_path || !*audio_path || !index_path || !*index_path)
		return NULL;

	struct sr_master_audio_reader *reader = bzalloc(sizeof(*reader));
	reader->audio_file = os_fopen(audio_path, "rb");
	reader->index_file = os_fopen(index_path, "rb");
	if (!reader->audio_file || !reader->index_file) {
		blog(LOG_WARNING, "Pitel Instant Replay: could not open master audio segment/index '%s' / '%s'", audio_path,
		     index_path);
		sr_master_audio_reader_close(reader);
		return NULL;
	}

	if (!read_headers(reader) || !sr_master_audio_reader_refresh_index(reader)) {
		blog(LOG_WARNING, "Pitel Instant Replay: invalid or unsupported master audio segment '%s'", audio_path);
		sr_master_audio_reader_close(reader);
		return NULL;
	}
	return reader;
}

void sr_master_audio_reader_close(struct sr_master_audio_reader *reader)
{
	if (!reader)
		return;
	if (reader->audio_file)
		fclose(reader->audio_file);
	if (reader->index_file)
		fclose(reader->index_file);
	bfree(reader->extradata);
	bfree(reader->entries);
	bfree(reader);
}

bool sr_master_audio_reader_get_info(const struct sr_master_audio_reader *reader,
				     struct sr_master_audio_segment_info *info)
{
	if (!reader || !info)
		return false;
	memset(info, 0, sizeof(*info));
	info->codec_id = (enum AVCodecID)reader->audio_header.codec_id;
	info->sequence = reader->audio_header.sequence;
	info->sample_rate = reader->audio_header.sample_rate;
	info->channels = reader->audio_header.channels;
	info->bit_rate = reader->audio_header.bit_rate;
	info->segment_flags = reader->audio_header.flags;
	info->segment_start_ns = reader->audio_header.segment_start_ns;
	info->extradata = reader->extradata;
	info->extradata_size = (int)reader->audio_header.extradata_size;
	info->indexed_packets = reader->entry_count;
	return true;
}

size_t sr_master_audio_reader_entry_count(const struct sr_master_audio_reader *reader)
{
	return reader ? reader->entry_count : 0;
}

bool sr_master_audio_reader_entry_at(const struct sr_master_audio_reader *reader, size_t position,
				     struct sr_audio_index_entry *entry)
{
	if (!reader || !entry || position >= reader->entry_count)
		return false;
	*entry = reader->entries[position];
	return true;
}

bool sr_master_audio_reader_find_position(const struct sr_master_audio_reader *reader, uint64_t timestamp_ns,
					  size_t *position, struct sr_audio_index_entry *entry)
{
	if (!reader || !reader->entry_count || timestamp_ns < reader->entries[0].timestamp_ns)
		return false;

	size_t lo = 0;
	size_t hi = reader->entry_count;
	while (lo + 1 < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (reader->entries[mid].timestamp_ns <= timestamp_ns)
			lo = mid;
		else
			hi = mid;
	}

	if (position)
		*position = lo;
	if (entry)
		*entry = reader->entries[lo];
	return true;
}

bool sr_master_audio_reader_read_packet(struct sr_master_audio_reader *reader, const struct sr_audio_index_entry *entry,
					AVPacket **packet, uint64_t *timestamp_ns)
{
	if (!reader || !entry || !packet)
		return false;
	*packet = NULL;
	if (entry->packet_size > SR_AUDIO_MAX_PACKET_SIZE ||
	    os_fseeki64(reader->audio_file, (int64_t)entry->file_offset, SEEK_SET) != 0)
		return false;

	struct sr_audio_packet_header header;
	if (!read_exact(reader->audio_file, &header, sizeof(header)))
		return false;
	if (header.magic != SR_AUDIO_PACKET_MAGIC || header.payload_size > SR_AUDIO_MAX_PACKET_SIZE ||
	    header.payload_size != entry->packet_size || header.timestamp_ns != entry->timestamp_ns)
		return false;

	AVPacket *result = av_packet_alloc();
	if (!result)
		return false;
	if (av_new_packet(result, (int)header.payload_size) < 0) {
		av_packet_free(&result);
		return false;
	}
	if (!read_exact(reader->audio_file, result->data, header.payload_size)) {
		av_packet_free(&result);
		return false;
	}
	result->pts = header.pts;
	result->dts = header.dts;
	result->duration = header.duration;
	if (timestamp_ns)
		*timestamp_ns = header.timestamp_ns;
	*packet = result;
	return true;
}
