/*
 * Pitel Instant Replay - FFmpeg codec adapters
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include "sr-codec.h"
#include "sr-gpu-video.h"

#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

struct sr_encoder {
	AVCodecContext *codec_ctx;
	const AVCodec *codec;
	AVFrame *nv12;
	struct SwsContext *converter;
	enum AVPixelFormat converter_input;
	int64_t next_pts;
	bool warned_format;
	bool logged_direct_nv12;
};

struct sr_decoder {
	AVCodecContext *codec_ctx;
	AVFrame *output;
	bool used_hardware;
	bool download_hardware_frames;
};

static enum AVPixelFormat obs_frame_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return AV_PIX_FMT_YUV420P;
	case VIDEO_FORMAT_NV12:
		return AV_PIX_FMT_NV12;
	case VIDEO_FORMAT_YUY2:
		return AV_PIX_FMT_YUYV422;
	case VIDEO_FORMAT_UYVY:
		return AV_PIX_FMT_UYVY422;
	case VIDEO_FORMAT_YVYU:
		return AV_PIX_FMT_YVYU422;
	case VIDEO_FORMAT_RGBA:
		return AV_PIX_FMT_RGBA;
	case VIDEO_FORMAT_BGRA:
		return AV_PIX_FMT_BGRA;
	case VIDEO_FORMAT_BGRX:
		return AV_PIX_FMT_BGR0;
	case VIDEO_FORMAT_Y800:
		return AV_PIX_FMT_GRAY8;
	case VIDEO_FORMAT_I444:
		return AV_PIX_FMT_YUV444P;
	case VIDEO_FORMAT_I422:
		return AV_PIX_FMT_YUV422P;
	default:
		return AV_PIX_FMT_NONE;
	}
}

static int gop_frames(uint32_t fps_num, uint32_t fps_den, uint32_t interval_ms)
{
	if (!fps_num || !fps_den || interval_ms == SR_GOP_ALL_I)
		return 1;
	uint64_t frames = ((uint64_t)fps_num * interval_ms + ((uint64_t)fps_den * 1000ULL) / 2ULL) /
			  ((uint64_t)fps_den * 1000ULL);
	if (frames < 1)
		frames = 1;
	if (frames > INT_MAX)
		frames = INT_MAX;
	return (int)frames;
}

static void set_encoder_options(AVCodecContext *ctx, const char *codec_name, int qp)
{
	if (strcmp(codec_name, "h264_nvenc") == 0) {
		av_opt_set(ctx->priv_data, "rc", "constqp", 0);
		av_opt_set_int(ctx->priv_data, "qp", qp, 0);
		av_opt_set(ctx->priv_data, "preset", "p4", 0);
		av_opt_set(ctx->priv_data, "tune", "ull", 0);
		av_opt_set_int(ctx->priv_data, "delay", 0, 0);
		return;
	}
	if (strcmp(codec_name, "h264_amf") == 0) {
		av_opt_set(ctx->priv_data, "rc", "cqp", 0);
		av_opt_set_int(ctx->priv_data, "qp_i", qp, 0);
		av_opt_set_int(ctx->priv_data, "qp_p", qp, 0);
		av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
		return;
	}
	if (strcmp(codec_name, "h264_qsv") == 0) {
		ctx->global_quality = qp;
		av_opt_set(ctx->priv_data, "preset", "fast", 0);
		return;
	}
	if (strcmp(codec_name, "libx264") == 0) {
		char quality[16];
		snprintf(quality, sizeof(quality), "%d", qp);
		av_opt_set(ctx->priv_data, "crf", quality, 0);
		av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
		av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
	}
}

static bool try_encoder(struct sr_encoder *encoder, const char *codec_name, uint32_t width, uint32_t height,
			uint32_t fps_num, uint32_t fps_den, int qp, int keyint)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
	if (!codec)
		return false;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;

	ctx->width = (int)(width & ~1u);
	ctx->height = (int)(height & ~1u);
	ctx->pix_fmt = AV_PIX_FMT_NV12;
	ctx->time_base = (AVRational){(int)fps_den, (int)fps_num};
	ctx->framerate = (AVRational){(int)fps_num, (int)fps_den};
	ctx->gop_size = keyint;
	ctx->max_b_frames = 0;
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
	ctx->color_range = AVCOL_RANGE_MPEG;
	ctx->colorspace = AVCOL_SPC_BT709;
	ctx->color_primaries = AVCOL_PRI_BT709;
	ctx->color_trc = AVCOL_TRC_BT709;
	ctx->thread_count = 0;
	set_encoder_options(ctx, codec_name, qp);

	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		avcodec_free_context(&ctx);
		return false;
	}
	frame->format = AV_PIX_FMT_NV12;
	frame->width = ctx->width;
	frame->height = ctx->height;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->colorspace = AVCOL_SPC_BT709;
	frame->color_primaries = AVCOL_PRI_BT709;
	frame->color_trc = AVCOL_TRC_BT709;
	if (av_frame_get_buffer(frame, 32) < 0) {
		av_frame_free(&frame);
		avcodec_free_context(&ctx);
		return false;
	}

	encoder->codec_ctx = ctx;
	encoder->codec = codec;
	encoder->nv12 = frame;
	return true;
}

static const char *backend_codec(enum sr_encoder_backend backend)
{
	switch (backend) {
	case SR_ENC_NVENC:
		return "h264_nvenc";
	case SR_ENC_AMF:
		return "h264_amf";
	case SR_ENC_QSV:
		return "h264_qsv";
	case SR_ENC_X264:
		return "libx264";
	case SR_ENC_AUTO:
	default:
		return NULL;
	}
}

struct sr_encoder *sr_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
				     enum sr_encoder_backend backend, int qp, uint32_t gop_interval_ms)
{
	if (!width || !height || !fps_num || !fps_den)
		return NULL;

	struct sr_encoder *encoder = bzalloc(sizeof(*encoder));
	encoder->converter_input = AV_PIX_FMT_NONE;
	const int keyint = gop_frames(fps_num, fps_den, gop_interval_ms);
	const char *requested = backend_codec(backend);
	bool opened = false;

	if (requested) {
		opened = try_encoder(encoder, requested, width, height, fps_num, fps_den, qp, keyint);
		if (!opened && backend != SR_ENC_X264) {
			blog(LOG_WARNING, "Pitel Instant Replay: encoder '%s' unavailable; trying GPL Bridge software fallback",
			     requested);
			opened = try_encoder(encoder, "libx264", width, height, fps_num, fps_den, qp, keyint);
		}
	} else {
		static const char *const candidates[] = {"h264_nvenc", "h264_amf", "h264_qsv", "libx264"};
		for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && !opened; ++i)
			opened = try_encoder(encoder, candidates[i], width, height, fps_num, fps_den, qp, keyint);
	}

	if (!opened) {
		bfree(encoder);
		return NULL;
	}

	const double keyint_ms = 1000.0 * (double)keyint * (double)fps_den / (double)fps_num;
	blog(LOG_INFO, "Pitel Instant Replay: opened %s encoder, %dx%d, GOP %d frames (%.1f ms), B=0",
	     encoder->codec->name, encoder->codec_ctx->width, encoder->codec_ctx->height, keyint, keyint_ms);
	return encoder;
}

void sr_encoder_destroy(struct sr_encoder *encoder)
{
	if (!encoder)
		return;
	if (encoder->converter)
		sws_freeContext(encoder->converter);
	av_frame_free(&encoder->nv12);
	avcodec_free_context(&encoder->codec_ctx);
	bfree(encoder);
}

static bool copy_limited_nv12(struct sr_encoder *encoder, const struct obs_source_frame *source)
{
	if (source->format != VIDEO_FORMAT_NV12 || source->full_range || !source->data[0] || !source->data[1])
		return false;
	if (source->width != (uint32_t)encoder->codec_ctx->width || source->height != (uint32_t)encoder->codec_ctx->height)
		return false;

	av_image_copy_plane(encoder->nv12->data[0], encoder->nv12->linesize[0], source->data[0],
			    (int)source->linesize[0], encoder->codec_ctx->width, encoder->codec_ctx->height);
	av_image_copy_plane(encoder->nv12->data[1], encoder->nv12->linesize[1], source->data[1],
			    (int)source->linesize[1], encoder->codec_ctx->width, encoder->codec_ctx->height / 2);
	return true;
}

static bool convert_to_limited_nv12(struct sr_encoder *encoder, const struct obs_source_frame *source,
				    enum AVPixelFormat input_format)
{
	if (encoder->converter_input != input_format) {
		if (encoder->converter)
			sws_freeContext(encoder->converter);
		encoder->converter = sws_getContext((int)source->width, (int)source->height, input_format,
						encoder->codec_ctx->width, encoder->codec_ctx->height, AV_PIX_FMT_NV12,
						SWS_BILINEAR, NULL, NULL, NULL);
		encoder->converter_input = input_format;
	}
	if (!encoder->converter)
		return false;

	const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
	if (sws_setColorspaceDetails(encoder->converter, coefficients, source->full_range ? 1 : 0, coefficients, 0, 0,
				     1 << 16, 1 << 16) < 0)
		return false;

	const uint8_t *planes[MAX_AV_PLANES] = {0};
	int strides[MAX_AV_PLANES] = {0};
	for (size_t i = 0; i < MAX_AV_PLANES; ++i) {
		planes[i] = source->data[i];
		strides[i] = (int)source->linesize[i];
	}
	return sws_scale(encoder->converter, planes, strides, 0, (int)source->height, encoder->nv12->data,
			 encoder->nv12->linesize) > 0;
}

AVPacket *sr_encoder_encode(struct sr_encoder *encoder, const struct obs_source_frame *frame)
{
	if (!encoder || !frame)
		return NULL;
	const enum AVPixelFormat input_format = obs_frame_format(frame->format);
	if (input_format == AV_PIX_FMT_NONE) {
		if (!encoder->warned_format) {
			blog(LOG_WARNING, "Pitel Instant Replay: unsupported OBS video format %d", (int)frame->format);
			encoder->warned_format = true;
		}
		return NULL;
	}
	if (av_frame_make_writable(encoder->nv12) < 0)
		return NULL;

	if (copy_limited_nv12(encoder, frame)) {
		if (!encoder->logged_direct_nv12) {
			blog(LOG_INFO, "Pitel Instant Replay: %s uses direct limited-range NV12 input",
			     encoder->codec->name);
			encoder->logged_direct_nv12 = true;
		}
	} else if (!convert_to_limited_nv12(encoder, frame, input_format)) {
		return NULL;
	}

	encoder->nv12->pts = encoder->next_pts++;
	if (avcodec_send_frame(encoder->codec_ctx, encoder->nv12) < 0)
		return NULL;

	AVPacket *packet = av_packet_alloc();
	if (!packet)
		return NULL;
	if (avcodec_receive_packet(encoder->codec_ctx, packet) < 0) {
		av_packet_free(&packet);
		return NULL;
	}
	return packet;
}

enum AVCodecID sr_encoder_codec_id(const struct sr_encoder *encoder)
{
	return encoder && encoder->codec_ctx ? encoder->codec_ctx->codec_id : AV_CODEC_ID_NONE;
}

const char *sr_encoder_name(const struct sr_encoder *encoder)
{
	return encoder && encoder->codec ? encoder->codec->name : "";
}

void sr_encoder_get_extradata(const struct sr_encoder *encoder, const uint8_t **data, int *size)
{
	if (!data || !size)
		return;
	*data = encoder && encoder->codec_ctx ? encoder->codec_ctx->extradata : NULL;
	*size = encoder && encoder->codec_ctx ? encoder->codec_ctx->extradata_size : 0;
}

static enum AVPixelFormat choose_d3d11(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	UNUSED_PARAMETER(ctx);
	for (const enum AVPixelFormat *candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
		if (*candidate == AV_PIX_FMT_D3D11)
			return *candidate;
	}
	return formats[0];
}

static AVCodecContext *new_decoder_context(const AVCodec *codec, const uint8_t *extradata, int extradata_size)
{
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;
	ctx->thread_count = 2;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (extradata && extradata_size > 0) {
		ctx->extradata = av_mallocz((size_t)extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!ctx->extradata) {
			avcodec_free_context(&ctx);
			return NULL;
		}
		memcpy(ctx->extradata, extradata, (size_t)extradata_size);
		ctx->extradata_size = extradata_size;
	}
	return ctx;
}

static AVBufferRef *new_intel_private_d3d11_device(void)
{
#ifdef _WIN32
	AVBufferRef *device = NULL;
	if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) < 0)
		return NULL;
	return device;
#else
	return NULL;
#endif
}

static bool attach_hardware_device(AVCodecContext *ctx, bool intel, bool prefer_hardware)
{
	AVBufferRef *device = NULL;
	if (intel)
		device = new_intel_private_d3d11_device();
	else if (prefer_hardware)
		device = sr_gpu_create_replay_decode_device();
	if (!device)
		return false;

	ctx->hw_device_ctx = av_buffer_ref(device);
	av_buffer_unref(&device);
	if (!ctx->hw_device_ctx)
		return false;
	ctx->get_format = choose_d3d11;
	return true;
}

static AVCodecContext *open_decoder_context(const AVCodec *codec, const uint8_t *extradata, int extradata_size,
					    bool intel, bool prefer_hardware, bool *hardware_attached)
{
	*hardware_attached = false;
	AVCodecContext *ctx = new_decoder_context(codec, extradata, extradata_size);
	if (!ctx)
		return NULL;
	*hardware_attached = attach_hardware_device(ctx, intel, prefer_hardware);
	if (avcodec_open2(ctx, codec, NULL) >= 0)
		return ctx;

	avcodec_free_context(&ctx);
	if (!*hardware_attached)
		return NULL;

	/* A replay must stay usable even when a driver rejects D3D11VA for a
	 * particular profile. Retry once with a completely clean software context. */
	*hardware_attached = false;
	ctx = new_decoder_context(codec, extradata, extradata_size);
	if (!ctx)
		return NULL;
	if (avcodec_open2(ctx, codec, NULL) < 0) {
		avcodec_free_context(&ctx);
		return NULL;
	}
	return ctx;
}

static struct sr_decoder *decoder_create_common(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size,
						 bool prefer_hardware)
{
	const AVCodec *codec = avcodec_find_decoder(codec_id);
	if (!codec)
		return NULL;

	const bool intel = sr_gpu_active_adapter_vendor_id() == SR_GPU_VENDOR_ID_INTEL;
	bool hardware_attached = false;
	AVCodecContext *ctx = open_decoder_context(codec, extradata, extradata_size, intel, prefer_hardware,
						   &hardware_attached);
	if (!ctx)
		return NULL;

	struct sr_decoder *decoder = bzalloc(sizeof(*decoder));
	decoder->codec_ctx = ctx;
	decoder->output = av_frame_alloc();
	decoder->download_hardware_frames = intel && hardware_attached;
	if (!decoder->output) {
		avcodec_free_context(&decoder->codec_ctx);
		bfree(decoder);
		return NULL;
	}
	return decoder;
}

struct sr_decoder *sr_decoder_create(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size)
{
	return decoder_create_common(codec_id, extradata, extradata_size, false);
}

struct sr_decoder *sr_decoder_create_replay(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size)
{
	return decoder_create_common(codec_id, extradata, extradata_size, true);
}

void sr_decoder_destroy(struct sr_decoder *decoder)
{
	if (!decoder)
		return;
	av_frame_free(&decoder->output);
	avcodec_free_context(&decoder->codec_ctx);
	bfree(decoder);
}

bool sr_decoder_is_hardware(const struct sr_decoder *decoder)
{
	return decoder && decoder->used_hardware;
}

void sr_decoder_flush(struct sr_decoder *decoder)
{
	if (decoder && decoder->codec_ctx)
		avcodec_flush_buffers(decoder->codec_ctx);
}

bool sr_decoder_decode(struct sr_decoder *decoder, const AVPacket *packet, AVFrame **frame)
{
	if (!decoder || !packet || !frame)
		return false;
	if (avcodec_send_packet(decoder->codec_ctx, packet) < 0)
		return false;

	av_frame_unref(decoder->output);
	if (avcodec_receive_frame(decoder->codec_ctx, decoder->output) < 0)
		return false;

	decoder->used_hardware = sr_gpu_frame_is_native(decoder->output);
	if (decoder->used_hardware && decoder->download_hardware_frames) {
		AVFrame *download = av_frame_alloc();
		if (!download)
			return false;
		if (av_hwframe_transfer_data(download, decoder->output, 0) < 0) {
			av_frame_free(&download);
			return false;
		}
		av_frame_copy_props(download, decoder->output);
		av_frame_unref(decoder->output);
		av_frame_move_ref(decoder->output, download);
		av_frame_free(&download);
	}

	*frame = decoder->output;
	return true;
}
