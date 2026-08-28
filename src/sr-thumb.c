/*
 * Pitel Instant Replay - thumbnail service
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include "sr-thumb.h"
#include "sr-disk-player.h"

#include <obs-module.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

static bool frame_to_rgba(const AVFrame *source, int width, int height, uint8_t **rgba)
{
	if (!source || !rgba || width <= 0 || height <= 0)
		return false;

	struct SwsContext *converter = sws_getContext(source->width, source->height, source->format, width, height,
						      AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
	if (!converter)
		return false;

	uint8_t *pixels = bmalloc((size_t)width * (size_t)height * 4u);
	if (!pixels) {
		sws_freeContext(converter);
		return false;
	}

	uint8_t *destinations[4] = {pixels, NULL, NULL, NULL};
	int strides[4] = {width * 4, 0, 0, 0};
	const int converted = sws_scale(converter, (const uint8_t *const *)source->data, source->linesize, 0,
					source->height, destinations, strides);
	sws_freeContext(converter);
	if (converted <= 0) {
		bfree(pixels);
		return false;
	}

	*rgba = pixels;
	return true;
}

static AVFrame *software_frame_from(const AVFrame *source)
{
	if (!source)
		return NULL;

	AVFrame *copy = av_frame_alloc();
	if (!copy)
		return NULL;

	if (source->hw_frames_ctx) {
		if (av_hwframe_transfer_data(copy, source, 0) < 0) {
			av_frame_free(&copy);
			return NULL;
		}
		av_frame_copy_props(copy, source);
		return copy;
	}

	if (av_frame_ref(copy, source) < 0) {
		av_frame_free(&copy);
		return NULL;
	}
	return copy;
}

bool sr_thumbnail_rgba(const char *path, int width, int height, uint8_t **rgba)
{
	if (!rgba)
		return false;
	*rgba = NULL;
	if (!path || !*path || width <= 0 || height <= 0)
		return false;

	AVFormatContext *format = NULL;
	if (avformat_open_input(&format, path, NULL, NULL) < 0)
		return false;
	if (avformat_find_stream_info(format, NULL) < 0) {
		avformat_close_input(&format);
		return false;
	}

	const int stream_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (stream_index < 0) {
		avformat_close_input(&format);
		return false;
	}

	AVStream *stream = format->streams[stream_index];
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!decoder || avcodec_parameters_to_context(decoder, stream->codecpar) < 0 ||
	    avcodec_open2(decoder, codec, NULL) < 0) {
		avcodec_free_context(&decoder);
		avformat_close_input(&format);
		return false;
	}

	AVPacket *packet = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool decoded = false;
	if (packet && frame) {
		while (!decoded && av_read_frame(format, packet) >= 0) {
			if (packet->stream_index == stream_index && avcodec_send_packet(decoder, packet) >= 0 &&
			    avcodec_receive_frame(decoder, frame) >= 0)
				decoded = true;
			av_packet_unref(packet);
		}
		if (!decoded && avcodec_send_packet(decoder, NULL) >= 0 && avcodec_receive_frame(decoder, frame) >= 0)
			decoded = true;
	}

	bool success = false;
	if (decoded) {
		AVFrame *software = software_frame_from(frame);
		if (software) {
			success = frame_to_rgba(software, width, height, rgba);
			av_frame_free(&software);
		}
	}

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&format);
	return success;
}

bool sr_disk_thumbnail_rgba(const char *session_dir, const char *camera_name, uint64_t timestamp_ns, int width,
			    int height, uint8_t **rgba)
{
	if (!rgba)
		return false;
	*rgba = NULL;
	if (!session_dir || !*session_dir || !camera_name || !*camera_name || width <= 0 || height <= 0)
		return false;

	struct sr_disk_player *player = sr_disk_player_create(session_dir, camera_name);
	if (!player)
		return false;

	AVFrame *frame = NULL;
	bool decoded = sr_disk_player_decode_at(player, timestamp_ns, &frame, NULL);
	if (!decoded) {
		uint64_t first_ns = 0;
		if (sr_disk_player_get_bounds(player, &first_ns, NULL))
			decoded = sr_disk_player_decode_at(player, first_ns, &frame, NULL);
	}

	bool success = false;
	if (decoded && frame) {
		AVFrame *software = software_frame_from(frame);
		if (software) {
			success = frame_to_rgba(software, width, height, rgba);
			av_frame_free(&software);
		}
	}

	av_frame_free(&frame);
	sr_disk_player_destroy(player);
	return success;
}
