/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-export.h"

#include "sr-master-audio-catalog.h"
#include "sr-master-audio-reader.h"
#include "sr-segment-catalog.h"
#include "sr-segment-reader.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <libavformat/avformat.h>

#include <limits.h>
#include <string.h>

#define NS_PER_SECOND 1000000000ULL
#define NS_TIME_BASE \
	(AVRational) \
	{ \
		1, 1000000000 \
	}

struct video_cursor {
	struct sr_segment_descriptor *segments;
	size_t segment_count;
	size_t segment_index;
	struct sr_segment_reader *reader;
	size_t packet_index;
	uint64_t stop_ns;
	uint64_t last_timestamp_ns;
	bool have_last_timestamp;
	struct sr_segment_stream_info reference;
	uint8_t *reference_extradata;
};

struct audio_cursor {
	struct sr_master_audio_descriptor *segments;
	size_t segment_count;
	size_t segment_index;
	struct sr_master_audio_reader *reader;
	size_t packet_index;
	uint64_t stop_ns;
	uint64_t last_timestamp_ns;
	bool have_last_timestamp;
	struct sr_master_audio_segment_info reference;
	uint8_t *reference_extradata;
};

static bool add_signed_offset(uint64_t timestamp_ns, int64_t offset_ns, uint64_t *result)
{
	if (!result)
		return false;
	if (offset_ns >= 0) {
		const uint64_t magnitude = (uint64_t)offset_ns;
		if (timestamp_ns > UINT64_MAX - magnitude)
			return false;
		*result = timestamp_ns + magnitude;
		return true;
	}

	const uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;
	if (timestamp_ns < magnitude)
		return false;
	*result = timestamp_ns - magnitude;
	return true;
}

static int64_t relative_ns(uint64_t timestamp_ns, uint64_t origin_ns)
{
	if (timestamp_ns >= origin_ns) {
		const uint64_t delta = timestamp_ns - origin_ns;
		return delta > INT64_MAX ? INT64_MAX : (int64_t)delta;
	}
	const uint64_t delta = origin_ns - timestamp_ns;
	return delta > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)delta;
}

static bool camera_media_to_global(uint64_t media_timestamp_ns, int64_t sync_offset_ns, uint64_t *global_ns)
{
	if (sync_offset_ns >= 0) {
		const uint64_t magnitude = (uint64_t)sync_offset_ns;
		if (media_timestamp_ns < magnitude)
			return false;
		*global_ns = media_timestamp_ns - magnitude;
		return true;
	}

	const uint64_t magnitude = (uint64_t)(-(sync_offset_ns + 1)) + 1ULL;
	if (media_timestamp_ns > UINT64_MAX - magnitude)
		return false;
	*global_ns = media_timestamp_ns + magnitude;
	return true;
}

static bool cancelled(sr_event_export_cancel_cb should_cancel, void *data)
{
	return should_cancel && should_cancel(data);
}

static char *part_path(const char *path)
{
	struct dstr part = {0};
	dstr_copy(&part, path ? path : "");
	dstr_cat(&part, ".part");
	char *copy = bstrdup(part.array);
	dstr_free(&part);
	return copy;
}

static bool same_bytes(const uint8_t *a, int a_size, const uint8_t *b, int b_size)
{
	return a_size == b_size && (!a_size || (a && b && memcmp(a, b, (size_t)a_size) == 0));
}

static bool video_stream_matches(const struct video_cursor *cursor, const struct sr_segment_stream_info *info)
{
	return cursor->reference.codec_id == info->codec_id && cursor->reference.width == info->width &&
	       cursor->reference.height == info->height && cursor->reference.fps_num == info->fps_num &&
	       cursor->reference.fps_den == info->fps_den &&
	       same_bytes(cursor->reference_extradata, cursor->reference.extradata_size, info->extradata,
			  info->extradata_size);
}

static bool audio_stream_matches(const struct audio_cursor *cursor, const struct sr_master_audio_segment_info *info)
{
	return cursor->reference.codec_id == info->codec_id && cursor->reference.sample_rate == info->sample_rate &&
	       cursor->reference.channels == info->channels && cursor->reference.bit_rate == info->bit_rate &&
	       same_bytes(cursor->reference_extradata, cursor->reference.extradata_size, info->extradata,
			  info->extradata_size);
}

static bool video_open_segment(struct video_cursor *cursor, size_t index, bool reference)
{
	if (!cursor || index >= cursor->segment_count)
		return false;
	sr_segment_reader_close(cursor->reader);
	cursor->reader =
		sr_segment_reader_open(cursor->segments[index].segment_path, cursor->segments[index].index_path);
	if (!cursor->reader)
		return false;
	struct sr_segment_stream_info info;
	if (!sr_segment_reader_get_info(cursor->reader, &info))
		return false;

	if (reference) {
		cursor->reference = info;
		cursor->reference_extradata =
			info.extradata_size > 0 ? bmemdup(info.extradata, (size_t)info.extradata_size) : NULL;
		if (info.extradata_size > 0 && !cursor->reference_extradata)
			return false;
		cursor->reference.extradata = cursor->reference_extradata;
	} else if (!video_stream_matches(cursor, &info)) {
		return false;
	}

	cursor->segment_index = index;
	cursor->packet_index = 0;
	return true;
}

static bool video_cursor_init(struct video_cursor *cursor, const struct sr_event_export_spec *spec,
			      uint64_t media_in_ns, uint64_t media_out_ns)
{
	memset(cursor, 0, sizeof(*cursor));
	cursor->stop_ns = media_out_ns;
	if (!sr_segment_catalog_scan(spec->session_dir, spec->camera_name, &cursor->segments, &cursor->segment_count) ||
	    !cursor->segment_count)
		return false;

	const struct sr_segment_descriptor *start =
		sr_segment_catalog_find(cursor->segments, cursor->segment_count, media_in_ns);
	if (!start)
		return false;
	const size_t start_index = (size_t)(start - cursor->segments);
	if (!video_open_segment(cursor, start_index, true))
		return false;

	if (!sr_segment_reader_find_position(cursor->reader, media_in_ns, true, &cursor->packet_index, NULL))
		return false;
	return true;
}

static void video_cursor_free(struct video_cursor *cursor)
{
	if (!cursor)
		return;
	sr_segment_reader_close(cursor->reader);
	sr_segment_catalog_free(cursor->segments, cursor->segment_count);
	bfree(cursor->reference_extradata);
	memset(cursor, 0, sizeof(*cursor));
}

/* Returns 1 for a packet, 0 for end, -1 for an incompatible/corrupt segment. */
static int video_cursor_next(struct video_cursor *cursor, AVPacket **packet, uint64_t *timestamp_ns)
{
	*packet = NULL;
	for (;;) {
		const size_t count = sr_segment_reader_entry_count(cursor->reader);
		while (cursor->packet_index < count) {
			struct sr_index_entry entry;
			if (!sr_segment_reader_entry_at(cursor->reader, cursor->packet_index++, &entry))
				return -1;
			if (entry.timestamp_ns >= cursor->stop_ns)
				return 0;
			if (cursor->have_last_timestamp && entry.timestamp_ns <= cursor->last_timestamp_ns)
				continue;
			if (!sr_segment_reader_read_video_packet(cursor->reader, &entry, packet, timestamp_ns, NULL))
				return -1;
			cursor->last_timestamp_ns = *timestamp_ns;
			cursor->have_last_timestamp = true;
			return 1;
		}

		const size_t next = cursor->segment_index + 1;
		if (next >= cursor->segment_count || cursor->segments[next].start_ns >= cursor->stop_ns)
			return 0;
		if (!video_open_segment(cursor, next, false))
			return -1;
	}
}

static bool audio_open_segment(struct audio_cursor *cursor, size_t index, bool reference)
{
	if (!cursor || index >= cursor->segment_count)
		return false;
	sr_master_audio_reader_close(cursor->reader);
	cursor->reader =
		sr_master_audio_reader_open(cursor->segments[index].audio_path, cursor->segments[index].index_path);
	if (!cursor->reader)
		return false;
	struct sr_master_audio_segment_info info;
	if (!sr_master_audio_reader_get_info(cursor->reader, &info))
		return false;

	if (reference) {
		cursor->reference = info;
		cursor->reference_extradata =
			info.extradata_size > 0 ? bmemdup(info.extradata, (size_t)info.extradata_size) : NULL;
		if (info.extradata_size > 0 && !cursor->reference_extradata)
			return false;
		cursor->reference.extradata = cursor->reference_extradata;
	} else if (!audio_stream_matches(cursor, &info)) {
		return false;
	}

	cursor->segment_index = index;
	cursor->packet_index = 0;
	return true;
}

static bool audio_cursor_init(struct audio_cursor *cursor, const struct sr_event_export_spec *spec)
{
	memset(cursor, 0, sizeof(*cursor));
	cursor->stop_ns = spec->event_out_ns;
	if (!sr_master_audio_catalog_scan(spec->session_dir, &cursor->segments, &cursor->segment_count) ||
	    !cursor->segment_count)
		return false;

	const struct sr_master_audio_descriptor *start =
		sr_master_audio_catalog_find(cursor->segments, cursor->segment_count, spec->event_in_ns);
	if (!start)
		return false;
	const size_t start_index = (size_t)(start - cursor->segments);
	if (!audio_open_segment(cursor, start_index, true))
		return false;
	if (!sr_master_audio_reader_find_position(cursor->reader, spec->event_in_ns, &cursor->packet_index, NULL))
		return false;
	return true;
}

static void audio_cursor_free(struct audio_cursor *cursor)
{
	if (!cursor)
		return;
	sr_master_audio_reader_close(cursor->reader);
	sr_master_audio_catalog_free(cursor->segments, cursor->segment_count);
	bfree(cursor->reference_extradata);
	memset(cursor, 0, sizeof(*cursor));
}

static int audio_cursor_next(struct audio_cursor *cursor, AVPacket **packet, uint64_t *timestamp_ns)
{
	*packet = NULL;
	for (;;) {
		const size_t count = sr_master_audio_reader_entry_count(cursor->reader);
		while (cursor->packet_index < count) {
			struct sr_audio_index_entry entry;
			if (!sr_master_audio_reader_entry_at(cursor->reader, cursor->packet_index++, &entry))
				return -1;
			if (entry.timestamp_ns >= cursor->stop_ns)
				return 0;
			if (cursor->have_last_timestamp && entry.timestamp_ns <= cursor->last_timestamp_ns)
				continue;
			if (!sr_master_audio_reader_read_packet(cursor->reader, &entry, packet, timestamp_ns))
				return -1;
			cursor->last_timestamp_ns = *timestamp_ns;
			cursor->have_last_timestamp = true;
			return 1;
		}

		const size_t next = cursor->segment_index + 1;
		if (next >= cursor->segment_count || cursor->segments[next].start_ns >= cursor->stop_ns)
			return 0;
		if (!audio_open_segment(cursor, next, false))
			return -1;
	}
}

static bool copy_extradata(AVCodecParameters *parameters, const uint8_t *data, int size)
{
	if (size <= 0)
		return true;
	parameters->extradata = av_mallocz((size_t)size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!parameters->extradata)
		return false;
	memcpy(parameters->extradata, data, (size_t)size);
	parameters->extradata_size = size;
	return true;
}

static AVStream *create_video_stream(AVFormatContext *output, const struct video_cursor *cursor)
{
	AVStream *stream = avformat_new_stream(output, NULL);
	if (!stream)
		return NULL;
	stream->time_base = NS_TIME_BASE;
	AVCodecParameters *parameters = stream->codecpar;
	parameters->codec_type = AVMEDIA_TYPE_VIDEO;
	parameters->codec_id = cursor->reference.codec_id;
	parameters->codec_tag = 0;
	parameters->width = (int)cursor->reference.width;
	parameters->height = (int)cursor->reference.height;
	if (!copy_extradata(parameters, cursor->reference_extradata, cursor->reference.extradata_size))
		return NULL;
	return stream;
}

static AVStream *create_audio_stream(AVFormatContext *output, const struct audio_cursor *cursor)
{
	AVStream *stream = avformat_new_stream(output, NULL);
	if (!stream)
		return NULL;
	stream->time_base = NS_TIME_BASE;
	AVCodecParameters *parameters = stream->codecpar;
	parameters->codec_type = AVMEDIA_TYPE_AUDIO;
	parameters->codec_id = cursor->reference.codec_id;
	parameters->codec_tag = 0;
	parameters->sample_rate = (int)cursor->reference.sample_rate;
	parameters->bit_rate = cursor->reference.bit_rate;
	av_channel_layout_default(&parameters->ch_layout, (int)cursor->reference.channels);
	if (!copy_extradata(parameters, cursor->reference_extradata, cursor->reference.extradata_size))
		return NULL;
	return stream;
}

static int64_t video_duration_ns(const struct video_cursor *cursor)
{
	if (!cursor->reference.fps_num || !cursor->reference.fps_den)
		return 33333333;
	return (int64_t)(((uint64_t)cursor->reference.fps_den * NS_PER_SECOND) / cursor->reference.fps_num);
}

static int64_t audio_duration_ns(const struct audio_cursor *cursor, const AVPacket *packet)
{
	if (!cursor->reference.sample_rate || packet->duration <= 0)
		return 21333333;
	return av_rescale_q(packet->duration, (AVRational){1, (int)cursor->reference.sample_rate}, NS_TIME_BASE);
}

static bool write_packet(AVFormatContext *output, AVStream *stream, AVPacket *packet, int64_t relative_timestamp_ns,
			 int64_t duration_ns)
{
	packet->stream_index = stream->index;
	packet->pts = av_rescale_q(relative_timestamp_ns, NS_TIME_BASE, stream->time_base);
	packet->dts = packet->pts;
	packet->duration = av_rescale_q(duration_ns, NS_TIME_BASE, stream->time_base);
	packet->pos = -1;
	return av_interleaved_write_frame(output, packet) >= 0;
}

static void report_progress(const struct sr_event_export_spec *spec, uint64_t global_timestamp_ns,
			    sr_event_export_progress_cb progress, void *data, unsigned *last_percent)
{
	if (!progress || spec->event_out_ns <= spec->event_in_ns)
		return;
	uint64_t bounded = global_timestamp_ns;
	if (bounded < spec->event_in_ns)
		bounded = spec->event_in_ns;
	if (bounded > spec->event_out_ns)
		bounded = spec->event_out_ns;
	const uint64_t elapsed = bounded - spec->event_in_ns;
	const uint64_t duration = spec->event_out_ns - spec->event_in_ns;
	const unsigned percent = (unsigned)((long double)elapsed * 100.0L / (long double)duration);
	if (percent != *last_percent) {
		*last_percent = percent;
		progress(data, percent);
	}
}

bool sr_event_export_fast(const struct sr_event_export_spec *spec, sr_event_export_cancel_cb should_cancel,
			  sr_event_export_progress_cb progress, void *callback_data,
			  struct sr_event_export_result *result)
{
	struct sr_event_export_result local = {0};
	if (!result)
		result = &local;
	else
		memset(result, 0, sizeof(*result));
	result->error = SR_EVENT_EXPORT_INVALID_ARGUMENT;

	if (!spec || !spec->session_dir || !*spec->session_dir || !spec->camera_name || !*spec->camera_name ||
	    !spec->output_path || !*spec->output_path || spec->event_out_ns <= spec->event_in_ns)
		return false;
	if (os_file_exists(spec->output_path)) {
		result->error = SR_EVENT_EXPORT_DESTINATION_EXISTS;
		return false;
	}

	uint64_t media_in_ns = 0;
	uint64_t media_out_ns = 0;
	if (!add_signed_offset(spec->event_in_ns, spec->camera_sync_offset_ns, &media_in_ns) ||
	    !add_signed_offset(spec->event_out_ns, spec->camera_sync_offset_ns, &media_out_ns))
		return false;

	struct video_cursor video;
	if (!video_cursor_init(&video, spec, media_in_ns, media_out_ns)) {
		result->error = SR_EVENT_EXPORT_NO_VIDEO;
		video_cursor_free(&video);
		return false;
	}

	struct audio_cursor audio = {0};
	const bool have_audio = spec->include_master_audio && audio_cursor_init(&audio, spec);
	if (!have_audio) {
		audio_cursor_free(&audio);
		memset(&audio, 0, sizeof(audio));
	}

	char *temporary_path = part_path(spec->output_path);
	AVFormatContext *output = NULL;
	AVPacket *video_packet = NULL;
	AVPacket *audio_packet = NULL;
	uint64_t video_timestamp_ns = 0;
	uint64_t audio_timestamp_ns = 0;
	int video_state = 0;
	int audio_state = 0;
	bool header_written = false;
	bool ok = false;
	unsigned last_percent = UINT_MAX;

	if (!temporary_path || os_file_exists(temporary_path)) {
		result->error = SR_EVENT_EXPORT_DESTINATION_EXISTS;
		goto cleanup;
	}
	if (avformat_alloc_output_context2(&output, NULL, "mp4", temporary_path) < 0 || !output) {
		result->error = SR_EVENT_EXPORT_OPEN_FAILED;
		goto cleanup;
	}

	AVStream *video_stream = create_video_stream(output, &video);
	AVStream *audio_stream = have_audio ? create_audio_stream(output, &audio) : NULL;
	if (!video_stream || (have_audio && !audio_stream)) {
		result->error = SR_EVENT_EXPORT_OPEN_FAILED;
		goto cleanup;
	}

	if (!(output->oformat->flags & AVFMT_NOFILE) && avio_open(&output->pb, temporary_path, AVIO_FLAG_WRITE) < 0) {
		result->error = SR_EVENT_EXPORT_OPEN_FAILED;
		goto cleanup;
	}

	AVDictionary *options = NULL;
	av_dict_set(&options, "movflags", "+faststart", 0);
	av_dict_set(&options, "use_editlist", "1", 0);
	av_dict_set(&options, "avoid_negative_ts", "disabled", 0);
	if (avformat_write_header(output, &options) < 0) {
		av_dict_free(&options);
		result->error = SR_EVENT_EXPORT_WRITE_FAILED;
		goto cleanup;
	}
	av_dict_free(&options);
	header_written = true;

	video_state = video_cursor_next(&video, &video_packet, &video_timestamp_ns);
	audio_state = have_audio ? audio_cursor_next(&audio, &audio_packet, &audio_timestamp_ns) : 0;
	for (;;) {
		if (video_state < 0 || audio_state < 0) {
			result->error = SR_EVENT_EXPORT_UNSUPPORTED_CHANGE;
			goto cleanup;
		}
		if (video_state == 0 && audio_state == 0)
			break;
		if (cancelled(should_cancel, callback_data)) {
			result->error = SR_EVENT_EXPORT_CANCELLED;
			goto cleanup;
		}

		uint64_t video_global_ns = 0;
		if (video_state > 0 &&
		    !camera_media_to_global(video_timestamp_ns, spec->camera_sync_offset_ns, &video_global_ns)) {
			result->error = SR_EVENT_EXPORT_INVALID_ARGUMENT;
			goto cleanup;
		}
		const bool take_video = video_state > 0 && (audio_state <= 0 || video_global_ns <= audio_timestamp_ns);
		if (take_video) {
			int64_t duration_ns = video_duration_ns(&video);
			if (video_timestamp_ns >= media_in_ns && video_timestamp_ns < media_out_ns &&
			    (uint64_t)duration_ns > media_out_ns - video_timestamp_ns)
				duration_ns = (int64_t)(media_out_ns - video_timestamp_ns);
			if (!write_packet(output, video_stream, video_packet,
					  relative_ns(video_timestamp_ns, media_in_ns), duration_ns)) {
				result->error = SR_EVENT_EXPORT_WRITE_FAILED;
				goto cleanup;
			}
			av_packet_free(&video_packet);
			result->video_packets++;
			report_progress(spec, video_global_ns, progress, callback_data, &last_percent);
			video_state = video_cursor_next(&video, &video_packet, &video_timestamp_ns);
		} else {
			int64_t duration_ns = audio_duration_ns(&audio, audio_packet);
			if (audio_timestamp_ns >= spec->event_in_ns && audio_timestamp_ns < spec->event_out_ns &&
			    (uint64_t)duration_ns > spec->event_out_ns - audio_timestamp_ns)
				duration_ns = (int64_t)(spec->event_out_ns - audio_timestamp_ns);
			if (!write_packet(output, audio_stream, audio_packet,
					  relative_ns(audio_timestamp_ns, spec->event_in_ns), duration_ns)) {
				result->error = SR_EVENT_EXPORT_WRITE_FAILED;
				goto cleanup;
			}
			av_packet_free(&audio_packet);
			result->audio_packets++;
			report_progress(spec, audio_timestamp_ns, progress, callback_data, &last_percent);
			audio_state = audio_cursor_next(&audio, &audio_packet, &audio_timestamp_ns);
		}
	}

	if (!result->video_packets) {
		result->error = SR_EVENT_EXPORT_NO_VIDEO;
		goto cleanup;
	}
	if (av_write_trailer(output) < 0) {
		result->error = SR_EVENT_EXPORT_WRITE_FAILED;
		goto cleanup;
	}
	header_written = false;
	if (!(output->oformat->flags & AVFMT_NOFILE))
		avio_closep(&output->pb);
	if (os_rename(temporary_path, spec->output_path) != 0) {
		result->error = SR_EVENT_EXPORT_WRITE_FAILED;
		goto cleanup;
	}

	result->audio_included = result->audio_packets > 0;
	result->error = SR_EVENT_EXPORT_OK;
	if (progress)
		progress(callback_data, 100);
	blog(LOG_INFO, "Sports Replay: exported Event angle '%s' to '%s' (%zu video, %zu audio packets)",
	     spec->camera_name, spec->output_path, result->video_packets, result->audio_packets);
	ok = true;

cleanup:
	av_packet_free(&video_packet);
	av_packet_free(&audio_packet);
	if (output) {
		if (header_written)
			av_write_trailer(output);
		if (!(output->oformat->flags & AVFMT_NOFILE) && output->pb)
			avio_closep(&output->pb);
		avformat_free_context(output);
	}
	if (!ok && temporary_path)
		os_unlink(temporary_path);
	bfree(temporary_path);
	video_cursor_free(&video);
	audio_cursor_free(&audio);
	return ok;
}

const char *sr_event_export_error_text(enum sr_event_export_error error)
{
	switch (error) {
	case SR_EVENT_EXPORT_OK:
		return "ok";
	case SR_EVENT_EXPORT_INVALID_ARGUMENT:
		return "invalid argument or Event range";
	case SR_EVENT_EXPORT_DESTINATION_EXISTS:
		return "destination or temporary file already exists";
	case SR_EVENT_EXPORT_NO_VIDEO:
		return "no continuous video covers this Event";
	case SR_EVENT_EXPORT_UNSUPPORTED_CHANGE:
		return "codec, format or segment continuity changed inside the Event";
	case SR_EVENT_EXPORT_OPEN_FAILED:
		return "could not create the MP4 output";
	case SR_EVENT_EXPORT_WRITE_FAILED:
		return "MP4 write or atomic finalize failed";
	case SR_EVENT_EXPORT_CANCELLED:
		return "cancelled";
	default:
		return "unknown export error";
	}
}
