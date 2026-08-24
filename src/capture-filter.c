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

#include "sr-buffer.h"
#include "sr-camera-identity.h"
#include "sr-codec.h"
#include "sr-capture.h"
#include "sr-credit.h"
#include "sr-config.h"
#include "sr-master-audio.h"
#include "sr-session.h"
#include "sr-segment-writer.h"

#define S_DURATION "duration_ms"
#define S_ENCODER "encoder"
#define S_QUALITY "quality"
#define S_GOP "gop_ms"
#define S_DISK_RECORDING "disk_recording"

struct sr_capture {
	obs_source_t *self;
	struct sr_buffer buffer;
	struct sr_encoder *encoder;
	struct sr_segment_writer *writer;

	enum sr_encoder_backend backend;
	int qp;
	uint32_t gop_ms;
	bool disk_recording;
	bool writer_failed;
	bool master_audio_acquired;

	/* format the current encoder was opened with */
	uint32_t enc_width;
	uint32_t enc_height;
	bool encoder_failed;
	bool reset_encoder;

	uint64_t last_stats_log;
};

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
	if (c->writer) {
		sr_segment_writer_destroy(c->writer);
		c->writer = NULL;
	}
	if (c->master_audio_acquired) {
		sr_master_audio_release();
		c->master_audio_acquired = false;
	}
}

static void sr_capture_update(void *data, obs_data_t *settings)
{
	struct sr_capture *c = data;

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
		c->encoder_failed = false;
		c->writer_failed = false;
	}

	if (disk_recording != c->disk_recording) {
		/* Only change intent here. The writer itself is created/destroyed in
		 * filter_video so it cannot be freed by the UI update callback while
		 * the video callback is queueing an encoded packet. */
		c->disk_recording = disk_recording;
		c->writer_failed = false;
	}
}

static void *sr_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_capture *c = bzalloc(sizeof(struct sr_capture));
	c->self = source;
	sr_buffer_init(&c->buffer);
	c->backend = SR_ENC_AUTO;
	c->qp = 23;
	c->gop_ms = SR_GOP_500MS;
	sr_capture_update(c, settings);
	return c;
}

static void sr_capture_destroy(void *data)
{
	struct sr_capture *c = data;
	destroy_writer(c);
	sr_encoder_destroy(c->encoder);
	sr_buffer_free(&c->buffer);
	bfree(c);
}

static obs_source_t *capture_camera_source(struct sr_capture *c)
{
	return c ? obs_filter_get_parent(c->self) : NULL;
}

static const char *capture_camera_name(struct sr_capture *c)
{
	obs_source_t *parent = capture_camera_source(c);
	return parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);
}

static bool ensure_writer(struct sr_capture *c, const struct obs_video_info *ovi)
{
	if (!c->disk_recording || c->writer || c->writer_failed || !c->encoder)
		return c->writer != NULL;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		c->writer_failed = true;
		return false;
	}

	const uint8_t *extradata = NULL;
	int extradata_size = 0;
	sr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);

	obs_source_t *camera_source = capture_camera_source(c);
	char camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!camera_source || !sr_camera_key_from_source(camera_source, camera_key, sizeof(camera_key))) {
		obs_log(LOG_ERROR, "'%s': could not resolve persistent OBS UUID for replay camera '%s'",
			obs_source_get_name(c->self), capture_camera_name(c));
		bfree(session_dir);
		c->writer_failed = true;
		return false;
	}

	struct sr_segment_writer_config cfg = {
		.session_dir = session_dir,
		.camera_name = capture_camera_name(c),
		.camera_key = camera_key,
		.codec_id = sr_encoder_codec_id(c->encoder),
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
	bfree(session_dir);
	if (!c->writer) {
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

static struct obs_source_frame *sr_capture_filter_video(void *data, struct obs_source_frame *frame)
{
	struct sr_capture *c = data;

	if (!frame || !frame->data[0] || c->encoder_failed)
		return frame;

	/* Apply a recording toggle on the video callback rather than the UI
	 * settings callback; see sr_capture_update. */
	if (!c->disk_recording && c->writer)
		destroy_writer(c);

	if (c->encoder && (c->reset_encoder || frame->width != c->enc_width || frame->height != c->enc_height)) {
		destroy_writer(c);
		sr_encoder_destroy(c->encoder);
		c->encoder = NULL;
		c->writer_failed = false;
		sr_buffer_clear(&c->buffer);
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return frame;

	if (!c->encoder) {
		c->encoder = sr_encoder_create(frame->width, frame->height, ovi.fps_num, ovi.fps_den, c->backend, c->qp,
					       c->gop_ms);
		if (!c->encoder) {
			obs_log(LOG_ERROR, "'%s': no H.264 encoder available, replay capture disabled",
				obs_source_get_name(c->self));
			c->encoder_failed = true;
			return frame;
		}
		c->reset_encoder = false;
		c->enc_width = frame->width;
		c->enc_height = frame->height;

		pthread_mutex_lock(&c->buffer.mutex);
		c->buffer.codec_id = sr_encoder_codec_id(c->encoder);
		c->buffer.width = frame->width;
		c->buffer.height = frame->height;
		pthread_mutex_unlock(&c->buffer.mutex);

		const uint8_t *extradata = NULL;
		int extradata_size = 0;
		sr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);
		sr_buffer_set_extradata(&c->buffer, extradata, extradata_size);
	}

	AVPacket *pkt = sr_encoder_encode(c->encoder, frame);
	if (pkt) {
		/* Delay session/writer creation until the encoder has actually emitted
		 * a packet. Some codec implementations only finalize stream headers
		 * after encoding begins. */
		ensure_writer(c, &ovi);

		/* The disk writer clones the packet. The original remains owned by
		 * the legacy RAM ring buffer, so current replay behavior stays intact
		 * while the new continuous storage engine is developed. */
		if (c->writer)
			sr_segment_writer_push_video(c->writer, pkt, frame->timestamp);
		sr_buffer_push_video(&c->buffer, pkt, frame->timestamp);
	}

	log_buffer_stats(c, frame->timestamp);
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

	return audio;
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
	.filter_video = sr_capture_filter_video,
	.filter_audio = sr_capture_filter_audio,
};
