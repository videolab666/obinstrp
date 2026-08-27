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
	bool recording_requested;
	bool encoder_failed;
	bool writer_failed;
	bool master_audio_acquired;
	bool gpu_supported;

	struct sr_gpu_encoder *encoder;
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
	return g_program.initialized && g_program.gpu_supported;
#else
	return false;
#endif
}

bool sr_program_recorder_selected(void)
{
	return sr_program_recorder_supported() && sr_config_get_program_output_enabled();
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
	if (!state->encoder || state->writer_failed)
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		state->writer_failed = true;
		return false;
	}

	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	sr_gpu_encoder_get_extradata(state->encoder, &extradata, &extradata_size);
	const struct sr_segment_writer_config config = {
		.session_dir = session_dir,
		.camera_name = SR_PROGRAM_CAMERA_NAME,
		.camera_key = SR_PROGRAM_CAMERA_KEY,
		.codec_id = sr_gpu_encoder_codec_id(state->encoder),
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

	blog(LOG_INFO, "Pitel Instant Replay: PROGRAM replay recorder started (%ux%u @ %.3f fps)", state->width,
	     state->height, (double)state->fps_num / (double)state->fps_den);
	return true;
}

static void program_rendered(void *param)
{
	struct sr_program_recorder *state = param;
	if (!state)
		return;

	pthread_mutex_lock(&state->mutex);
	if (!state->recording_requested || !sr_program_recorder_selected() || !sr_program_recorder_supported()) {
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

bool sr_program_recorder_init(void)
{
	if (g_program.initialized)
		return true;
	memset(&g_program, 0, sizeof(g_program));
	if (pthread_mutex_init(&g_program.mutex, NULL) != 0)
		return false;
	g_program.initialized = true;

#ifdef _WIN32
	g_program.gpu_supported = sr_gpu_program_texture_encode_available();
	if (g_program.gpu_supported) {
		obs_add_main_rendered_callback(program_rendered, &g_program);
		g_program.callback_registered = true;
	} else {
		blog(LOG_INFO,
		     "Pitel Instant Replay: PROGRAM GPU recorder disabled on the active OBS adapter; NVENC requires OBS on NVIDIA and AMF requires OBS on AMD");
	}
#endif
	return true;
}

void sr_program_recorder_free(void)
{
	if (!g_program.initialized)
		return;
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

	pthread_mutex_lock(&g_program.mutex);
	const bool was_requested = g_program.recording_requested;
	g_program.recording_requested = enabled;
	if (enabled) {
		if (!was_requested)
			g_program.recording_start_ns = obs_get_video_frame_time();
		g_program.encoder_failed = false;
		g_program.writer_failed = false;
	} else {
		g_program.recording_start_ns = 0;
		if (!g_program.callback_registered)
			release_resources_locked(&g_program);
	}
	pthread_mutex_unlock(&g_program.mutex);
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
	const char *encoder_name = g_program.encoder ? sr_gpu_encoder_name(g_program.encoder) : NULL;
	if (encoder_name)
		strncpy(entry->encoder_name, encoder_name, sizeof(entry->encoder_name) - 1);
	entry->path = g_program.encoder_failed ? SR_CAPTURE_PERF_ERROR
		      : g_program.encoder      ? SR_CAPTURE_PERF_GPU_D3D11
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
