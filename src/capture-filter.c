/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "sr-buffer.h"
#include "sr-camera-identity.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-credit.h"
#include "sr-config.h"
#include "sr-master-audio.h"
#include "sr-session.h"
#include "sr-segment-writer.h"

#include <string.h>

#define S_DURATION "duration_ms"
#define S_ENCODER "encoder"
#define S_QUALITY "quality"
#define S_GOP "gop_ms"
#define S_DISK_RECORDING "disk_recording"

struct sr_capture {
	obs_source_t *self;
	pthread_mutex_t parent_mutex;
	obs_weak_source_t *parent_weak;
	char camera_name[256];
	struct sr_buffer buffer;
	struct sr_encoder *encoder;
	struct sr_gpu_encoder *gpu_encoder;
	struct sr_segment_writer *writer;
	struct sr_camera_audio_writer *camera_audio_writer;
	pthread_mutex_t camera_audio_mutex;
	pthread_mutex_t encode_mutex;

	enum sr_encoder_backend backend;
	int qp;
	uint32_t gop_ms;
	bool disk_recording;
	bool restart_writer;
	bool parent_showing_held;
	bool writer_failed;
	bool master_audio_acquired;

	/* Format the current encoder was opened with. The GPU encoder is created
	 * from the OBS render callback after filter_video has observed the source
	 * dimensions at least once. */
	uint32_t enc_width;
	uint32_t enc_height;
	bool encoder_failed;
	bool reset_encoder;
	bool gpu_reset;
	bool gpu_failed;
	bool gpu_fallback_logged;
	enum sr_capture_gpu_fallback_reason gpu_fallback_reason;

	uint64_t encode_calls;
	uint64_t encode_time_ns_total;
	uint64_t encode_time_ns_last;

	/* The GPU path runs on the OBS video clock. Keep the most recent mapping
	 * back into the asynchronous source/device clock so the legacy RAM ring
	 * remains in the same timestamp domain as filter_audio. */
	uint64_t latest_source_ts;
	uint64_t latest_source_obs_ts;

	uint64_t last_stats_log;
	uint64_t last_status_publish;
	pthread_mutex_t status_mutex;
	struct sr_capture_recording_summary status;
	struct sr_capture_performance_entry performance_status;
};

static void sr_capture_gpu_render(void *data, uint32_t cx, uint32_t cy);

static void sr_capture_set_parent(struct sr_capture *c, obs_source_t *parent)
{
	if (!c)
		return;

	obs_weak_source_t *weak = parent ? obs_source_get_weak_source(parent) : NULL;
	pthread_mutex_lock(&c->parent_mutex);
	obs_weak_source_t *old = c->parent_weak;
	c->parent_weak = weak;
	if (parent) {
		const char *name = obs_source_get_name(parent);
		if (name) {
			strncpy(c->camera_name, name, sizeof(c->camera_name) - 1);
			c->camera_name[sizeof(c->camera_name) - 1] = '\0';
		}
	}
	pthread_mutex_unlock(&c->parent_mutex);

	if (old)
		obs_weak_source_release(old);
}

static obs_source_t *sr_capture_parent_ref(struct sr_capture *c)
{
	if (!c)
		return NULL;

	pthread_mutex_lock(&c->parent_mutex);
	obs_source_t *parent = c->parent_weak ? obs_weak_source_get_source(c->parent_weak) : NULL;
	pthread_mutex_unlock(&c->parent_mutex);
	return parent;
}

static void sr_capture_filter_add(void *data, obs_source_t *source)
{
	sr_capture_set_parent(data, source);
}

static void sr_capture_filter_remove(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	sr_capture_set_parent(data, NULL);
}

static void sr_capture_ensure_parent_from_filter_callback(struct sr_capture *c)
{
	if (!c)
		return;

	pthread_mutex_lock(&c->parent_mutex);
	const bool have_parent = c->parent_weak != NULL;
	pthread_mutex_unlock(&c->parent_mutex);
	if (have_parent)
		return;

	/* obs_filter_get_parent() is guaranteed by libobs inside filter_video.
	 * Cache only a weak reference here so the capture filter cannot keep its
	 * parent alive through a source/filter reference cycle. */
	obs_source_t *parent = obs_filter_get_parent(c->self);
	if (parent)
		sr_capture_set_parent(c, parent);
}

static bool gpu_backend_candidate(enum sr_encoder_backend backend)
{
#ifdef _WIN32
	return backend == SR_ENC_AUTO || backend == SR_ENC_NVENC || backend == SR_ENC_AMF;
#else
	UNUSED_PARAMETER(backend);
	return false;
#endif
}

static void reset_encode_metrics(struct sr_capture *c)
{
	if (!c)
		return;
	c->encode_calls = 0;
	c->encode_time_ns_total = 0;
	c->encode_time_ns_last = 0;
}

static void set_parent_showing_hold(struct sr_capture *c, bool hold)
{
	if (!c || hold == c->parent_showing_held)
		return;
	obs_source_t *parent = sr_capture_parent_ref(c);
	if (!parent)
		return;
	if (hold)
		obs_source_inc_showing(parent);
	else
		obs_source_dec_showing(parent);
	c->parent_showing_held = hold;
	obs_source_release(parent);
}

static void publish_status(struct sr_capture *c, uint64_t now, bool force)
{
	if (!c || (!force && c->last_status_publish && now - c->last_status_publish < 500000000ULL))
		return;

	struct sr_capture_recording_summary status = {
		.camera_count = 1,
		.requested_count = c->disk_recording ? 1 : 0,
		.active_count = c->writer ? 1 : 0,
		.failed_count = (c->writer_failed || c->encoder_failed) ? 1 : 0,
	};
	struct sr_capture_performance_entry performance = {
		.path = c->encoder_failed ? SR_CAPTURE_PERF_ERROR
			: c->gpu_encoder  ? SR_CAPTURE_PERF_GPU_D3D11
			: c->encoder      ? SR_CAPTURE_PERF_CPU
					  : SR_CAPTURE_PERF_WAITING,
		.gpu_fallback_reason = c->gpu_fallback_reason,
		.width = c->enc_width,
		.height = c->enc_height,
		.gop_ms = c->gop_ms,
		.qp = c->qp,
		.disk_requested = c->disk_recording,
		.writer_active = c->writer != NULL,
		.writer_failed = c->writer_failed,
		.encoder_failed = c->encoder_failed,
		.ram_bytes = sr_buffer_video_bytes(&c->buffer),
		.encode_calls = c->encode_calls,
		.encode_time_ns_total = c->encode_time_ns_total,
		.encode_time_ns_last = c->encode_time_ns_last,
	};

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		performance.fps_num = ovi.fps_num;
		performance.fps_den = ovi.fps_den;
	}

	const char *encoder_name = c->gpu_encoder ? sr_gpu_encoder_name(c->gpu_encoder)
				   : c->encoder   ? sr_encoder_name(c->encoder)
						  : NULL;
	if (encoder_name)
		strncpy(performance.encoder_name, encoder_name, sizeof(performance.encoder_name) - 1);

	if (c->writer) {
		struct sr_segment_writer_stats stats;
		sr_segment_writer_get_stats(c->writer, &stats);
		status.reserve_blocked_count = stats.reserve_blocked ? 1 : 0;
		status.packets_written = stats.packets_written;
		status.bytes_written = stats.bytes_written;
		if (stats.write_failed)
			status.failed_count = 1;

		performance.reserve_blocked = stats.reserve_blocked;
		performance.writer_failed = performance.writer_failed || stats.write_failed;
		performance.packets_written = stats.packets_written;
		performance.bytes_written = stats.bytes_written;
		performance.packets_dropped = stats.packets_dropped;
		performance.segments_finalized = stats.segments_finalized;
		performance.queue_depth = stats.queue_depth;
		performance.queue_high_watermark = stats.queue_high_watermark;
	}

	pthread_mutex_lock(&c->status_mutex);
	c->status = status;
	c->performance_status = performance;
	pthread_mutex_unlock(&c->status_mutex);
	c->last_status_publish = now;
}

static void publish_recording_intent(struct sr_capture *c)
{
	if (!c)
		return;
	pthread_mutex_lock(&c->status_mutex);
	c->status.camera_count = 1;
	c->status.requested_count = c->disk_recording ? 1 : 0;
	c->performance_status.disk_requested = c->disk_recording;
	if (!c->disk_recording) {
		c->status.failed_count = 0;
		c->status.reserve_blocked_count = 0;
	}
	pthread_mutex_unlock(&c->status_mutex);
}

struct sr_buffer *sr_capture_get_buffer(void *capture_data)
{
	struct sr_capture *c = capture_data;
	return c ? &c->buffer : NULL;
}

static const char *sr_capture_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SportsReplayCapture");
}

static void destroy_writer(struct sr_capture *c)
{
	pthread_mutex_lock(&c->camera_audio_mutex);
	struct sr_camera_audio_writer *camera_audio = c->camera_audio_writer;
	c->camera_audio_writer = NULL;
	pthread_mutex_unlock(&c->camera_audio_mutex);
	sr_camera_audio_writer_destroy(camera_audio);
	if (c->writer) {
		sr_segment_writer_destroy(c->writer);
		c->writer = NULL;
	}
	if (c->master_audio_acquired) {
		sr_master_audio_release();
		c->master_audio_acquired = false;
	}
	publish_status(c, 0, true);
}

static void apply_recording_intent(struct sr_capture *c)
{
	if (c->restart_writer) {
		destroy_writer(c);
		c->restart_writer = false;
	}
	if (!c->disk_recording && c->writer)
		destroy_writer(c);
	if (!c->disk_recording)
		set_parent_showing_hold(c, false);
}

static void sr_capture_update(void *data, obs_data_t *settings)
{
	struct sr_capture *c = data;
	pthread_mutex_lock(&c->encode_mutex);

	c->buffer.duration_ns = (uint64_t)obs_data_get_int(settings, S_DURATION) * 1000000ULL;

	const enum sr_encoder_backend backend = (enum sr_encoder_backend)obs_data_get_int(settings, S_ENCODER);
	const int qp = (int)obs_data_get_int(settings, S_QUALITY);
	uint32_t gop_ms = (uint32_t)obs_data_get_int(settings, S_GOP);
	if (gop_ms != SR_GOP_ALL_I && gop_ms != SR_GOP_250MS && gop_ms != SR_GOP_500MS && gop_ms != SR_GOP_1000MS)
		gop_ms = SR_GOP_500MS;
	const bool disk_recording = obs_data_get_bool(settings, S_DISK_RECORDING);

	if (backend != c->backend || qp != c->qp || gop_ms != c->gop_ms) {
		c->backend = backend;
		c->qp = qp;
		c->gop_ms = gop_ms;
		c->reset_encoder = true;
		c->gpu_reset = true;
		c->gpu_failed = false;
		c->gpu_fallback_logged = false;
		c->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
		c->encoder_failed = false;
		c->writer_failed = false;
		reset_encode_metrics(c);
	}

	if (disk_recording != c->disk_recording) {
		/* Only change intent here. The writer itself is created/destroyed by
		 * the serialized video/GPU callbacks so the UI cannot free it while
		 * either path is queueing an encoded packet. */
		if (!disk_recording)
			c->restart_writer = true;
		c->disk_recording = disk_recording;
		c->writer_failed = false;
		if (disk_recording)
			set_parent_showing_hold(c, true);
	}
	pthread_mutex_unlock(&c->encode_mutex);
	publish_recording_intent(c);
}

static void *sr_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *c = bzalloc(sizeof(struct sr_capture));
	c->self = source;
	pthread_mutex_init(&c->parent_mutex, NULL);
	pthread_mutex_init(&c->status_mutex, NULL);
	pthread_mutex_init(&c->camera_audio_mutex, NULL);
	pthread_mutex_init(&c->encode_mutex, NULL);
	sr_buffer_init(&c->buffer);
	c->backend = SR_ENC_AUTO;
	c->qp = 23;
	c->gop_ms = SR_GOP_500MS;
	sr_capture_update(c, settings);
	obs_add_main_render_callback(sr_capture_gpu_render, c);
	return c;
}

static void sr_capture_destroy(void *data)
{
	struct sr_capture *c = data;
	obs_remove_main_render_callback(sr_capture_gpu_render, c);
	set_parent_showing_hold(c, false);
	sr_capture_set_parent(c, NULL);

	pthread_mutex_lock(&c->encode_mutex);
	destroy_writer(c);
	sr_encoder_destroy(c->encoder);
	c->encoder = NULL;
	struct sr_gpu_encoder *gpu_encoder = c->gpu_encoder;
	c->gpu_encoder = NULL;
	pthread_mutex_unlock(&c->encode_mutex);

	/* No render callback can reference the GPU encoder after removal. Destroy
	 * it outside encode_mutex because destruction enters the OBS graphics
	 * context and must not invert the render-thread lock order. */
	sr_gpu_encoder_destroy(gpu_encoder);
	sr_buffer_free(&c->buffer);
	pthread_mutex_destroy(&c->status_mutex);
	pthread_mutex_destroy(&c->parent_mutex);
	pthread_mutex_destroy(&c->camera_audio_mutex);
	pthread_mutex_destroy(&c->encode_mutex);
	bfree(c);
}

static obs_source_t *capture_camera_source_ref(struct sr_capture *c)
{
	return sr_capture_parent_ref(c);
}

static const char *capture_camera_name(struct sr_capture *c)
{
	return c && c->camera_name[0] ? c->camera_name : obs_source_get_name(c->self);
}

static bool capture_encoder_ready(const struct sr_capture *c)
{
	return c && (c->gpu_encoder || c->encoder);
}

static enum AVCodecID capture_encoder_codec_id(const struct sr_capture *c)
{
	if (c->gpu_encoder)
		return sr_gpu_encoder_codec_id(c->gpu_encoder);
	return c->encoder ? sr_encoder_codec_id(c->encoder) : AV_CODEC_ID_NONE;
}

static void capture_encoder_get_extradata(const struct sr_capture *c, const uint8_t **data, int *size)
{
	if (c->gpu_encoder)
		sr_gpu_encoder_get_extradata(c->gpu_encoder, data, size);
	else
		sr_encoder_get_extradata(c->encoder, data, size);
}

static void update_buffer_video_format(struct sr_capture *c)
{
	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	capture_encoder_get_extradata(c, &extradata, &extradata_size);

	pthread_mutex_lock(&c->buffer.mutex);
	c->buffer.codec_id = capture_encoder_codec_id(c);
	c->buffer.width = c->enc_width;
	c->buffer.height = c->enc_height;
	pthread_mutex_unlock(&c->buffer.mutex);
	sr_buffer_set_extradata(&c->buffer, extradata, extradata_size);
}

static bool ensure_writer(struct sr_capture *c, const struct obs_video_info *ovi)
{
	if (!c->disk_recording || c->writer || c->writer_failed || !capture_encoder_ready(c))
		return c->writer != NULL;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		c->writer_failed = true;
		return false;
	}

	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	capture_encoder_get_extradata(c, &extradata, &extradata_size);

	obs_source_t *camera_source = capture_camera_source_ref(c);
	char camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!camera_source || !sr_camera_key_from_source(camera_source, camera_key, sizeof(camera_key))) {
		obs_log(LOG_ERROR, "'%s': could not resolve persistent OBS UUID for replay camera '%s'",
			obs_source_get_name(c->self), capture_camera_name(c));
		if (camera_source)
			obs_source_release(camera_source);
		bfree(session_dir);
		c->writer_failed = true;
		return false;
	}
	obs_source_release(camera_source);

	struct sr_segment_writer_config cfg = {
		.session_dir = session_dir,
		.camera_name = capture_camera_name(c),
		.camera_key = camera_key,
		.codec_id = capture_encoder_codec_id(c),
		.width = c->enc_width,
		.height = c->enc_height,
		.fps_num = ovi->fps_num,
		.fps_den = ovi->fps_den,
		.extradata = extradata,
		.extradata_size = extradata_size,
		.target_segment_ms = sr_config_get_segment_duration_ms(),
		.min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY
					  ? 0
					  : sr_config_get_min_free_bytes(),
		.max_queue_packets = 600,
	};

	c->writer = sr_segment_writer_create(&cfg);
	if (!c->writer) {
		bfree(session_dir);
		c->writer_failed = true;
		obs_log(LOG_ERROR, "'%s': could not start continuous replay recorder", obs_source_get_name(c->self));
		return false;
	}

	if (!c->master_audio_acquired) {
		if (sr_master_audio_acquire())
			c->master_audio_acquired = true;
		else
			obs_log(LOG_WARNING,
				"'%s': continuous video is recording, but master replay audio could not start",
				obs_source_get_name(c->self));
	}
	struct obs_audio_info audio_info;
	if (!c->camera_audio_writer && obs_get_audio_info(&audio_info)) {
		struct sr_camera_audio_writer *camera_audio =
			sr_camera_audio_writer_create(session_dir, capture_camera_name(c), audio_info.samples_per_sec);
		pthread_mutex_lock(&c->camera_audio_mutex);
		c->camera_audio_writer = camera_audio;
		pthread_mutex_unlock(&c->camera_audio_mutex);
		if (!c->camera_audio_writer)
			obs_log(LOG_WARNING,
				"'%s': continuous video is recording, but selected-camera replay audio could not start",
				obs_source_get_name(c->self));
	}
	bfree(session_dir);
	publish_status(c, 0, true);
	return true;
}

static void log_buffer_stats(struct sr_capture *c, uint64_t now)
{
	if (c->last_stats_log && now - c->last_stats_log < 60000000000ULL)
		return;
	c->last_stats_log = now;

	const size_t bytes = sr_buffer_video_bytes(&c->buffer);
	if (!c->writer) {
		obs_log(LOG_INFO, "'%s': replay buffer using %.1f MB", obs_source_get_name(c->self),
			(double)bytes / (1024.0 * 1024.0));
		return;
	}

	struct sr_segment_writer_stats stats;
	sr_segment_writer_get_stats(c->writer, &stats);
	obs_log(LOG_INFO,
		"'%s': replay RAM %.1f MB; disk packets %llu, %.1f MB, segments %llu, queue %zu (peak %zu), dropped %llu%s%s",
		obs_source_get_name(c->self), (double)bytes / (1024.0 * 1024.0),
		(unsigned long long)stats.packets_written, (double)stats.bytes_written / (1024.0 * 1024.0),
		(unsigned long long)stats.segments_finalized, stats.queue_depth, stats.queue_high_watermark,
		(unsigned long long)stats.packets_dropped, stats.reserve_blocked ? ", DISK RESERVE" : "",
		stats.write_failed ? ", WRITE ERROR" : "");
}

static uint64_t mapped_source_timestamp(const struct sr_capture *c, uint64_t obs_timestamp)
{
	if (!c->latest_source_ts)
		return obs_timestamp;
	if (!c->latest_source_obs_ts || obs_timestamp <= c->latest_source_obs_ts)
		return c->latest_source_ts;
	return c->latest_source_ts + (obs_timestamp - c->latest_source_obs_ts);
}

static void sr_capture_gpu_render(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	struct sr_capture *c = data;
	if (!c)
		return;

	pthread_mutex_lock(&c->encode_mutex);
	apply_recording_intent(c);

	if (c->gpu_reset) {
		destroy_writer(c);
		sr_gpu_encoder_destroy(c->gpu_encoder);
		c->gpu_encoder = NULL;
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		c->gpu_reset = false;
		c->reset_encoder = false;
		c->encoder_failed = false;
		c->writer_failed = false;
		c->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
		reset_encode_metrics(c);
		sr_buffer_clear(&c->buffer);
	}

	if (!gpu_backend_candidate(c->backend) || c->gpu_failed || !c->enc_width || !c->enc_height) {
		pthread_mutex_unlock(&c->encode_mutex);
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		pthread_mutex_unlock(&c->encode_mutex);
		return;
	}

	if (!c->gpu_encoder) {
		c->gpu_encoder = sr_gpu_encoder_create(c->enc_width, c->enc_height, ovi.fps_num, ovi.fps_den,
						       c->backend, c->qp, c->gop_ms);
		if (!c->gpu_encoder) {
			c->gpu_failed = true;
			c->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_CREATE_FAILED;
			if (!c->gpu_fallback_logged) {
				obs_log(LOG_INFO,
					"'%s': native D3D11 capture encoder unavailable; continuing with CPU-frame encoder path",
					obs_source_get_name(c->self));
				c->gpu_fallback_logged = true;
			}
			publish_status(c, obs_get_video_frame_time(), true);
			pthread_mutex_unlock(&c->encode_mutex);
			return;
		}
		update_buffer_video_format(c);
		obs_log(LOG_INFO, "'%s': capture path is GPU-resident through %s (D3D11 render -> NV12 hwframe)",
			obs_source_get_name(c->self), sr_gpu_encoder_name(c->gpu_encoder));
	}

	/* The main render callback is outside libobs filter callbacks, where
	 * obs_filter_get_target()/get_parent() are not guaranteed valid. Resolve
	 * the weak parent cached by filter_add/filter_video into a strong ref for
	 * the duration of this render instead. */
	obs_source_t *target = capture_camera_source_ref(c);
	if (!target) {
		publish_status(c, obs_get_video_frame_time(), false);
		pthread_mutex_unlock(&c->encode_mutex);
		return;
	}
	AVPacket *pkt = NULL;
	const uint64_t encode_start = os_gettime_ns();
	const bool encode_ok = sr_gpu_encoder_render_encode(c->gpu_encoder, target, &pkt);
	obs_source_release(target);
	const uint64_t encode_elapsed = os_gettime_ns() - encode_start;
	c->encode_calls++;
	c->encode_time_ns_total += encode_elapsed;
	c->encode_time_ns_last = encode_elapsed;
	if (!encode_ok) {
		obs_log(LOG_WARNING,
			"'%s': GPU capture encoder failed; switching to CPU-frame fallback at a new segment",
			obs_source_get_name(c->self));
		destroy_writer(c);
		sr_gpu_encoder_destroy(c->gpu_encoder);
		c->gpu_encoder = NULL;
		c->gpu_failed = true;
		c->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_RUNTIME_FAILED;
		c->writer_failed = false;
		reset_encode_metrics(c);
		sr_buffer_clear(&c->buffer);
		publish_status(c, obs_get_video_frame_time(), true);
		pthread_mutex_unlock(&c->encode_mutex);
		return;
	}

	if (pkt) {
		/* Hardware encoders may publish SPS/PPS only after the first submitted
		 * frame. Refresh the RAM buffer header before storing the first packet. */
		update_buffer_video_format(c);
		const uint64_t replay_timestamp = obs_get_video_frame_time();
		ensure_writer(c, &ovi);
		if (c->writer)
			sr_segment_writer_push_video(c->writer, pkt, replay_timestamp);
		sr_buffer_push_video(&c->buffer, pkt, mapped_source_timestamp(c, replay_timestamp));
	}

	publish_status(c, obs_get_video_frame_time(), false);
	pthread_mutex_unlock(&c->encode_mutex);
}

static struct obs_source_frame *sr_capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;
	if (!c)
		return frame;

	sr_capture_ensure_parent_from_filter_callback(c);
	pthread_mutex_lock(&c->encode_mutex);
	apply_recording_intent(c);

	if (!frame || !frame->data[0]) {
		pthread_mutex_unlock(&c->encode_mutex);
		return frame;
	}

	c->latest_source_ts = frame->timestamp;
	c->latest_source_obs_ts = obs_get_video_frame_time();

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		pthread_mutex_unlock(&c->encode_mutex);
		return frame;
	}

	const bool size_changed = c->enc_width && c->enc_height &&
				  (frame->width != c->enc_width || frame->height != c->enc_height);
	if (c->reset_encoder || size_changed) {
		destroy_writer(c);
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		if (c->gpu_encoder)
			c->gpu_reset = true;
		c->reset_encoder = false;
		c->encoder_failed = false;
		c->gpu_failed = false;
		c->gpu_fallback_logged = false;
		c->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
		c->writer_failed = false;
		reset_encode_metrics(c);
		sr_buffer_clear(&c->buffer);
	}
	c->enc_width = frame->width;
	c->enc_height = frame->height;

	/* A live GPU encoder, or one waiting for render-thread creation/reset,
	 * owns video encoding. Do not run the CPU encoder in parallel: that would
	 * defeat the optimization and mix codec headers at the writer boundary. */
	if (c->gpu_encoder || c->gpu_reset || (gpu_backend_candidate(c->backend) && !c->gpu_failed)) {
		log_buffer_stats(c, frame->timestamp);
		publish_status(c, frame->timestamp, false);
		pthread_mutex_unlock(&c->encode_mutex);
		return frame;
	}

	if (c->encoder_failed) {
		pthread_mutex_unlock(&c->encode_mutex);
		return frame;
	}

	if (!c->encoder) {
		c->encoder = sr_encoder_create(frame->width, frame->height, ovi.fps_num, ovi.fps_den, c->backend, c->qp,
					       c->gop_ms);
		if (!c->encoder) {
			obs_log(LOG_ERROR, "'%s': no H.264 encoder available, replay capture disabled",
				obs_source_get_name(c->self));
			c->encoder_failed = true;
			publish_status(c, frame->timestamp, true);
			pthread_mutex_unlock(&c->encode_mutex);
			return frame;
		}
		update_buffer_video_format(c);
	}

	const uint64_t encode_start = os_gettime_ns();
	AVPacket *pkt = sr_encoder_encode(c->encoder, frame);
	const uint64_t encode_elapsed = os_gettime_ns() - encode_start;
	c->encode_calls++;
	c->encode_time_ns_total += encode_elapsed;
	c->encode_time_ns_last = encode_elapsed;
	if (pkt) {
		/* Async input frames retain the source/device timestamp. OBS maps
		 * those frames onto its own video clock when selecting them for the
		 * current render tick, but that private timing adjustment is not
		 * reflected in frame->timestamp. Event IN/OUT markers use the OBS
		 * video clock, so persist disk packets in that same global timebase. */
		const uint64_t replay_timestamp = obs_get_video_frame_time();

		/* Delay session/writer creation until the encoder has actually emitted
		 * a packet. Some codec implementations only finalize stream headers
		 * after encoding begins. */
		ensure_writer(c, &ovi);

		/* The disk writer clones the packet. The original remains owned by
		 * the legacy RAM ring buffer. */
		if (c->writer)
			sr_segment_writer_push_video(c->writer, pkt,
						     replay_timestamp ? replay_timestamp : frame->timestamp);
		sr_buffer_push_video(&c->buffer, pkt, frame->timestamp);
	}

	log_buffer_stats(c, frame->timestamp);
	publish_status(c, frame->timestamp, false);
	pthread_mutex_unlock(&c->encode_mutex);
	return frame;
}

static struct obs_audio_data *sr_capture_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct sr_capture *c = data;

	if (!c->buffer.samples_per_sec) {
		struct obs_audio_info oai;
		if (obs_get_audio_info(&oai)) {
			pthread_mutex_lock(&c->buffer.mutex);
			c->buffer.samples_per_sec = oai.samples_per_sec;
			c->buffer.speakers = oai.speakers;
			pthread_mutex_unlock(&c->buffer.mutex);
		}
	}

	if (c->buffer.samples_per_sec)
		sr_buffer_push_audio(&c->buffer, audio, get_audio_channels(c->buffer.speakers));
	pthread_mutex_lock(&c->camera_audio_mutex);
	if (c->camera_audio_writer)
		sr_camera_audio_writer_push(c->camera_audio_writer, audio, get_audio_channels(c->buffer.speakers),
					    obs_get_video_frame_time());
	pthread_mutex_unlock(&c->camera_audio_mutex);

	return audio;
}

struct capture_control_context {
	bool update_setting;
	bool enabled;
	struct sr_capture_recording_summary *summary;
	struct sr_capture_performance_snapshot *performance;
	size_t camera_count;
};

static void capture_control_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct capture_control_context *ctx = param;
	if (!ctx || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;

	ctx->camera_count++;
	if (ctx->update_setting) {
		obs_data_t *settings = obs_source_get_settings(child);
		if (ctx->enabled && obs_data_get_bool(settings, S_DISK_RECORDING)) {
			obs_data_set_bool(settings, S_DISK_RECORDING, false);
			obs_source_update(child, settings);
		}
		obs_data_set_bool(settings, S_DISK_RECORDING, ctx->enabled);
		obs_source_update(child, settings);
		obs_data_release(settings);
	}

	if (!ctx->summary && !ctx->performance)
		return;
	struct sr_capture *capture = obs_obj_get_data(child);
	if (!capture)
		return;

	struct sr_capture_recording_summary status;
	struct sr_capture_performance_entry performance;
	pthread_mutex_lock(&capture->status_mutex);
	status = capture->status;
	performance = capture->performance_status;
	pthread_mutex_unlock(&capture->status_mutex);

	if (ctx->summary) {
		ctx->summary->camera_count += status.camera_count;
		ctx->summary->requested_count += status.requested_count;
		ctx->summary->active_count += status.active_count;
		ctx->summary->failed_count += status.failed_count;
		ctx->summary->reserve_blocked_count += status.reserve_blocked_count;
		ctx->summary->packets_written += status.packets_written;
		ctx->summary->bytes_written += status.bytes_written;
	}

	if (ctx->performance) {
		const char *camera_name = capture_camera_name(capture);
		if (camera_name)
			strncpy(performance.camera_name, camera_name, sizeof(performance.camera_name) - 1);
		const size_t new_count = ctx->performance->count + 1;
		struct sr_capture_performance_entry *entries =
			brealloc(ctx->performance->entries, new_count * sizeof(*entries));
		if (entries) {
			ctx->performance->entries = entries;
			ctx->performance->entries[ctx->performance->count] = performance;
			ctx->performance->count = new_count;
		}
	}
}

static bool capture_control_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, capture_control_filter, param);
	return true;
}

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)
{
	struct capture_control_context ctx = {.update_setting = true, .enabled = enabled};
	obs_enum_sources(capture_control_source, &ctx);
	if (camera_count)
		*camera_count = ctx.camera_count;
	return true;
}

bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary)
{
	if (!summary)
		return false;
	memset(summary, 0, sizeof(*summary));
	struct capture_control_context ctx = {.summary = summary};
	obs_enum_sources(capture_control_source, &ctx);
	return true;
}

bool sr_capture_get_performance_snapshot(struct sr_capture_performance_snapshot *snapshot)
{
	if (!snapshot)
		return false;
	memset(snapshot, 0, sizeof(*snapshot));
	struct capture_control_context ctx = {.performance = snapshot};
	obs_enum_sources(capture_control_source, &ctx);
	return true;
}

void sr_capture_free_performance_snapshot(struct sr_capture_performance_snapshot *snapshot)
{
	if (!snapshot)
		return;
	bfree(snapshot->entries);
	memset(snapshot, 0, sizeof(*snapshot));
}

static obs_properties_t *sr_capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_int(props, S_DURATION, obs_module_text("Duration"), 1000, 120000, 500);
	obs_property_int_set_suffix(p, " ms");

	p = obs_properties_add_list(props, S_ENCODER, obs_module_text("Encoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Encoder.Auto"), SR_ENC_AUTO);
	obs_property_list_add_int(p, obs_module_text("Encoder.NVENC"), SR_ENC_NVENC);
	obs_property_list_add_int(p, obs_module_text("Encoder.AMF"), SR_ENC_AMF);
	obs_property_list_add_int(p, obs_module_text("Encoder.QSV"), SR_ENC_QSV);
	obs_property_list_add_int(p, obs_module_text("Encoder.X264"), SR_ENC_X264);

	p = obs_properties_add_list(props, S_QUALITY, obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Quality.High"), 18);
	obs_property_list_add_int(p, obs_module_text("Quality.Medium"), 23);
	obs_property_list_add_int(p, obs_module_text("Quality.Low"), 28);

	p = obs_properties_add_list(props, S_GOP, obs_module_text("GOP"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("GOP.AllI"), SR_GOP_ALL_I);
	obs_property_list_add_int(p, obs_module_text("GOP.Ultra"), SR_GOP_250MS);
	obs_property_list_add_int(p, obs_module_text("GOP.Balanced"), SR_GOP_500MS);
	obs_property_list_add_int(p, obs_module_text("GOP.Economy"), SR_GOP_1000MS);
	obs_property_set_long_description(p, obs_module_text("GOP.Description"));

	p = obs_properties_add_bool(props, S_DISK_RECORDING, obs_module_text("DiskRecording"));
	obs_property_set_long_description(p, obs_module_text("DiskRecording.Description"));

	p = obs_properties_add_int(props, S_SYNC_OFFSET_MS, obs_module_text("SyncOffset"), -SR_CAMERA_SYNC_MAX_MS,
				   SR_CAMERA_SYNC_MAX_MS, 1);
	obs_property_int_set_suffix(p, " ms");
	obs_property_set_long_description(p, obs_module_text("SyncOffset.Description"));

	char credit[256];
	obs_properties_add_text(props, "sr_credit", sr_plugin_credit_html(credit, sizeof(credit)), OBS_TEXT_INFO);
	return props;
}

static void sr_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_DURATION, 15000);
	obs_data_set_default_int(settings, S_ENCODER, SR_ENC_AUTO);
	obs_data_set_default_int(settings, S_QUALITY, 23);
	obs_data_set_default_int(settings, S_GOP, SR_GOP_500MS);
	obs_data_set_default_int(settings, S_SYNC_OFFSET_MS, 0);
	/* Keep new continuous recording opt-in while the disk engine is still
	 * running in parallel with the proven legacy replay buffer. */
	obs_data_set_default_bool(settings, S_DISK_RECORDING, false);
}

struct obs_source_info sr_capture_info = {
	.id = SR_CAPTURE_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = sr_capture_get_name,
	.create = sr_capture_create,
	.destroy = sr_capture_destroy,
	.update = sr_capture_update,
	.get_defaults = sr_capture_defaults,
	.get_properties = sr_capture_properties,
	.filter_add = sr_capture_filter_add,
	.filter_remove = sr_capture_filter_remove,
	.filter_video = sr_capture_filter_video,
	.filter_audio = sr_capture_filter_audio,
};
