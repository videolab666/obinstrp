/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-master-audio.h"

#include "sr-audio-format.h"
#include "sr-camera-identity.h"
#include "sr-config.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/deque.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MASTER_AUDIO_SAMPLE_RATE 48000u
#define MASTER_AUDIO_CHANNELS 2u
#define MASTER_AUDIO_BIT_RATE 192000u
#define MASTER_AUDIO_MAX_QUEUE_CHUNKS 256u
#define MASTER_AUDIO_PUBLISH_NS 250000000ULL

struct sr_master_audio_chunk {
	struct sr_master_audio_chunk *next;
	float *samples;
	uint32_t frames;
	uint64_t timestamp_ns;
	uint64_t epoch;
};

struct sr_master_audio_state {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t thread;
	bool thread_started;
	bool callback_registered;
	bool stopping;
	bool active;
	unsigned active_refs;

	struct sr_master_audio_chunk *head;
	struct sr_master_audio_chunk *tail;
	size_t queue_depth;
	size_t max_queue_chunks;
	uint64_t enqueue_epoch;

	char *session_dir;
	char *audio_dir;
	uint32_t sample_rate;
	uint64_t target_segment_ns;
	uint64_t min_free_bytes;
	bool reserve_blocked;
	uint64_t reserve_recheck_after_ns;
	uint32_t next_sequence;

	AVCodecContext *encoder;
	AVFrame *frame;
	uint32_t pending_frames;
	uint64_t pending_start_ns;
	int64_t next_pts;
	struct deque submitted_timestamps;
	uint64_t write_epoch;
	bool have_write_epoch;
	bool next_segment_discontinuity;
	bool encoder_failed_session;

	FILE *audio_file;
	FILE *index_file;
	char *audio_part_path;
	char *audio_final_path;
	char *index_part_path;
	char *index_final_path;
	uint64_t segment_start_ns;
	uint64_t last_publish_ns;

	struct sr_master_audio_stats stats;
};

struct sr_camera_audio_writer {
	struct sr_master_audio_state *state;
};

static struct sr_master_audio_state *g_audio;

static char *join_path(const char *dir, const char *tail)
{
	struct dstr path = {0};
	dstr_copy(&path, dir ? dir : "");
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, tail ? tail : "");
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

static char *make_segment_path(const char *dir, uint32_t sequence, const char *suffix)
{
	char name[64];
	snprintf(name, sizeof(name), "%08u%s", sequence, suffix);
	return join_path(dir, name);
}

static uint32_t find_next_sequence(const char *audio_dir)
{
	char *pattern = join_path(audio_dir, "*.sraud*");
	if (!pattern)
		return 1;

	uint32_t max_sequence = 0;
	os_glob_t *glob = NULL;
	if (os_glob(pattern, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			const char *path = glob->gl_pathv[i].path;
			const char *base = strrchr(path, '/');
#ifdef _WIN32
			const char *base2 = strrchr(path, '\\');
			if (!base || (base2 && base2 > base))
				base = base2;
#endif
			base = base ? base + 1 : path;
			char *end = NULL;
			const unsigned long value = strtoul(base, &end, 10);
			if (end != base && value > max_sequence && value < UINT32_MAX)
				max_sequence = (uint32_t)value;
		}
		os_globfree(glob);
	}
	bfree(pattern);
	return max_sequence + 1u;
}

static bool write_exact(FILE *file, const void *data, size_t bytes)
{
	return bytes == 0 || (data && fwrite(data, 1, bytes, file) == bytes);
}

static void stats_set_encoder_failed(struct sr_master_audio_state *state)
{
	pthread_mutex_lock(&state->mutex);
	state->stats.encoder_failed = true;
	pthread_mutex_unlock(&state->mutex);
}

static void stats_set_write_failed(struct sr_master_audio_state *state)
{
	pthread_mutex_lock(&state->mutex);
	state->stats.write_failed = true;
	pthread_mutex_unlock(&state->mutex);
}

static void stats_add_packet(struct sr_master_audio_state *state, uint64_t bytes)
{
	pthread_mutex_lock(&state->mutex);
	state->stats.packets_written++;
	state->stats.bytes_written += bytes;
	pthread_mutex_unlock(&state->mutex);
}

static void stats_add_packet_drop(struct sr_master_audio_state *state)
{
	pthread_mutex_lock(&state->mutex);
	state->stats.packets_dropped++;
	pthread_mutex_unlock(&state->mutex);
}

static void stats_set_reserve_blocked(struct sr_master_audio_state *state, bool blocked)
{
	pthread_mutex_lock(&state->mutex);
	state->stats.reserve_blocked = blocked;
	pthread_mutex_unlock(&state->mutex);
}

static bool storage_reserve_allows(struct sr_master_audio_state *state, uint64_t timestamp_ns)
{
	if (!state->min_free_bytes) {
		if (state->reserve_blocked) {
			state->reserve_blocked = false;
			state->reserve_recheck_after_ns = 0;
			stats_set_reserve_blocked(state, false);
		}
		return true;
	}

	if (state->reserve_blocked && timestamp_ns < state->reserve_recheck_after_ns)
		return false;

	const uint64_t free_bytes = state->audio_dir ? os_get_free_disk_space(state->audio_dir) : 0;
	if (free_bytes < state->min_free_bytes) {
		if (!state->reserve_blocked) {
			blog(LOG_ERROR,
			     "Pitel Instant Replay: master replay audio paused: disk free space %.1f GB is below the %.1f GB reserve",
			     (double)free_bytes / (1024.0 * 1024.0 * 1024.0),
			     (double)state->min_free_bytes / (1024.0 * 1024.0 * 1024.0));
		}
		state->reserve_blocked = true;
		state->reserve_recheck_after_ns = timestamp_ns + 1000000000ULL;
		state->next_segment_discontinuity = true;
		stats_set_reserve_blocked(state, true);
		return false;
	}

	if (state->reserve_blocked) {
		blog(LOG_INFO, "Pitel Instant Replay: disk reserve restored; master replay audio resumed");
		state->reserve_blocked = false;
		state->reserve_recheck_after_ns = 0;
		state->next_segment_discontinuity = true;
		stats_set_reserve_blocked(state, false);
	}
	return true;
}

static void clear_segment_paths(struct sr_master_audio_state *state)
{
	bfree(state->audio_part_path);
	bfree(state->audio_final_path);
	bfree(state->index_part_path);
	bfree(state->index_final_path);
	state->audio_part_path = NULL;
	state->audio_final_path = NULL;
	state->index_part_path = NULL;
	state->index_final_path = NULL;
}

static void close_files(struct sr_master_audio_state *state)
{
	if (state->audio_file) {
		fflush(state->audio_file);
		fclose(state->audio_file);
		state->audio_file = NULL;
	}
	if (state->index_file) {
		fflush(state->index_file);
		fclose(state->index_file);
		state->index_file = NULL;
	}
}

static void close_segment(struct sr_master_audio_state *state, bool finalize)
{
	if (!state->audio_file && !state->index_file)
		return;

	close_files(state);
	if (finalize) {
		const int audio_rc = os_rename(state->audio_part_path, state->audio_final_path);
		const int index_rc = os_rename(state->index_part_path, state->index_final_path);
		if (audio_rc == 0 && index_rc == 0) {
			pthread_mutex_lock(&state->mutex);
			state->stats.segments_finalized++;
			pthread_mutex_unlock(&state->mutex);
		} else {
			blog(LOG_ERROR, "Pitel Instant Replay: could not finalize master audio segment");
			stats_set_write_failed(state);
		}
	}

	clear_segment_paths(state);
	state->segment_start_ns = 0;
	state->last_publish_ns = 0;
}

static bool open_segment(struct sr_master_audio_state *state, uint64_t start_ns)
{
	if (!state->encoder || !state->audio_dir || state->next_sequence == UINT32_MAX)
		return false;

	const uint32_t sequence = state->next_sequence++;
	clear_segment_paths(state);
	state->audio_part_path = make_segment_path(state->audio_dir, sequence, ".sraud.part");
	state->audio_final_path = make_segment_path(state->audio_dir, sequence, ".sraud");
	state->index_part_path = make_segment_path(state->audio_dir, sequence, ".sraidx.part");
	state->index_final_path = make_segment_path(state->audio_dir, sequence, ".sraidx");

	state->audio_file = os_fopen(state->audio_part_path, "wb");
	state->index_file = os_fopen(state->index_part_path, "wb");
	if (!state->audio_file || !state->index_file) {
		blog(LOG_ERROR, "Pitel Instant Replay: could not open master audio segment files");
		close_files(state);
		stats_set_write_failed(state);
		return false;
	}

	struct sr_audio_file_header header = {0};
	memcpy(header.magic, SR_AUDIO_MAGIC, sizeof(header.magic));
	header.version = SR_AUDIO_FORMAT_VERSION;
	header.codec_id = (uint32_t)state->encoder->codec_id;
	header.sample_rate = (uint32_t)state->encoder->sample_rate;
	header.channels = (uint32_t)state->encoder->ch_layout.nb_channels;
	header.bit_rate = (uint32_t)state->encoder->bit_rate;
	header.flags = state->next_segment_discontinuity ? SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY : 0;
	header.sequence = sequence;
	header.segment_start_ns = start_ns;
	header.extradata_size = state->encoder->extradata_size > 0 ? (uint32_t)state->encoder->extradata_size : 0;

	struct sr_audio_index_header index = {0};
	memcpy(index.magic, SR_AUDIO_INDEX_MAGIC, sizeof(index.magic));
	index.version = SR_AUDIO_FORMAT_VERSION;
	index.sequence = sequence;
	index.segment_start_ns = start_ns;

	const bool ok = write_exact(state->audio_file, &header, sizeof(header)) &&
			write_exact(state->audio_file, state->encoder->extradata, header.extradata_size) &&
			write_exact(state->index_file, &index, sizeof(index)) && fflush(state->audio_file) == 0 &&
			fflush(state->index_file) == 0;
	if (!ok) {
		blog(LOG_ERROR, "Pitel Instant Replay: could not write master audio segment header");
		close_segment(state, false);
		stats_set_write_failed(state);
		return false;
	}

	state->segment_start_ns = start_ns;
	state->last_publish_ns = start_ns;
	state->next_segment_discontinuity = false;
	return true;
}

static bool write_packet(struct sr_master_audio_state *state, AVPacket *packet, uint64_t timestamp_ns)
{
	if (state->audio_file && state->segment_start_ns && timestamp_ns >= state->segment_start_ns &&
	    timestamp_ns - state->segment_start_ns >= state->target_segment_ns)
		close_segment(state, true);

	if (!state->audio_file) {
		if (!storage_reserve_allows(state, timestamp_ns)) {
			stats_add_packet_drop(state);
			return true;
		}
		if (!open_segment(state, timestamp_ns))
			return false;
	}

	const int64_t offset = os_ftelli64(state->audio_file);
	if (offset < 0)
		return false;

	struct sr_audio_packet_header header = {0};
	header.magic = SR_AUDIO_PACKET_MAGIC;
	header.payload_size = (uint32_t)packet->size;
	header.timestamp_ns = timestamp_ns;
	header.pts = packet->pts;
	header.dts = packet->dts;
	header.duration = packet->duration;

	struct sr_audio_index_entry index = {0};
	index.timestamp_ns = timestamp_ns;
	index.file_offset = (uint64_t)offset;
	index.packet_size = (uint32_t)packet->size;
	index.samples = packet->duration > 0 ? (uint32_t)packet->duration : (uint32_t)state->encoder->frame_size;

	if (!write_exact(state->audio_file, &header, sizeof(header)) ||
	    !write_exact(state->audio_file, packet->data, (size_t)packet->size) ||
	    !write_exact(state->index_file, &index, sizeof(index))) {
		stats_set_write_failed(state);
		return false;
	}

	stats_add_packet(state, sizeof(header) + (uint64_t)packet->size + sizeof(index));
	if (timestamp_ns >= state->last_publish_ns &&
	    timestamp_ns - state->last_publish_ns >= MASTER_AUDIO_PUBLISH_NS) {
		if (fflush(state->audio_file) != 0 || fflush(state->index_file) != 0) {
			stats_set_write_failed(state);
			return false;
		}
		state->last_publish_ns = timestamp_ns;
	}
	return true;
}

static void clear_submitted_timestamps(struct sr_master_audio_state *state)
{
	uint64_t ignored;
	while (state->submitted_timestamps.size)
		deque_pop_front(&state->submitted_timestamps, &ignored, sizeof(ignored));
}

static void destroy_encoder(struct sr_master_audio_state *state)
{
	av_frame_free(&state->frame);
	avcodec_free_context(&state->encoder);
	state->pending_frames = 0;
	state->pending_start_ns = 0;
	state->next_pts = 0;
	clear_submitted_timestamps(state);
}

static bool open_encoder(struct sr_master_audio_state *state)
{
	if (state->encoder)
		return true;
	if (state->encoder_failed_session)
		return false;

	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: FFmpeg AAC encoder is unavailable; master replay audio disabled");
		state->encoder_failed_session = true;
		stats_set_encoder_failed(state);
		return false;
	}

	AVCodecContext *encoder = avcodec_alloc_context3(codec);
	if (!encoder)
		return false;
	encoder->sample_rate = (int)(state->sample_rate ? state->sample_rate : MASTER_AUDIO_SAMPLE_RATE);
	encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
	encoder->bit_rate = MASTER_AUDIO_BIT_RATE;
	encoder->time_base = (AVRational){1, MASTER_AUDIO_SAMPLE_RATE};
	encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	av_channel_layout_default(&encoder->ch_layout, MASTER_AUDIO_CHANNELS);

	if (avcodec_open2(encoder, codec, NULL) < 0) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: could not open FFmpeg AAC encoder; master replay audio disabled");
		avcodec_free_context(&encoder);
		state->encoder_failed_session = true;
		stats_set_encoder_failed(state);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		avcodec_free_context(&encoder);
		return false;
	}
	frame->format = encoder->sample_fmt;
	frame->sample_rate = encoder->sample_rate;
	frame->nb_samples = encoder->frame_size > 0 ? encoder->frame_size : 1024;
	if (av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout) < 0 || av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		avcodec_free_context(&encoder);
		return false;
	}

	state->encoder = encoder;
	state->frame = frame;
	state->pending_frames = 0;
	state->pending_start_ns = 0;
	state->next_pts = 0;
	blog(LOG_INFO, "Pitel Instant Replay: master replay audio encoder opened (AAC-LC, 48 kHz stereo, 192 kbit/s)");
	return true;
}

static bool drain_encoder_packets(struct sr_master_audio_state *state)
{
	for (;;) {
		AVPacket *packet = av_packet_alloc();
		if (!packet)
			return false;

		const int rc = avcodec_receive_packet(state->encoder, packet);
		if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
			av_packet_free(&packet);
			return true;
		}
		if (rc < 0) {
			av_packet_free(&packet);
			return false;
		}

		uint64_t timestamp_ns = state->pending_start_ns;
		if (state->submitted_timestamps.size)
			deque_pop_front(&state->submitted_timestamps, &timestamp_ns, sizeof(timestamp_ns));
		const bool ok = write_packet(state, packet, timestamp_ns);
		av_packet_free(&packet);
		if (!ok)
			return false;
	}
}

static bool submit_frame(struct sr_master_audio_state *state)
{
	if (!state->encoder || !state->frame || !state->pending_frames)
		return true;

	const uint32_t frame_samples = (uint32_t)state->frame->nb_samples;
	if (state->pending_frames < frame_samples) {
		for (uint32_t channel = 0; channel < MASTER_AUDIO_CHANNELS; channel++) {
			float *plane = (float *)state->frame->data[channel];
			memset(plane + state->pending_frames, 0,
			       (frame_samples - state->pending_frames) * sizeof(float));
		}
	}

	state->frame->pts = state->next_pts;
	state->next_pts += frame_samples;
	const uint64_t timestamp_ns = state->pending_start_ns;
	deque_push_back(&state->submitted_timestamps, &timestamp_ns, sizeof(timestamp_ns));

	const int rc = avcodec_send_frame(state->encoder, state->frame);
	state->pending_frames = 0;
	state->pending_start_ns = 0;
	if (rc < 0)
		return false;
	return drain_encoder_packets(state);
}

static void finish_stream(struct sr_master_audio_state *state)
{
	if (state->encoder) {
		if (state->pending_frames && !submit_frame(state))
			stats_set_write_failed(state);
		if (avcodec_send_frame(state->encoder, NULL) >= 0 && !drain_encoder_packets(state))
			stats_set_write_failed(state);
	}
	close_segment(state, true);
	destroy_encoder(state);
	state->have_write_epoch = false;
	state->next_segment_discontinuity = false;
	state->reserve_blocked = false;
	state->reserve_recheck_after_ns = 0;
	stats_set_reserve_blocked(state, false);
	state->encoder_failed_session = false;
}

static bool encode_chunk(struct sr_master_audio_state *state, struct sr_master_audio_chunk *chunk)
{
	if (!state->have_write_epoch) {
		state->write_epoch = chunk->epoch;
		state->have_write_epoch = true;
	} else if (chunk->epoch != state->write_epoch) {
		finish_stream(state);
		state->write_epoch = chunk->epoch;
		state->have_write_epoch = true;
		state->next_segment_discontinuity = true;
	}

	if (!open_encoder(state))
		return false;

	uint32_t offset = 0;
	while (offset < chunk->frames) {
		if (av_frame_make_writable(state->frame) < 0)
			return false;

		const uint32_t capacity = (uint32_t)state->frame->nb_samples - state->pending_frames;
		const uint32_t copy_frames = chunk->frames - offset < capacity ? chunk->frames - offset : capacity;
		if (!state->pending_frames)
			state->pending_start_ns = chunk->timestamp_ns + audio_frames_to_ns(state->sample_rate, offset);

		for (uint32_t channel = 0; channel < MASTER_AUDIO_CHANNELS; channel++) {
			float *dst = (float *)state->frame->data[channel] + state->pending_frames;
			const float *src = chunk->samples + (size_t)channel * chunk->frames + offset;
			memcpy(dst, src, copy_frames * sizeof(float));
		}
		state->pending_frames += copy_frames;
		offset += copy_frames;

		if (state->pending_frames == (uint32_t)state->frame->nb_samples && !submit_frame(state))
			return false;
	}
	return true;
}

static void free_chunk(struct sr_master_audio_chunk *chunk)
{
	if (!chunk)
		return;
	bfree(chunk->samples);
	bfree(chunk);
}

static struct sr_master_audio_chunk *pop_chunk(struct sr_master_audio_state *state, bool *finalize_idle)
{
	*finalize_idle = false;
	pthread_mutex_lock(&state->mutex);
	while (!state->head && !state->stopping) {
		if (!state->active && state->encoder) {
			*finalize_idle = true;
			pthread_mutex_unlock(&state->mutex);
			return NULL;
		}
		pthread_cond_wait(&state->cond, &state->mutex);
	}

	if (!state->head) {
		pthread_mutex_unlock(&state->mutex);
		return NULL;
	}

	struct sr_master_audio_chunk *chunk = state->head;
	state->head = chunk->next;
	if (!state->head)
		state->tail = NULL;
	state->queue_depth--;
	state->stats.queue_depth = state->queue_depth;
	pthread_mutex_unlock(&state->mutex);
	return chunk;
}

static void *audio_worker(void *param)
{
	struct sr_master_audio_state *state = param;
	os_set_thread_name("pitel-replay-audio");

	for (;;) {
		bool finalize_idle = false;
		struct sr_master_audio_chunk *chunk = pop_chunk(state, &finalize_idle);
		if (finalize_idle) {
			finish_stream(state);
			continue;
		}
		if (!chunk) {
			pthread_mutex_lock(&state->mutex);
			const bool stopping = state->stopping;
			pthread_mutex_unlock(&state->mutex);
			if (stopping)
				break;
			continue;
		}

		if (!state->encoder_failed_session && !encode_chunk(state, chunk)) {
			blog(LOG_ERROR,
			     "Pitel Instant Replay: master replay audio encode/write failure; dropping audio until recorder restarts");
			state->encoder_failed_session = true;
			stats_set_write_failed(state);
		}
		free_chunk(chunk);
	}

	finish_stream(state);
	return NULL;
}

static bool enqueue_audio(struct sr_master_audio_state *state, const uint8_t *left, const uint8_t *right,
			  uint32_t frames, uint64_t timestamp_ns)
{
	if (!state || !frames || !left)
		return false;

	pthread_mutex_lock(&state->mutex);
	const bool active = state->active && !state->stopping;
	pthread_mutex_unlock(&state->mutex);
	if (!active)
		return false;

	struct sr_master_audio_chunk *chunk = bzalloc(sizeof(*chunk));
	chunk->frames = frames;
	chunk->timestamp_ns = timestamp_ns;
	chunk->samples = bmalloc((size_t)frames * MASTER_AUDIO_CHANNELS * sizeof(float));
	memcpy(chunk->samples, left, (size_t)frames * sizeof(float));
	memcpy(chunk->samples + frames, right ? right : left, (size_t)frames * sizeof(float));

	pthread_mutex_lock(&state->mutex);
	state->stats.chunks_received++;
	if (!state->active || state->stopping || state->queue_depth >= state->max_queue_chunks) {
		if (state->active && !state->stopping) {
			state->stats.chunks_dropped++;
			state->enqueue_epoch++;
		}
		pthread_mutex_unlock(&state->mutex);
		free_chunk(chunk);
		return false;
	}

	chunk->epoch = state->enqueue_epoch;
	if (state->tail)
		state->tail->next = chunk;
	else
		state->head = chunk;
	state->tail = chunk;
	state->queue_depth++;
	state->stats.queue_depth = state->queue_depth;
	if (state->queue_depth > state->stats.queue_high_watermark)
		state->stats.queue_high_watermark = state->queue_depth;
	pthread_cond_signal(&state->cond);
	pthread_mutex_unlock(&state->mutex);
	return true;
}

static void master_audio_callback(void *param, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(mix_idx);
	struct sr_master_audio_state *state = param;
	if (!state || !data || !data->frames || !data->data[0] || !data->data[1])
		return;

	enqueue_audio(state, data->data[0], data->data[1], data->frames, data->timestamp);
}

bool sr_master_audio_init(void)
{
	if (g_audio)
		return true;

	struct sr_master_audio_state *state = bzalloc(sizeof(*state));
	state->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;
	state->sample_rate = MASTER_AUDIO_SAMPLE_RATE;
	if (pthread_mutex_init(&state->mutex, NULL) != 0) {
		bfree(state);
		return false;
	}
	if (pthread_cond_init(&state->cond, NULL) != 0) {
		pthread_mutex_destroy(&state->mutex);
		bfree(state);
		return false;
	}
	if (pthread_create(&state->thread, NULL, audio_worker, state) != 0) {
		pthread_cond_destroy(&state->cond);
		pthread_mutex_destroy(&state->mutex);
		bfree(state);
		return false;
	}
	state->thread_started = true;
	g_audio = state;

	struct audio_convert_info conversion = {
		.samples_per_sec = MASTER_AUDIO_SAMPLE_RATE,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = SPEAKERS_STEREO,
		.allow_clipping = false,
	};
	obs_add_raw_audio_callback(0, &conversion, master_audio_callback, state);
	state->callback_registered = true;
	return true;
}

void sr_master_audio_free(void)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return;
	g_audio = NULL;

	if (state->callback_registered)
		obs_remove_raw_audio_callback(0, master_audio_callback, state);

	pthread_mutex_lock(&state->mutex);
	state->stopping = true;
	state->active = false;
	state->active_refs = 0;
	pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->mutex);
	if (state->thread_started)
		pthread_join(state->thread, NULL);

	while (state->head) {
		struct sr_master_audio_chunk *chunk = state->head;
		state->head = chunk->next;
		free_chunk(chunk);
	}
	close_segment(state, false);
	destroy_encoder(state);
	deque_free(&state->submitted_timestamps);
	bfree(state->session_dir);
	bfree(state->audio_dir);
	pthread_cond_destroy(&state->cond);
	pthread_mutex_destroy(&state->mutex);
	bfree(state);
}

bool sr_master_audio_acquire(void)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return false;
	char *audio_dir = join_path(session_dir, "audio-master");
	if (!audio_dir || os_mkdirs(audio_dir) == MKDIR_ERROR) {
		bfree(audio_dir);
		bfree(session_dir);
		return false;
	}

	pthread_mutex_lock(&state->mutex);
	if (!state->active_refs) {
		bfree(state->session_dir);
		bfree(state->audio_dir);
		state->session_dir = session_dir;
		state->audio_dir = audio_dir;
		state->target_segment_ns = (uint64_t)sr_config_get_segment_duration_ms() * 1000000ULL;
		state->min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY
						? 0
						: sr_config_get_min_free_bytes();
		state->reserve_blocked = false;
		state->reserve_recheck_after_ns = 0;
		state->next_sequence = find_next_sequence(state->audio_dir);
		state->enqueue_epoch++;
		state->active = true;
		session_dir = NULL;
		audio_dir = NULL;
	}
	state->active_refs++;
	pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->mutex);
	bfree(audio_dir);
	bfree(session_dir);
	return true;
}

void sr_master_audio_release(void)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return;

	pthread_mutex_lock(&state->mutex);
	if (state->active_refs)
		state->active_refs--;
	if (!state->active_refs && state->active) {
		state->active = false;
		state->enqueue_epoch++;
		pthread_cond_broadcast(&state->cond);
	}
	pthread_mutex_unlock(&state->mutex);
}

void sr_master_audio_get_stats(struct sr_master_audio_stats *stats)
{
	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));

	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return;
	pthread_mutex_lock(&state->mutex);
	*stats = state->stats;
	stats->queue_depth = state->queue_depth;
	pthread_mutex_unlock(&state->mutex);
}

struct sr_camera_audio_writer *sr_camera_audio_writer_create(const char *session_dir, const char *camera_name,
							     uint32_t sample_rate)
{
	if (!session_dir || !*session_dir || !camera_name || !*camera_name || !sample_rate)
		return NULL;
	char camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!sr_camera_key_from_name(camera_name, camera_key, sizeof(camera_key)))
		return NULL;
	char *camera_dir = sr_camera_directory_for_key(session_dir, camera_key);
	if (!camera_dir || os_mkdirs(camera_dir) == MKDIR_ERROR) {
		bfree(camera_dir);
		return NULL;
	}

	struct sr_master_audio_state *state = bzalloc(sizeof(*state));
	state->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;
	state->sample_rate = sample_rate;
	state->session_dir = bstrdup(session_dir);
	state->audio_dir = camera_dir;
	state->target_segment_ns = (uint64_t)sr_config_get_segment_duration_ms() * 1000000ULL;
	state->min_free_bytes =
		sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY ? 0 : sr_config_get_min_free_bytes();
	state->next_sequence = find_next_sequence(state->audio_dir);
	state->active = true;
	state->active_refs = 1;
	if (pthread_mutex_init(&state->mutex, NULL) != 0) {
		bfree(state->session_dir);
		bfree(state->audio_dir);
		bfree(state);
		return NULL;
	}
	if (pthread_cond_init(&state->cond, NULL) != 0) {
		pthread_mutex_destroy(&state->mutex);
		bfree(state->session_dir);
		bfree(state->audio_dir);
		bfree(state);
		return NULL;
	}
	if (pthread_create(&state->thread, NULL, audio_worker, state) != 0) {
		pthread_cond_destroy(&state->cond);
		pthread_mutex_destroy(&state->mutex);
		bfree(state->session_dir);
		bfree(state->audio_dir);
		bfree(state);
		return NULL;
	}
	state->thread_started = true;

	struct sr_camera_audio_writer *writer = bzalloc(sizeof(*writer));
	writer->state = state;
	blog(LOG_INFO, "Pitel Instant Replay: camera replay audio recording started for '%s' (%u Hz stereo AAC)",
	     camera_name, sample_rate);
	return writer;
}

void sr_camera_audio_writer_destroy(struct sr_camera_audio_writer *writer)
{
	if (!writer)
		return;
	struct sr_master_audio_state *state = writer->state;
	if (state) {
		pthread_mutex_lock(&state->mutex);
		state->stopping = true;
		state->active = false;
		state->active_refs = 0;
		pthread_cond_broadcast(&state->cond);
		pthread_mutex_unlock(&state->mutex);
		if (state->thread_started)
			pthread_join(state->thread, NULL);
		while (state->head) {
			struct sr_master_audio_chunk *chunk = state->head;
			state->head = chunk->next;
			free_chunk(chunk);
		}
		close_segment(state, false);
		destroy_encoder(state);
		deque_free(&state->submitted_timestamps);
		bfree(state->session_dir);
		bfree(state->audio_dir);
		pthread_cond_destroy(&state->cond);
		pthread_mutex_destroy(&state->mutex);
		bfree(state);
	}
	bfree(writer);
}

bool sr_camera_audio_writer_push(struct sr_camera_audio_writer *writer, const struct obs_audio_data *audio,
				 size_t channels, uint64_t timestamp_ns)
{
	if (!writer || !writer->state || !audio || !audio->frames || !audio->data[0])
		return false;
	const uint8_t *right = channels > 1 ? audio->data[1] : audio->data[0];
	return enqueue_audio(writer->state, audio->data[0], right, audio->frames,
			     timestamp_ns ? timestamp_ns : audio->timestamp);
}
