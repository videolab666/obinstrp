/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-program-recorder.h"

#include "sr-camera-identity.h"
#include "sr-capture.h"
#include "sr-codec.h"
#include "sr-config.h"
#include "sr-gpu-video.h"
#include "sr-master-audio.h"
#include "sr-segment-writer.h"
#include "sr-session.h"

#include <graphics/graphics.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

#define PROGRAM_QP 23
#define PROGRAM_GOP_MS SR_GOP_500MS

struct sr_program_recorder {
	pthread_mutex_t mutex;
	bool initialized;
	bool callback_registered;
	bool raw_callback_registered;
	bool recording_requested;
	bool encoder_failed;
	bool writer_failed;
	bool master_audio_acquired;
	bool gpu_supported;
	bool raw_supported;

	struct sr_gpu_encoder *encoder;
	struct sr_encoder *raw_encoder;
	struct sr_segment_writer *writer;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;

	uint64_t recording_start_ns;
	uint64_t encode_calls;
	uint64_t encode_time_ns_total;
	uint64_t encode_time_ns_last;
};

static struct sr_program_recorder g_program;

bool sr_program_recorder_supported(void)
{
#ifdef _WIN32
	return g_program.initialized && (g_program.gpu_supported || g_program.raw_supported);
#else
	return false;
#endif
}

bool sr_program_recorder_selected(void)
{
	return sr_program_recorder_supported() && sr_config_get_program_output_enabled();
}

static const char *active_encoder_name_locked(const struct sr_program_recorder *state)
{
	if (state->encoder)
		return sr_gpu_encoder_name(state->encoder);
	if (state->raw_encoder)
		return sr_encoder_name(state->raw_encoder);
	return NULL;
}

static enum AVCodecID active_encoder_codec_id_locked(const struct sr_program_recorder *state)
{
	if (state->encoder)
		return sr_gpu_encoder_codec_id(state->encoder);
	if (state->raw_encoder)
		return sr_encoder_codec_id(state->raw_encoder);
	return AV_CODEC_ID_NONE;
}

static void active_encoder_get_extradata_locked(const struct sr_program_recorder *state, const uint8_t **data,
						 int *size)
{
	if (data)
		*data = NULL;
	if (size)
		*size = 0;
	if (state->encoder)
		sr_gpu_encoder_get_extradata(state->encoder, data, size);
	else if (state->raw_encoder)
		sr_encoder_get_extradata(state->raw_encoder, data, size);
}

static void release_resources_locked(struct sr_program_recorder *state)
{
	if (state->writer) {
		sr_segment_writer_destroy(state->writer);
		state->writer = NULL;
	}
	if (state->master_audio_acquired) {
		sr_master_audio_release();
		state->master_audio_acquired = false;
	}
	sr_gpu_encoder_destroy(state->encoder);
	state->encoder = NULL;
	sr_encoder_destroy(state->raw_encoder);
	state->raw_encoder = NULL;
	state->width = 0;
	state->height = 0;
	state->fps_num = 0;
	state->fps_den = 0;
	state->encode_calls = 0;
	state->encode_time_ns_total = 0;
	state->encode_time_ns_last = 0;
}

static bool ensure_writer_locked(struct sr_program_recorder *state)
{
	if (state->writer)
		return true;
	if ((!state->encoder && !state->raw_encoder) || state->writer_failed)
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		state->writer_failed = true;
		return false;
	}

	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	active_encoder_get_extradata_locked(state, &extradata, &extradata_size);
	const struct sr_segment_writer_config config = {
		.session_dir = session_dir,
		.camera_name = SR_PROGRAM_CAMERA_NAME,
		.camera_key = SR_PROGRAM_CAMERA_KEY,
		.codec_id = active_encoder_codec_id_locked(state),
		.width = state->width,
		.height = state->height,
		.fps_num = state->fps_num,
		.fps_den = state->fps_den,
		.extradata = extradata,
		.extradata_size = extradata_size,
		.target_segment_ms = sr_config_get_segment_duration_ms(),
		.max_queue_packets = 600,
		.min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY
					  ? 0
					  : sr_config_get_min_free_bytes(),
	};

	state->writer = sr_segment_writer_create(&config);
	bfree(session_dir);
	if (!state->writer) {
		state->writer_failed = true;
		blog(LOG_ERROR, "Pitel Instant Replay: could not start PROGRAM continuous replay writer");
		return false;
	}

	if (!state->master_audio_acquired) {
		if (sr_master_audio_acquire())
			state->master_audio_acquired = true;
		else
			blog(LOG_WARNING,
			     "Pitel Instant Replay: PROGRAM video is recording, but master replay audio could not start");
	}

	const char *encoder_name = active_encoder_name_locked(state);
	blog(LOG_INFO, "Pitel Instant Replay: PROGRAM replay recorder started (%ux%u @ %.3f fps, %s)", state->width,
	     state->height, (double)state->fps_num / (double)state->fps_den, encoder_name ? encoder_name : "unknown");
	return true;
}

static void program_rendered(void *param)
{
	struct sr_program_recorder *state = param;
	if (!state)
		return;

	pthread_mutex_lock(&state->mutex);
	if (!state->gpu_supported || !state->recording_requested || !sr_program_recorder_selected() ||
	    !sr_program_recorder_supported()) {
		if (state->encoder || state->writer)
			release_resources_locked(state);
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	struct obs_video_info video = {0};
	gs_texture_t *texture = obs_get_main_texture();
	if (!texture || !obs_get_video_info(&video) || !video.fps_num || !video.fps_den) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	uint32_t width = gs_texture_get_width(texture) & ~1u;
	uint32_t height = gs_texture_get_height(texture) & ~1u;
	if (!width || !height) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	if ((state->width && state->height && (state->width != width || state->height != height)) ||
	    (state->fps_num && state->fps_den &&
	     (state->fps_num != video.fps_num || state->fps_den != video.fps_den))) {
		release_resources_locked(state);
		state->encoder_failed = false;
		state->writer_failed = false;
	}

	state->width = width;
	state->height = height;
	state->fps_num = video.fps_num;
	state->fps_den = video.fps_den;

	if (!state->encoder && !state->encoder_failed) {
		state->encoder = sr_gpu_encoder_create(width, height, video.fps_num, video.fps_den, SR_ENC_AUTO,
						       PROGRAM_QP, PROGRAM_GOP_MS);
		if (!state->encoder) {
			state->encoder_failed = true;
			blog(LOG_ERROR,
			     "Pitel Instant Replay: PROGRAM recorder could not open the hardware encoder on the active OBS D3D11 adapter");
			pthread_mutex_unlock(&state->mutex);
			return;
		}
	}

	if (!state->encoder) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	AVPacket *packet = NULL;
	const uint64_t encode_start = os_gettime_ns();
	const bool encoded = sr_gpu_encoder_texture_encode(state->encoder, texture, &packet);
	const uint64_t elapsed = os_gettime_ns() - encode_start;
	state->encode_calls++;
	state->encode_time_ns_total += elapsed;
	state->encode_time_ns_last = elapsed;

	if (!encoded) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: PROGRAM D3D11 encode failed; recorder is disabled until REC is restarted");
		release_resources_locked(state);
		state->encoder_failed = true;
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	if (packet) {
		const uint64_t timestamp_ns = obs_get_video_frame_time();
		if (ensure_writer_locked(state) && state->writer)
			sr_segment_writer_push_video(state->writer, packet, timestamp_ns);
		av_packet_free(&packet);
	}

	pthread_mutex_unlock(&state->mutex);
}

static void program_raw_video(void *param, struct video_data *frame)
{
	struct sr_program_recorder *state = param;
	if (!state || !frame)
		return;

	pthread_mutex_lock(&state->mutex);
	if (!state->raw_supported || !state->raw_callback_registered || !state->recording_requested ||
	    !sr_program_recorder_selected() || !sr_program_recorder_supported()) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	if (!state->width || !state->height || !state->fps_num || !state->fps_den || !frame->data[0] ||
	    !frame->data[1]) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	if (!state->raw_encoder && !state->encoder_failed) {
		/* h264_qsv accepts system-memory NV12. sr_encoder_create() already
		 * falls back to libx264 if QSV is unavailable, so Intel Program keeps
		 * recording instead of silently disabling itself. The raw video
		 * callback is OBS's final Program output and is only connected while
		 * REC is active. */
		state->raw_encoder = sr_encoder_create(state->width, state->height, state->fps_num, state->fps_den,
						       SR_ENC_QSV, PROGRAM_QP, PROGRAM_GOP_MS);
		if (!state->raw_encoder) {
			state->encoder_failed = true;
			blog(LOG_ERROR,
			     "Pitel Instant Replay: PROGRAM Intel path could not open QSV or the libx264 fallback");
			pthread_mutex_unlock(&state->mutex);
			return;
		}

		const char *name = sr_encoder_name(state->raw_encoder);
		if (name && strcmp(name, "h264_qsv") == 0)
			blog(LOG_INFO, "Pitel Instant Replay: PROGRAM Intel path is using QSV from OBS NV12 output");
		else
			blog(LOG_WARNING, "Pitel Instant Replay: Intel QSV unavailable; PROGRAM is using '%s' fallback",
			     name ? name : "unknown");
	}

	if (!state->raw_encoder) {
		pthread_mutex_unlock(&state->mutex);
		return;
	}

	struct obs_source_frame input = {0};
	input.width = state->width;
	input.height = state->height;
	input.timestamp = frame->timestamp;
	input.format = VIDEO_FORMAT_NV12;
	input.full_range = false;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		input.data[i] = frame->data[i];
		input.linesize[i] = frame->linesize[i];
	}

	const uint64_t encode_start = os_gettime_ns();
	AVPacket *packet = sr_encoder_encode(state->raw_encoder, &input);
	const uint64_t elapsed = os_gettime_ns() - encode_start;
	state->encode_calls++;
	state->encode_time_ns_total += elapsed;
	state->encode_time_ns_last = elapsed;

	/* A hardware encoder may legally buffer a frame and return no packet yet.
	 * sr_encoder_encode() also owns the QSV->libx264 creation fallback, so a
	 * NULL packet here is not treated as a fatal recorder error. */
	if (packet) {
		if (ensure_writer_locked(state) && state->writer)
			sr_segment_writer_push_video(state->writer, packet, frame->timestamp);
		av_packet_free(&packet);
	}

	pthread_mutex_unlock(&state->mutex);
}

bool sr_program_recorder_init(void)
{
	if (g_program.initialized)
		return true;
	memset(&g_program, 0, sizeof(g_program));
	if (pthread_mutex_init(&g_program.mutex, NULL) != 0)
		return false;
	g_program.initialized = true;

#ifdef _WIN32
	const uint32_t vendor = sr_gpu_active_adapter_vendor_id();
	g_program.gpu_supported = sr_gpu_program_texture_encode_available();
	g_program.raw_supported = vendor == SR_GPU_VENDOR_ID_INTEL;
	if (g_program.gpu_supported) {
		obs_add_main_rendered_callback(program_rendered, &g_program);
		g_program.callback_registered = true;
	} else if (g_program.raw_supported) {
		blog(LOG_INFO,
		     "Pitel Instant Replay: PROGRAM Intel path enabled; REC will request OBS NV12 output and encode with QSV (libx264 fallback)");
	} else {
		blog(LOG_INFO,
		     "Pitel Instant Replay: PROGRAM recorder disabled on the active OBS adapter; supported adapters are NVIDIA, AMD and Intel");
	}
#endif
	return true;
}

void sr_program_recorder_free(void)
{
	if (!g_program.initialized)
		return;

	if (g_program.raw_callback_registered) {
		pthread_mutex_lock(&g_program.mutex);
		g_program.recording_requested = false;
		g_program.raw_callback_registered = false;
		pthread_mutex_unlock(&g_program.mutex);
		obs_remove_raw_video_callback(program_raw_video, &g_program);
	}
	if (g_program.callback_registered)
		obs_remove_main_rendered_callback(program_rendered, &g_program);

	pthread_mutex_lock(&g_program.mutex);
	g_program.recording_requested = false;
	release_resources_locked(&g_program);
	pthread_mutex_unlock(&g_program.mutex);
	pthread_mutex_destroy(&g_program.mutex);
	memset(&g_program, 0, sizeof(g_program));
}

void sr_program_recorder_set_selected(bool enabled)
{
	if (enabled && !sr_program_recorder_supported())
		enabled = false;
	sr_config_set_program_output_enabled(enabled);
	if (!enabled)
		sr_program_recorder_set_recording(false);
}

bool sr_program_recorder_set_recording(bool enabled)
{
	if (!g_program.initialized)
		return false;
	if (enabled && (!sr_program_recorder_selected() || !sr_program_recorder_supported()))
		return false;

	struct video_scale_info raw_conversion = {0};
	uint32_t raw_width = 0;
	uint32_t raw_height = 0;
	uint32_t raw_fps_num = 0;
	uint32_t raw_fps_den = 0;
	if (enabled && g_program.raw_supported) {
		struct obs_video_info video = {0};
		if (!obs_get_video_info(&video) || !video.output_width || !video.output_height || !video.fps_num ||
		    !video.fps_den)
			return false;

		raw_width = video.output_width & ~1u;
		raw_height = video.output_height & ~1u;
		raw_fps_num = video.fps_num;
		raw_fps_den = video.fps_den;
		if (!raw_width || !raw_height)
			return false;

		raw_conversion.format = VIDEO_FORMAT_NV12;
		raw_conversion.width = raw_width;
		raw_conversion.height = raw_height;
		raw_conversion.range = VIDEO_RANGE_PARTIAL;
		raw_conversion.colorspace = VIDEO_CS_709;
	}

	bool add_raw_callback = false;
	bool remove_raw_callback = false;
	pthread_mutex_lock(&g_program.mutex);
	const bool was_requested = g_program.recording_requested;
	g_program.recording_requested = enabled;
	if (enabled) {
		if (!was_requested)
			g_program.recording_start_ns = obs_get_video_frame_time();
		g_program.encoder_failed = false;
		g_program.writer_failed = false;

		if (g_program.raw_supported && !g_program.raw_callback_registered) {
			g_program.width = raw_width;
			g_program.height = raw_height;
			g_program.fps_num = raw_fps_num;
			g_program.fps_den = raw_fps_den;
			g_program.raw_callback_registered = true;
			add_raw_callback = true;
		}
	} else {
		g_program.recording_start_ns = 0;
		if (g_program.raw_supported && g_program.raw_callback_registered) {
			/* Mark it inactive before disconnecting so an in-flight callback
			 * that was waiting on the mutex exits without touching encoders. */
			g_program.raw_callback_registered = false;
			remove_raw_callback = true;
		} else if (!g_program.callback_registered) {
			release_resources_locked(&g_program);
		}
	}
	pthread_mutex_unlock(&g_program.mutex);

	if (remove_raw_callback) {
		obs_remove_raw_video_callback(program_raw_video, &g_program);
		pthread_mutex_lock(&g_program.mutex);
		release_resources_locked(&g_program);
		pthread_mutex_unlock(&g_program.mutex);
	}

	if (add_raw_callback) {
		obs_add_raw_video_callback(&raw_conversion, program_raw_video, &g_program);
		blog(LOG_INFO, "Pitel Instant Replay: PROGRAM Intel NV12 callback connected (%ux%u @ %.3f fps)",
		     raw_width, raw_height, (double)raw_fps_num / (double)raw_fps_den);
	}

	return true;
}

bool sr_program_recorder_recording_requested(void)
{
	if (!g_program.initialized)
		return false;
	pthread_mutex_lock(&g_program.mutex);
	const bool requested = g_program.recording_requested;
	pthread_mutex_unlock(&g_program.mutex);
	return requested;
}

void sr_program_recorder_add_recording_summary(struct sr_capture_recording_summary *summary)
{
	if (!summary || !sr_program_recorder_selected())
		return;

	pthread_mutex_lock(&g_program.mutex);
	summary->camera_count++;
	if (g_program.recording_requested)
		summary->requested_count++;
	if (g_program.writer)
		summary->active_count++;
	if (g_program.encoder_failed || g_program.writer_failed)
		summary->failed_count++;
	if (g_program.recording_requested && g_program.recording_start_ns) {
		if (!summary->recording_start_ns || g_program.recording_start_ns < summary->recording_start_ns)
			summary->recording_start_ns = g_program.recording_start_ns;
		const uint64_t now = obs_get_video_frame_time();
		if (now >= g_program.recording_start_ns) {
			const uint64_t duration = now - g_program.recording_start_ns;
			if (duration > summary->recording_duration_ns)
				summary->recording_duration_ns = duration;
		}
	}
	if (g_program.writer) {
		struct sr_segment_writer_stats stats = {0};
		sr_segment_writer_get_stats(g_program.writer, &stats);
		if (stats.reserve_blocked)
			summary->reserve_blocked_count++;
		if (stats.write_failed)
			summary->failed_count++;
		summary->packets_written += stats.packets_written;
		summary->bytes_written += stats.bytes_written;
	}
	pthread_mutex_unlock(&g_program.mutex);
}

bool sr_program_recorder_get_performance_entry(struct sr_capture_performance_entry *entry)
{
	if (!entry || !sr_program_recorder_selected())
		return false;

	memset(entry, 0, sizeof(*entry));
	pthread_mutex_lock(&g_program.mutex);
	strncpy(entry->camera_name, SR_PROGRAM_CAMERA_NAME, sizeof(entry->camera_name) - 1);
	const char *encoder_name = active_encoder_name_locked(&g_program);
	if (encoder_name)
		strncpy(entry->encoder_name, encoder_name, sizeof(entry->encoder_name) - 1);
	entry->path = g_program.encoder_failed ? SR_CAPTURE_PERF_ERROR
		      : g_program.encoder      ? SR_CAPTURE_PERF_GPU_D3D11
		      : g_program.raw_encoder  ? SR_CAPTURE_PERF_CPU
					       : SR_CAPTURE_PERF_WAITING;
	entry->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
	entry->width = g_program.width;
	entry->height = g_program.height;
	entry->fps_num = g_program.fps_num;
	entry->fps_den = g_program.fps_den;
	entry->gop_ms = PROGRAM_GOP_MS;
	entry->qp = PROGRAM_QP;
	entry->disk_requested = g_program.recording_requested;
	entry->writer_active = g_program.writer != NULL;
	entry->writer_failed = g_program.writer_failed;
	entry->encoder_failed = g_program.encoder_failed;
	entry->encode_calls = g_program.encode_calls;
	entry->encode_time_ns_total = g_program.encode_time_ns_total;
	entry->encode_time_ns_last = g_program.encode_time_ns_last;
	if (g_program.writer) {
		struct sr_segment_writer_stats stats = {0};
		sr_segment_writer_get_stats(g_program.writer, &stats);
		entry->reserve_blocked = stats.reserve_blocked;
		entry->writer_failed = entry->writer_failed || stats.write_failed;
		entry->packets_written = stats.packets_written;
		entry->bytes_written = stats.bytes_written;
		entry->packets_dropped = stats.packets_dropped;
		entry->segments_finalized = stats.segments_finalized;
		entry->queue_depth = stats.queue_depth;
		entry->queue_high_watermark = stats.queue_high_watermark;
	}
	pthread_mutex_unlock(&g_program.mutex);
	return true;
}
