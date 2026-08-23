/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-master-audio-player.h"

#include "sr-master-audio-catalog.h"
#include "sr-master-audio-reader.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/deque.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <string.h>

struct sr_master_audio_player {
	char *session_dir;
	struct sr_master_audio_descriptor *segments;
	size_t segment_count;

	struct sr_master_audio_reader *reader;
	uint32_t current_sequence;
	uint64_t current_start_ns;
	bool current_active;
	size_t next_entry;

	AVCodecContext *decoder;
	AVFrame *decode_frame;
	struct deque submitted_timestamps;
};

static void clear_submitted_timestamps(struct sr_master_audio_player *player)
{
	uint64_t ignored;
	while (player->submitted_timestamps.size)
		deque_pop_front(&player->submitted_timestamps, &ignored, sizeof(ignored));
}

static void close_reader(struct sr_master_audio_player *player)
{
	sr_master_audio_reader_close(player->reader);
	player->reader = NULL;
	player->current_sequence = 0;
	player->current_start_ns = 0;
	player->current_active = false;
	player->next_entry = 0;
}

static void close_decoder(struct sr_master_audio_player *player)
{
	av_frame_free(&player->decode_frame);
	avcodec_free_context(&player->decoder);
	clear_submitted_timestamps(player);
}

static bool decoder_matches(const struct sr_master_audio_player *player,
			    const struct sr_master_audio_segment_info *info)
{
	if (!player->decoder)
		return false;
	if (player->decoder->codec_id != info->codec_id || player->decoder->sample_rate != (int)info->sample_rate ||
	    player->decoder->ch_layout.nb_channels != (int)info->channels ||
	    player->decoder->extradata_size != info->extradata_size)
		return false;
	return info->extradata_size <= 0 ||
	       memcmp(player->decoder->extradata, info->extradata, (size_t)info->extradata_size) == 0;
}

static bool open_decoder(struct sr_master_audio_player *player, const struct sr_master_audio_segment_info *info,
			 bool force_reset)
{
	if (!force_reset && decoder_matches(player, info))
		return true;

	close_decoder(player);
	const AVCodec *codec = avcodec_find_decoder(info->codec_id);
	if (!codec)
		return false;

	AVCodecContext *decoder = avcodec_alloc_context3(codec);
	if (!decoder)
		return false;
	decoder->sample_rate = (int)info->sample_rate;
	av_channel_layout_default(&decoder->ch_layout, (int)info->channels);
	if (info->extradata && info->extradata_size > 0) {
		decoder->extradata = av_mallocz((size_t)info->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!decoder->extradata) {
			avcodec_free_context(&decoder);
			return false;
		}
		memcpy(decoder->extradata, info->extradata, (size_t)info->extradata_size);
		decoder->extradata_size = info->extradata_size;
	}

	if (avcodec_open2(decoder, codec, NULL) < 0) {
		avcodec_free_context(&decoder);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		avcodec_free_context(&decoder);
		return false;
	}
	player->decoder = decoder;
	player->decode_frame = frame;
	return true;
}

bool sr_master_audio_player_refresh(struct sr_master_audio_player *player)
{
	if (!player)
		return false;

	struct sr_master_audio_descriptor *segments = NULL;
	size_t count = 0;
	if (!sr_master_audio_catalog_scan(player->session_dir, &segments, &count))
		return false;

	sr_master_audio_catalog_free(player->segments, player->segment_count);
	player->segments = segments;
	player->segment_count = count;
	return true;
}

static bool open_catalog_segment(struct sr_master_audio_player *player, size_t index, bool force_decoder_reset)
{
	if (!player || index >= player->segment_count)
		return false;

	const struct sr_master_audio_descriptor *descriptor = &player->segments[index];
	struct sr_master_audio_reader *reader =
		sr_master_audio_reader_open(descriptor->audio_path, descriptor->index_path);
	if (!reader)
		return false;

	struct sr_master_audio_segment_info info;
	if (!sr_master_audio_reader_get_info(reader, &info)) {
		sr_master_audio_reader_close(reader);
		return false;
	}

	const bool reset_decoder = force_decoder_reset || (info.segment_flags & SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY) != 0;
	if (!open_decoder(player, &info, reset_decoder)) {
		sr_master_audio_reader_close(reader);
		return false;
	}

	sr_master_audio_reader_close(player->reader);
	player->reader = reader;
	player->current_sequence = descriptor->sequence;
	player->current_start_ns = descriptor->start_ns;
	player->current_active = descriptor->active;
	player->next_entry = 0;
	return true;
}

static bool choose_seek_segment(struct sr_master_audio_player *player, uint64_t timestamp_ns, size_t *index)
{
	const struct sr_master_audio_descriptor *descriptor =
		sr_master_audio_catalog_find(player->segments, player->segment_count, timestamp_ns);
	if (descriptor) {
		*index = (size_t)(descriptor - player->segments);
		return true;
	}

	/* Audio can begin a few milliseconds after the first camera packet. If the
	 * requested Event IN lands in that gap, start from the next available audio
	 * segment rather than treating the whole replay as silent. */
	for (size_t i = 0; i < player->segment_count; i++) {
		if (player->segments[i].start_ns >= timestamp_ns) {
			*index = i;
			return true;
		}
	}
	return false;
}

bool sr_master_audio_player_seek(struct sr_master_audio_player *player, uint64_t timestamp_ns)
{
	if (!player || !sr_master_audio_player_refresh(player) || !player->segment_count)
		return false;

	size_t index = 0;
	if (!choose_seek_segment(player, timestamp_ns, &index) || !open_catalog_segment(player, index, true))
		return false;

	const size_t count = sr_master_audio_reader_entry_count(player->reader);
	if (!count)
		return false;

	size_t position = 0;
	struct sr_audio_index_entry entry;
	if (sr_master_audio_reader_find_position(player->reader, timestamp_ns, &position, &entry)) {
		/* Prime one AAC frame before the requested timestamp when possible. It
		 * supplies the MDCT overlap state and the Event Output trims the decoded
		 * samples before IN. */
		player->next_entry = position > 0 ? position - 1 : position;
	} else {
		player->next_entry = 0;
	}
	return true;
}

static bool advance_segment(struct sr_master_audio_player *player)
{
	const uint64_t previous_start = player->current_start_ns;
	if (!sr_master_audio_player_refresh(player))
		return false;

	for (size_t i = 0; i < player->segment_count; i++) {
		if (player->segments[i].start_ns > previous_start)
			return open_catalog_segment(player, i, false);
	}
	return false;
}

static bool ensure_next_entry(struct sr_master_audio_player *player)
{
	if (!player->reader)
		return false;

	if (player->next_entry < sr_master_audio_reader_entry_count(player->reader))
		return true;

	if (player->current_active) {
		sr_master_audio_reader_refresh_index(player->reader);
		if (player->next_entry < sr_master_audio_reader_entry_count(player->reader))
			return true;
	}

	return advance_segment(player) && player->next_entry < sr_master_audio_reader_entry_count(player->reader);
}

static bool receive_frame(struct sr_master_audio_player *player, AVFrame **frame, uint64_t *timestamp_ns)
{
	const int ret = avcodec_receive_frame(player->decoder, player->decode_frame);
	if (ret < 0)
		return false;

	uint64_t timestamp = 0;
	if (player->submitted_timestamps.size >= sizeof(timestamp))
		deque_pop_front(&player->submitted_timestamps, &timestamp, sizeof(timestamp));

	AVFrame *copy = av_frame_clone(player->decode_frame);
	av_frame_unref(player->decode_frame);
	if (!copy)
		return false;
	*frame = copy;
	if (timestamp_ns)
		*timestamp_ns = timestamp;
	return true;
}

bool sr_master_audio_player_decode_next(struct sr_master_audio_player *player, AVFrame **frame, uint64_t *timestamp_ns)
{
	if (!player || !frame || !player->decoder)
		return false;
	*frame = NULL;
	if (timestamp_ns)
		*timestamp_ns = 0;

	for (unsigned attempts = 0; attempts < 16; attempts++) {
		int ret = avcodec_receive_frame(player->decoder, player->decode_frame);
		if (ret == 0) {
			uint64_t timestamp = 0;
			if (player->submitted_timestamps.size >= sizeof(timestamp))
				deque_pop_front(&player->submitted_timestamps, &timestamp, sizeof(timestamp));
			AVFrame *copy = av_frame_clone(player->decode_frame);
			av_frame_unref(player->decode_frame);
			if (!copy)
				return false;
			*frame = copy;
			if (timestamp_ns)
				*timestamp_ns = timestamp;
			return true;
		}
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
			return false;

		if (!ensure_next_entry(player))
			return false;

		struct sr_audio_index_entry entry;
		if (!sr_master_audio_reader_entry_at(player->reader, player->next_entry, &entry))
			return false;

		AVPacket *packet = NULL;
		uint64_t packet_timestamp = 0;
		if (!sr_master_audio_reader_read_packet(player->reader, &entry, &packet, &packet_timestamp))
			return false;

		ret = avcodec_send_packet(player->decoder, packet);
		av_packet_free(&packet);
		if (ret == AVERROR(EAGAIN))
			continue;
		if (ret < 0)
			return false;

		player->next_entry++;
		deque_push_back(&player->submitted_timestamps, &packet_timestamp, sizeof(packet_timestamp));
	}
	return false;
}

bool sr_master_audio_player_get_bounds(struct sr_master_audio_player *player, uint64_t *first_ns, uint64_t *last_ns)
{
	if (!player || !sr_master_audio_player_refresh(player) || !player->segment_count)
		return false;
	if (first_ns)
		*first_ns = player->segments[0].start_ns;
	if (last_ns)
		*last_ns = player->segments[player->segment_count - 1].end_ns;
	return true;
}

struct sr_master_audio_player *sr_master_audio_player_create(const char *session_dir)
{
	if (!session_dir || !*session_dir)
		return NULL;
	struct sr_master_audio_player *player = bzalloc(sizeof(*player));
	player->session_dir = bstrdup(session_dir);
	if (!player->session_dir) {
		bfree(player);
		return NULL;
	}
	return player;
}

void sr_master_audio_player_destroy(struct sr_master_audio_player *player)
{
	if (!player)
		return;
	close_reader(player);
	close_decoder(player);
	deque_free(&player->submitted_timestamps);
	sr_master_audio_catalog_free(player->segments, player->segment_count);
	bfree(player->session_dir);
	bfree(player);
}
