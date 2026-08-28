/*
 * Pitel Instant Replay - disk-only OBS capture filter
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include "sr-camera-identity.h"
#include "sr-capture.h"
#include "sr-codec.h"
#include "sr-config.h"
#include "sr-master-audio.h"
#include "sr-program-recorder.h"
#include "sr-segment-writer.h"
#include "sr-session.h"

#include <string.h>

#define S_ENCODER "encoder"
#define S_QUALITY "quality"
#define S_GOP "gop_ms"
#define S_DISK_RECORDING SR_CAPTURE_SETTING_DISK_RECORDING

struct sr_capture {
	obs_source_t *self;

	pthread_mutex_t parent_mutex;
	obs_weak_source_t *parent_weak;
	char camera_name[256];

	pthread_mutex_t encode_mutex;
	struct sr_encoder *cpu_encoder;
	struct sr_gpu_encoder *gpu_encoder;
	struct sr_segment_writer *writer;

	pthread_mutex_t camera_audio_mutex;
	struct sr_camera_audio_writer *camera_audio_writer;
	uint32_t audio_sample_rate;
	size_t audio_channels;

	enum sr_encoder_backend backend;
	int qp;
	uint32_t gop_ms;
	bool disk_recording;
	bool restart_writer;
	bool parent_showing_held;
	bool writer_failed;
	bool master_audio_acquired;
	uint64_t recording_obs_start_ns;

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
	uint64_t last_stats_log;
	uint64_t last_status_publish;

	pthread_mutex_t status_mutex;
	struct sr_capture_recording_summary status;
	struct sr_capture_performance_entry performance_status;
};

static void capture_gpu_render(void *data, uint32_t cx, uint32_t cy);

static const char *capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PitelInstantReplayCapture");
}

static void set_parent(struct sr_capture *capture, obs_source_t *parent)
{
	if (!capture)
		return;

	obs_weak_source_t *next = parent ? obs_source_get_weak_source(parent) : NULL;
	pthread_mutex_lock(&capture->parent_mutex);
	obs_weak_source_t *previous = capture->parent_weak;
	capture->parent_weak = next;
	if (parent) {
		const char *name = obs_source_get_name(parent);
		if (name) {
			strncpy(capture->camera_name, name, sizeof(capture->camera_name) - 1);
			capture->camera_name[sizeof(capture->camera_name) - 1] = '\0';
		}
	}
	pthread_mutex_unlock(&capture->parent_mutex);

	if (previous)
		obs_weak_source_release(previous);
}

static obs_source_t *parent_ref(struct sr_capture *capture)
{
	if (!capture)
		return NULL;
	pthread_mutex_lock(&capture->parent_mutex);
	obs_source_t *parent = capture->parent_weak ? obs_weak_source_get_source(capture->parent_weak) : NULL;
	pthread_mutex_unlock(&capture->parent_mutex);
	return parent;
}

static void filter_add(void *data, obs_source_t *parent)
{
	set_parent(data, parent);
}

static void filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	set_parent(data, NULL);
}

static void ensure_parent_from_filter_callback(struct sr_capture *capture)
{
	pthread_mutex_lock(&capture->parent_mutex);
	const bool known = capture->parent_weak != NULL;
	pthread_mutex_unlock(&capture->parent_mutex);
	if (known)
		return;

	obs_source_t *parent = obs_filter_get_parent(capture->self);
	if (parent)
		set_parent(capture, parent);
}

static const char *camera_name(const struct sr_capture *capture)
{
	if (!capture)
		return "";
	return capture->camera_name[0] ? capture->camera_name : obs_source_get_name(capture->self);
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

static void reset_encode_metrics(struct sr_capture *capture)
{
	capture->encode_calls = 0;
	capture->encode_time_ns_total = 0;
	capture->encode_time_ns_last = 0;
}

static void set_showing_hold(struct sr_capture *capture, bool hold)
{
	if (!capture || capture->parent_showing_held == hold)
		return;
	obs_source_t *parent = parent_ref(capture);
	if (!parent)
		return;
	if (hold)
		obs_source_inc_showing(parent);
	else
		obs_source_dec_showing(parent);
	capture->parent_showing_held = hold;
	obs_source_release(parent);
}

static bool encoder_ready(const struct sr_capture *capture)
{
	return capture && (capture->gpu_encoder || capture->cpu_encoder);
}

static enum AVCodecID encoder_codec_id(const struct sr_capture *capture)
{
	if (capture->gpu_encoder)
		return sr_gpu_encoder_codec_id(capture->gpu_encoder);
	return capture->cpu_encoder ? sr_encoder_codec_id(capture->cpu_encoder) : AV_CODEC_ID_NONE;
}

static const char *encoder_name(const struct sr_capture *capture)
{
	if (capture->gpu_encoder)
		return sr_gpu_encoder_name(capture->gpu_encoder);
	return capture->cpu_encoder ? sr_encoder_name(capture->cpu_encoder) : NULL;
}

static void encoder_extradata(const struct sr_capture *capture, const uint8_t **data, int *size)
{
	*data = NULL;
	*size = 0;
	if (capture->gpu_encoder)
		sr_gpu_encoder_get_extradata(capture->gpu_encoder, data, size);
	else if (capture->cpu_encoder)
		sr_encoder_get_extradata(capture->cpu_encoder, data, size);
}

static void publish_status(struct sr_capture *capture, uint64_t now, bool force)
{
	if (!capture)
		return;
	if (!force && capture->last_status_publish && now - capture->last_status_publish < 500000000ULL)
		return;

	const uint64_t video_now = obs_get_video_frame_time();
	struct sr_capture_recording_summary status = {0};
	status.camera_count = 1;
	status.requested_count = capture->disk_recording ? 1 : 0;
	status.active_count = capture->writer ? 1 : 0;
	status.failed_count = (capture->writer_failed || capture->encoder_failed) ? 1 : 0;
	status.recording_start_ns = capture->disk_recording ? capture->recording_obs_start_ns : 0;
	if (capture->disk_recording && capture->recording_obs_start_ns && video_now >= capture->recording_obs_start_ns)
		status.recording_duration_ns = video_now - capture->recording_obs_start_ns;

	struct sr_capture_performance_entry performance = {0};
	performance.path = capture->encoder_failed ? SR_CAPTURE_PERF_ERROR
			   : capture->gpu_encoder ? SR_CAPTURE_PERF_GPU_D3D11
			   : capture->cpu_encoder ? SR_CAPTURE_PERF_CPU
			   : SR_CAPTURE_PERF_WAITING;
	performance.gpu_fallback_reason = capture->gpu_fallback_reason;
	performance.width = capture->enc_width;
	performance.height = capture->enc_height;
	performance.gop_ms = capture->gop_ms;
	performance.qp = capture->qp;
	performance.disk_requested = capture->disk_recording;
	performance.writer_active = capture->writer != NULL;
	performance.writer_failed = capture->writer_failed;
	performance.encoder_failed = capture->encoder_failed;
	performance.ram_bytes = 0;
	performance.encode_calls = capture->encode_calls;
	performance.encode_time_ns_total = capture->encode_time_ns_total;
	performance.encode_time_ns_last = capture->encode_time_ns_last;

	struct obs_video_info video = {0};
	if (obs_get_video_info(&video)) {
		performance.fps_num = video.fps_num;
		performance.fps_den = video.fps_den;
	}

	const char *name = encoder_name(capture);
	if (name)
		strncpy(performance.encoder_name, name, sizeof(performance.encoder_name) - 1);

	if (capture->writer) {
		struct sr_segment_writer_stats writer_stats = {0};
		sr_segment_writer_get_stats(capture->writer, &writer_stats);
		status.reserve_blocked_count = writer_stats.reserve_blocked ? 1 : 0;
		status.packets_written = writer_stats.packets_written;
		status.bytes_written = writer_stats.bytes_written;
		if (writer_stats.write_failed)
			status.failed_count = 1;

		performance.reserve_blocked = writer_stats.reserve_blocked;
		performance.writer_failed = performance.writer_failed || writer_stats.write_failed;
		performance.packets_written = writer_stats.packets_written;
		performance.bytes_written = writer_stats.bytes_written;
		performance.packets_dropped = writer_stats.packets_dropped;
		performance.segments_finalized = writer_stats.segments_finalized;
		performance.queue_depth = writer_stats.queue_depth;
		performance.queue_high_watermark = writer_stats.queue_high_watermark;
	}

	pthread_mutex_lock(&capture->status_mutex);
	capture->status = status;
	capture->performance_status = performance;
	pthread_mutex_unlock(&capture->status_mutex);
	capture->last_status_publish = now;
}

static void publish_recording_intent(struct sr_capture *capture)
{
	pthread_mutex_lock(&capture->status_mutex);
	capture->status.camera_count = 1;
	capture->status.requested_count = capture->disk_recording ? 1 : 0;
	capture->status.recording_start_ns = capture->disk_recording ? capture->recording_obs_start_ns : 0;
	if (!capture->disk_recording)
		capture->status.recording_duration_ns = 0;
	capture->performance_status.disk_requested = capture->disk_recording;
	if (!capture->disk_recording) {
		capture->status.failed_count = 0;
		capture->status.reserve_blocked_count = 0;
	}
	pthread_mutex_unlock(&capture->status_mutex);
}

static void destroy_writer(struct sr_capture *capture)
{
	pthread_mutex_lock(&capture->camera_audio_mutex);
	struct sr_camera_audio_writer *audio = capture->camera_audio_writer;
	capture->camera_audio_writer = NULL;
	pthread_mutex_unlock(&capture->camera_audio_mutex);
	sr_camera_audio_writer_destroy(audio);

	if (capture->writer) {
		sr_segment_writer_destroy(capture->writer);
		capture->writer = NULL;
	}
	if (capture->master_audio_acquired) {
		sr_master_audio_release();
		capture->master_audio_acquired = false;
	}
	publish_status(capture, obs_get_video_frame_time(), true);
}

static void apply_recording_intent(struct sr_capture *capture)
{
	if (capture->restart_writer) {
		destroy_writer(capture);
		capture->restart_writer = false;
	}
	if (!capture->disk_recording && capture->writer)
		destroy_writer(capture);
	if (!capture->disk_recording)
		set_showing_hold(capture, false);
}

static bool ensure_writer(struct sr_capture *capture, const struct obs_video_info *video)
{
	if (!capture->disk_recording || capture->writer || capture->writer_failed || !encoder_ready(capture))
		return capture->writer != NULL;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		capture->writer_failed = true;
		return false;
	}

	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	encoder_extradata(capture, &extradata, &extradata_size);

	obs_source_t *camera = parent_ref(capture);
	char camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!camera || !sr_camera_key_from_source(camera, camera_key, sizeof(camera_key))) {
		blog(LOG_ERROR, "Pitel Instant Replay: cannot resolve persistent identity for camera '%s'", camera_name(capture));
		if (camera)
			obs_source_release(camera);
		bfree(session_dir);
		capture->writer_failed = true;
		return false;
	}
	obs_source_release(camera);

	int64_t sync_offset_ns = 0;
	(void)sr_camera_sync_offset_ns(camera_name(capture), &sync_offset_ns);

	struct sr_segment_writer_config config = {0};
	config.session_dir = session_dir;
	config.camera_name = camera_name(capture);
	config.camera_key = camera_key;
	config.sync_offset_ns = sync_offset_ns;
	config.codec_id = encoder_codec_id(capture);
	config.width = capture->enc_width;
	config.height = capture->enc_height;
	config.fps_num = video->fps_num;
	config.fps_den = video->fps_den;
	config.extradata = extradata;
	config.extradata_size = extradata_size;
	config.target_segment_ms = sr_config_get_segment_duration_ms();
	config.min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY
					? 0
					: sr_config_get_min_free_bytes();
	config.max_queue_packets = 600;
	config.start_discontinuity = sr_session_recording_starts_with_discontinuity();

	capture->writer = sr_segment_writer_create(&config);
	if (!capture->writer) {
		capture->writer_failed = true;
		blog(LOG_ERROR, "Pitel Instant Replay: continuous recorder could not start for '%s'", camera_name(capture));
		bfree(session_dir);
		return false;
	}

	if (!capture->master_audio_acquired) {
		if (sr_master_audio_acquire())
			capture->master_audio_acquired = true;
		else
			blog(LOG_WARNING, "Pitel Instant Replay: master replay audio could not start for '%s'", camera_name(capture));
	}

	struct obs_audio_info audio_info = {0};
	if (obs_get_audio_info(&audio_info)) {
		pthread_mutex_lock(&capture->camera_audio_mutex);
		capture->audio_sample_rate = audio_info.samples_per_sec;
		capture->audio_channels = get_audio_channels(audio_info.speakers);
		if (!capture->camera_audio_writer)
			capture->camera_audio_writer = sr_camera_audio_writer_create(session_dir, camera_name(capture),
										 audio_info.samples_per_sec);
		pthread_mutex_unlock(&capture->camera_audio_mutex);
	}

	bfree(session_dir);
	publish_status(capture, obs_get_video_frame_time(), true);
	return true;
}

static void log_writer_stats(struct sr_capture *capture, uint64_t now)
{
	if (!capture->writer)
		return;
	if (capture->last_stats_log && now - capture->last_stats_log < 60000000000ULL)
		return;
	capture->last_stats_log = now;

	struct sr_segment_writer_stats stats = {0};
	sr_segment_writer_get_stats(capture->writer, &stats);
	blog(LOG_INFO,
	     "Pitel Instant Replay: '%s' disk packets=%llu bytes=%.1fMB segments=%llu queue=%zu peak=%zu dropped=%llu%s%s",
	     camera_name(capture), (unsigned long long)stats.packets_written,
	     (double)stats.bytes_written / (1024.0 * 1024.0), (unsigned long long)stats.segments_finalized,
	     stats.queue_depth, stats.queue_high_watermark, (unsigned long long)stats.packets_dropped,
	     stats.reserve_blocked ? " reserve-blocked" : "", stats.write_failed ? " write-error" : "");
}

static void reset_encoders_locked(struct sr_capture *capture)
{
	destroy_writer(capture);
	sr_encoder_destroy(capture->cpu_encoder);
	capture->cpu_encoder = NULL;
	if (capture->gpu_encoder)
		capture->gpu_reset = true;
	capture->reset_encoder = false;
	capture->encoder_failed = false;
	capture->writer_failed = false;
	capture->gpu_failed = false;
	capture->gpu_fallback_logged = false;
	capture->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
	reset_encode_metrics(capture);
}

static void capture_update(void *data, obs_data_t *settings)
{
	struct sr_capture *capture = data;
	pthread_mutex_lock(&capture->encode_mutex);

	enum sr_encoder_backend backend = (enum sr_encoder_backend)obs_data_get_int(settings, S_ENCODER);
	int qp = (int)obs_data_get_int(settings, S_QUALITY);
	uint32_t gop_ms = (uint32_t)obs_data_get_int(settings, S_GOP);
	if (gop_ms != SR_GOP_ALL_I && gop_ms != SR_GOP_250MS && gop_ms != SR_GOP_500MS && gop_ms != SR_GOP_1000MS)
		gop_ms = SR_GOP_500MS;

	if (backend != capture->backend || qp != capture->qp || gop_ms != capture->gop_ms) {
		capture->backend = backend;
		capture->qp = qp;
		capture->gop_ms = gop_ms;
		capture->reset_encoder = true;
		capture->gpu_reset = true;
	}

	const bool disk_recording = obs_data_get_bool(settings, S_DISK_RECORDING);
	if (disk_recording != capture->disk_recording) {
		if (!disk_recording)
			capture->restart_writer = true;
		capture->disk_recording = disk_recording;
		capture->recording_obs_start_ns = disk_recording ? obs_get_video_frame_time() : 0;
		capture->writer_failed = false;
		if (disk_recording)
			set_showing_hold(capture, true);
	}

	pthread_mutex_unlock(&capture->encode_mutex);
	publish_recording_intent(capture);
}

static void *capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *capture = bzalloc(sizeof(*capture));
	capture->self = source;
	pthread_mutex_init(&capture->parent_mutex, NULL);
	pthread_mutex_init(&capture->encode_mutex, NULL);
	pthread_mutex_init(&capture->camera_audio_mutex, NULL);
	pthread_mutex_init(&capture->status_mutex, NULL);
	capture->backend = SR_ENC_AUTO;
	capture->qp = 23;
	capture->gop_ms = SR_GOP_500MS;

	/* Recording is an operator action. A scene collection may persist the
	 * previous filter value, but plugin startup must always be stopped. */
	obs_data_set_bool(settings, S_DISK_RECORDING, false);
	capture_update(capture, settings);
	obs_add_main_render_callback(capture_gpu_render, capture);
	return capture;
}

static void capture_destroy(void *data)
{
	struct sr_capture *capture = data;
	if (!capture)
		return;

	obs_remove_main_render_callback(capture_gpu_render, capture);
	set_showing_hold(capture, false);
	set_parent(capture, NULL);

	pthread_mutex_lock(&capture->encode_mutex);
	destroy_writer(capture);
	sr_encoder_destroy(capture->cpu_encoder);
	capture->cpu_encoder = NULL;
	struct sr_gpu_encoder *gpu = capture->gpu_encoder;
	capture->gpu_encoder = NULL;
	pthread_mutex_unlock(&capture->encode_mutex);

	sr_gpu_encoder_destroy(gpu);
	pthread_mutex_destroy(&capture->status_mutex);
	pthread_mutex_destroy(&capture->camera_audio_mutex);
	pthread_mutex_destroy(&capture->encode_mutex);
	pthread_mutex_destroy(&capture->parent_mutex);
	bfree(capture);
}

static void capture_gpu_render(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	struct sr_capture *capture = data;
	if (!capture)
		return;

	pthread_mutex_lock(&capture->encode_mutex);
	apply_recording_intent(capture);

	if (capture->gpu_reset) {
		destroy_writer(capture);
		sr_gpu_encoder_destroy(capture->gpu_encoder);
		capture->gpu_encoder = NULL;
		sr_encoder_destroy(capture->cpu_encoder);
		capture->cpu_encoder = NULL;
		capture->gpu_reset = false;
		capture->reset_encoder = false;
		capture->encoder_failed = false;
		capture->writer_failed = false;
		capture->gpu_failed = false;
		capture->gpu_fallback_logged = false;
		capture->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_NONE;
		reset_encode_metrics(capture);
	}

	if (!capture->disk_recording || !gpu_backend_candidate(capture->backend) || capture->gpu_failed ||
	    !capture->enc_width || !capture->enc_height) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return;
	}

	struct obs_video_info video = {0};
	if (!obs_get_video_info(&video)) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return;
	}

	if (!capture->gpu_encoder) {
		capture->gpu_encoder = sr_gpu_encoder_create(capture->enc_width, capture->enc_height, video.fps_num,
							     video.fps_den, capture->backend, capture->qp, capture->gop_ms);
		if (!capture->gpu_encoder) {
			capture->gpu_failed = true;
			capture->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_CREATE_FAILED;
			capture->gpu_fallback_logged = true;
			blog(LOG_INFO, "Pitel Instant Replay: '%s' GPU encoder unavailable; using CPU-frame fallback",
			     camera_name(capture));
			publish_status(capture, obs_get_video_frame_time(), true);
			pthread_mutex_unlock(&capture->encode_mutex);
			return;
		}
	}

	obs_source_t *camera = parent_ref(capture);
	if (!camera) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return;
	}

	AVPacket *packet = NULL;
	const uint64_t started = os_gettime_ns();
	const bool ok = sr_gpu_encoder_render_encode(capture->gpu_encoder, camera, &packet);
	obs_source_release(camera);
	const uint64_t elapsed = os_gettime_ns() - started;
	capture->encode_calls++;
	capture->encode_time_ns_total += elapsed;
	capture->encode_time_ns_last = elapsed;

	if (!ok) {
		blog(LOG_WARNING, "Pitel Instant Replay: '%s' GPU encoder failed; switching to CPU-frame fallback",
		     camera_name(capture));
		destroy_writer(capture);
		sr_gpu_encoder_destroy(capture->gpu_encoder);
		capture->gpu_encoder = NULL;
		capture->gpu_failed = true;
		capture->gpu_fallback_reason = SR_CAPTURE_GPU_FALLBACK_RUNTIME_FAILED;
		capture->writer_failed = false;
		reset_encode_metrics(capture);
		publish_status(capture, obs_get_video_frame_time(), true);
		pthread_mutex_unlock(&capture->encode_mutex);
		return;
	}

	if (packet) {
		const uint64_t timestamp = obs_get_video_frame_time();
		ensure_writer(capture, &video);
		if (capture->writer)
			sr_segment_writer_push_video(capture->writer, packet, timestamp);
		av_packet_free(&packet);
	}

	log_writer_stats(capture, obs_get_video_frame_time());
	publish_status(capture, obs_get_video_frame_time(), false);
	pthread_mutex_unlock(&capture->encode_mutex);
}

static struct obs_source_frame *capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *capture = data;
	if (!capture)
		return frame;

	ensure_parent_from_filter_callback(capture);
	pthread_mutex_lock(&capture->encode_mutex);
	apply_recording_intent(capture);

	if (!frame || !frame->data[0]) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return frame;
	}

	const bool size_changed = capture->enc_width && capture->enc_height &&
				  (capture->enc_width != frame->width || capture->enc_height != frame->height);
	if (capture->reset_encoder || size_changed)
		reset_encoders_locked(capture);

	capture->enc_width = frame->width;
	capture->enc_height = frame->height;

	if (!capture->disk_recording) {
		publish_status(capture, frame->timestamp, false);
		pthread_mutex_unlock(&capture->encode_mutex);
		return frame;
	}

	struct obs_video_info video = {0};
	if (!obs_get_video_info(&video)) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return frame;
	}

	/* The render callback owns video while a GPU backend is active or still
	 * eligible. CPU encoding begins only after GPU setup/runtime failure. */
	if (capture->gpu_encoder || capture->gpu_reset || (gpu_backend_candidate(capture->backend) && !capture->gpu_failed)) {
		publish_status(capture, frame->timestamp, false);
		pthread_mutex_unlock(&capture->encode_mutex);
		return frame;
	}

	if (capture->encoder_failed) {
		pthread_mutex_unlock(&capture->encode_mutex);
		return frame;
	}

	if (!capture->cpu_encoder) {
		capture->cpu_encoder = sr_encoder_create(frame->width, frame->height, video.fps_num, video.fps_den,
							 capture->backend, capture->qp, capture->gop_ms);
		if (!capture->cpu_encoder) {
			capture->encoder_failed = true;
			blog(LOG_ERROR, "Pitel Instant Replay: no H.264 encoder available for '%s'", camera_name(capture));
			publish_status(capture, frame->timestamp, true);
			pthread_mutex_unlock(&capture->encode_mutex);
			return frame;
		}
	}

	const uint64_t started = os_gettime_ns();
	AVPacket *packet = sr_encoder_encode(capture->cpu_encoder, frame);
	const uint64_t elapsed = os_gettime_ns() - started;
	capture->encode_calls++;
	capture->encode_time_ns_total += elapsed;
	capture->encode_time_ns_last = elapsed;

	if (packet) {
		const uint64_t timestamp = obs_get_video_frame_time();
		ensure_writer(capture, &video);
		if (capture->writer)
			sr_segment_writer_push_video(capture->writer, packet, timestamp ? timestamp : frame->timestamp);
		av_packet_free(&packet);
	}

	log_writer_stats(capture, frame->timestamp);
	publish_status(capture, frame->timestamp, false);
	pthread_mutex_unlock(&capture->encode_mutex);
	return frame;
}

static struct obs_audio_data *capture_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct sr_capture *capture = data;
	if (!capture || !audio)
		return audio;

	pthread_mutex_lock(&capture->camera_audio_mutex);
	if (!capture->audio_channels) {
		struct obs_audio_info info = {0};
		if (obs_get_audio_info(&info)) {
			capture->audio_sample_rate = info.samples_per_sec;
			capture->audio_channels = get_audio_channels(info.speakers);
		}
	}
	if (capture->camera_audio_writer && capture->audio_channels)
		sr_camera_audio_writer_push(capture->camera_audio_writer, audio, capture->audio_channels,
					    obs_get_video_frame_time());
	pthread_mutex_unlock(&capture->camera_audio_mutex);
	return audio;
}

struct capture_control_context {
	bool update_setting;
	bool enabled;
	struct sr_capture_recording_summary *summary;
	struct sr_capture_performance_snapshot *performance;
	size_t camera_count;
};

static void control_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct capture_control_context *context = param;
	if (!context || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;

	const bool enabled_filter = obs_source_enabled(child);
	if (context->update_setting && context->enabled && !enabled_filter)
		return;
	if (enabled_filter)
		context->camera_count++;

	if (context->update_setting) {
		obs_data_t *settings = obs_source_get_settings(child);
		/* Force an edge even if OBS restored a stale true value. */
		if (context->enabled && obs_data_get_bool(settings, S_DISK_RECORDING)) {
			obs_data_set_bool(settings, S_DISK_RECORDING, false);
			obs_source_update(child, settings);
		}
		obs_data_set_bool(settings, S_DISK_RECORDING, context->enabled);
		obs_source_update(child, settings);
		obs_data_release(settings);
	}

	if (!enabled_filter || (!context->summary && !context->performance))
		return;

	struct sr_capture *capture = obs_obj_get_data(child);
	if (!capture)
		return;

	struct sr_capture_recording_summary status = {0};
	struct sr_capture_performance_entry performance = {0};
	pthread_mutex_lock(&capture->status_mutex);
	status = capture->status;
	performance = capture->performance_status;
	pthread_mutex_unlock(&capture->status_mutex);

	if (context->summary) {
		context->summary->camera_count += status.camera_count;
		context->summary->requested_count += status.requested_count;
		context->summary->active_count += status.active_count;
		context->summary->failed_count += status.failed_count;
		context->summary->reserve_blocked_count += status.reserve_blocked_count;
		context->summary->packets_written += status.packets_written;
		context->summary->bytes_written += status.bytes_written;
		if (status.recording_start_ns && (!context->summary->recording_start_ns ||
						 status.recording_start_ns < context->summary->recording_start_ns))
			context->summary->recording_start_ns = status.recording_start_ns;
		if (status.recording_duration_ns > context->summary->recording_duration_ns)
			context->summary->recording_duration_ns = status.recording_duration_ns;
	}

	if (context->performance) {
		const char *name = camera_name(capture);
		if (name)
			strncpy(performance.camera_name, name, sizeof(performance.camera_name) - 1);
		const size_t next_count = context->performance->count + 1;
		struct sr_capture_performance_entry *entries =
			brealloc(context->performance->entries, next_count * sizeof(*entries));
		if (entries) {
			context->performance->entries = entries;
			context->performance->entries[context->performance->count] = performance;
			context->performance->count = next_count;
		}
	}
}

static bool control_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, control_filter, param);
	return true;
}

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)
{
	struct capture_control_context context = {.update_setting = true, .enabled = enabled};
	obs_enum_sources(control_source, &context);

	if (sr_program_recorder_selected()) {
		context.camera_count++;
		sr_program_recorder_set_recording(enabled);
	}
	if (camera_count)
		*camera_count = context.camera_count;
	return true;
}

bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary)
{
	if (!summary)
		return false;
	memset(summary, 0, sizeof(*summary));
	struct capture_control_context context = {.summary = summary};
	obs_enum_sources(control_source, &context);
	sr_program_recorder_add_recording_summary(summary);
	return true;
}

bool sr_capture_get_performance_snapshot(struct sr_capture_performance_snapshot *snapshot)
{
	if (!snapshot)
		return false;
	memset(snapshot, 0, sizeof(*snapshot));
	struct capture_control_context context = {.performance = snapshot};
	obs_enum_sources(control_source, &context);

	struct sr_capture_performance_entry program = {0};
	if (sr_program_recorder_get_performance_entry(&program)) {
		const size_t next_count = snapshot->count + 1;
		struct sr_capture_performance_entry *entries = brealloc(snapshot->entries, next_count * sizeof(*entries));
		if (entries) {
			snapshot->entries = entries;
			snapshot->entries[snapshot->count] = program;
			snapshot->count = next_count;
		}
	}
	return true;
}

void sr_capture_free_performance_snapshot(struct sr_capture_performance_snapshot *snapshot)
{
	if (!snapshot)
		return;
	bfree(snapshot->entries);
	memset(snapshot, 0, sizeof(*snapshot));
}

static obs_properties_t *capture_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *properties = obs_properties_create();

	obs_property_t *property = obs_properties_add_list(properties, S_ENCODER, obs_module_text("Encoder"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(property, obs_module_text("Encoder.Auto"), SR_ENC_AUTO);
	obs_property_list_add_int(property, obs_module_text("Encoder.NVENC"), SR_ENC_NVENC);
	obs_property_list_add_int(property, obs_module_text("Encoder.AMF"), SR_ENC_AMF);
	obs_property_list_add_int(property, obs_module_text("Encoder.QSV"), SR_ENC_QSV);
	obs_property_list_add_int(property, obs_module_text("Encoder.X264"), SR_ENC_X264);

	property = obs_properties_add_list(properties, S_QUALITY, obs_module_text("Quality"), OBS_COMBO_TYPE_LIST,
					   OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(property, obs_module_text("Quality.High"), 18);
	obs_property_list_add_int(property, obs_module_text("Quality.Medium"), 23);
	obs_property_list_add_int(property, obs_module_text("Quality.Low"), 28);

	property = obs_properties_add_list(properties, S_GOP, obs_module_text("GOP"), OBS_COMBO_TYPE_LIST,
					   OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(property, obs_module_text("GOP.AllI"), SR_GOP_ALL_I);
	obs_property_list_add_int(property, obs_module_text("GOP.Ultra"), SR_GOP_250MS);
	obs_property_list_add_int(property, obs_module_text("GOP.Balanced"), SR_GOP_500MS);
	obs_property_list_add_int(property, obs_module_text("GOP.Economy"), SR_GOP_1000MS);
	obs_property_set_long_description(property, obs_module_text("GOP.Description"));

	property = obs_properties_add_bool(properties, S_DISK_RECORDING, obs_module_text("DiskRecording"));
	obs_property_set_long_description(property, obs_module_text("DiskRecording.Description"));

	property = obs_properties_add_int(properties, S_SYNC_OFFSET_MS, obs_module_text("SyncOffset"),
					 SR_CAMERA_SYNC_MAX_MS * -1, SR_CAMERA_SYNC_MAX_MS, 1);
	obs_property_int_set_suffix(property, " ms");
	obs_property_set_long_description(property, obs_module_text("SyncOffset.Description"));
	return properties;
}

static void capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_ENCODER, SR_ENC_AUTO);
	obs_data_set_default_int(settings, S_QUALITY, 23);
	obs_data_set_default_int(settings, S_GOP, SR_GOP_500MS);
	obs_data_set_default_int(settings, S_SYNC_OFFSET_MS, 0);
	obs_data_set_default_bool(settings, S_DISK_RECORDING, false);
}

struct obs_source_info sr_capture_info = {
	.id = SR_CAPTURE_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = capture_name,
	.create = capture_create,
	.destroy = capture_destroy,
	.update = capture_update,
	.get_defaults = capture_defaults,
	.get_properties = capture_properties,
	.filter_add = filter_add,
	.filter_remove = filter_remove,
	.filter_video = capture_filter_video,
	.filter_audio = capture_filter_audio,
};
