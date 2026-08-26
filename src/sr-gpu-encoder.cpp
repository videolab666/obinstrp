/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-codec.h"
#include "sr-gpu-video.h"

#include <graphics/graphics.h>
#include <obs-module.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libavutil/opt.h>
}

#include <cerrno>
#include <climits>
#include <cstring>

struct sr_gpu_encoder {
#ifdef _WIN32
	AVCodecContext *ctx = nullptr;
	const AVCodec *codec = nullptr;
	AVBufferRef *hw_device = nullptr;
	AVBufferRef *hw_frames = nullptr;
	gs_texrender_t *render = nullptr;
	ID3D11Device *device = nullptr; /* borrowed from hw_device */
	ID3D11VideoDevice *video_device = nullptr;
	ID3D11VideoContext *video_context = nullptr;
	ID3D11VideoProcessorEnumerator *enumerator = nullptr;
	ID3D11VideoProcessor *processor = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps_num = 0;
	uint32_t fps_den = 0;
	int64_t next_pts = 0;
	bool render_failure_logged = false;
#endif
};

#ifdef _WIN32
template<typename T> static void com_release(T *&value)
{
	if (value) {
		value->Release();
		value = nullptr;
	}
}

static int gop_frames_from_interval(uint32_t fps_num, uint32_t fps_den, uint32_t gop_interval_ms)
{
	if (!gop_interval_ms || !fps_num || !fps_den)
		return 1;

	const uint64_t numerator = static_cast<uint64_t>(fps_num) * static_cast<uint64_t>(gop_interval_ms);
	const uint64_t denominator = static_cast<uint64_t>(fps_den) * 1000ULL;
	uint64_t frames = (numerator + denominator / 2ULL) / denominator;
	if (!frames)
		frames = 1;
	if (frames > static_cast<uint64_t>(INT_MAX))
		frames = static_cast<uint64_t>(INT_MAX);
	return static_cast<int>(frames);
}

static void configure_encoder_options(AVCodecContext *ctx, const char *name, int qp)
{
	if (strcmp(name, "h264_nvenc") == 0) {
		av_opt_set(ctx->priv_data, "rc", "constqp", 0);
		av_opt_set_int(ctx->priv_data, "qp", qp, 0);
		av_opt_set(ctx->priv_data, "preset", "p4", 0);
		av_opt_set(ctx->priv_data, "tune", "ull", 0);
		av_opt_set_int(ctx->priv_data, "delay", 0, 0);
	} else if (strcmp(name, "h264_amf") == 0) {
		av_opt_set(ctx->priv_data, "rc", "cqp", 0);
		av_opt_set_int(ctx->priv_data, "qp_i", qp, 0);
		av_opt_set_int(ctx->priv_data, "qp_p", qp, 0);
		av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
	}
}

static bool init_video_processor(sr_gpu_encoder *enc)
{
	AVHWDeviceContext *device_ctx = reinterpret_cast<AVHWDeviceContext *>(enc->hw_device->data);
	AVD3D11VADeviceContext *d3d = static_cast<AVD3D11VADeviceContext *>(device_ctx->hwctx);
	if (!d3d || !d3d->device)
		return false;

	enc->device = d3d->device;
	if (FAILED(enc->device->QueryInterface(__uuidof(ID3D11VideoDevice),
					       reinterpret_cast<void **>(&enc->video_device))))
		return false;

	ID3D11DeviceContext *device_context = d3d->device_context;
	if (!device_context) {
		enc->device->GetImmediateContext(&device_context);
		if (!device_context)
			return false;
		const HRESULT hr = device_context->QueryInterface(__uuidof(ID3D11VideoContext),
								  reinterpret_cast<void **>(&enc->video_context));
		device_context->Release();
		if (FAILED(hr))
			return false;
	} else if (FAILED(device_context->QueryInterface(__uuidof(ID3D11VideoContext),
							 reinterpret_cast<void **>(&enc->video_context)))) {
		return false;
	}

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputFrameRate.Numerator = enc->fps_num;
	content.InputFrameRate.Denominator = enc->fps_den;
	content.InputWidth = enc->width;
	content.InputHeight = enc->height;
	content.OutputFrameRate = content.InputFrameRate;
	content.OutputWidth = enc->width;
	content.OutputHeight = enc->height;
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	if (FAILED(enc->video_device->CreateVideoProcessorEnumerator(&content, &enc->enumerator)) ||
	    FAILED(enc->video_device->CreateVideoProcessor(enc->enumerator, 0, &enc->processor)))
		return false;

	return true;
}

static bool init_hw_frames(sr_gpu_encoder *enc)
{
	enc->hw_frames = av_hwframe_ctx_alloc(enc->hw_device);
	if (!enc->hw_frames)
		return false;

	AVHWFramesContext *frames = reinterpret_cast<AVHWFramesContext *>(enc->hw_frames->data);
	frames->format = AV_PIX_FMT_D3D11;
	frames->sw_format = AV_PIX_FMT_NV12;
	frames->width = static_cast<int>(enc->width);
	frames->height = static_cast<int>(enc->height);
	/* Dynamic allocation gives every in-flight encoder frame its own texture.
	 * This avoids an array-surface decoder-style pool and lets NVENC/AMF retain
	 * frames asynchronously without blocking the render callback. */
	frames->initial_pool_size = 0;

	AVD3D11VAFramesContext *d3d_frames = static_cast<AVD3D11VAFramesContext *>(frames->hwctx);
	if (d3d_frames)
		d3d_frames->BindFlags = D3D11_BIND_RENDER_TARGET;

	return av_hwframe_ctx_init(enc->hw_frames) >= 0;
}

static bool open_gpu_codec(sr_gpu_encoder *enc, const char *name, int qp, uint32_t gop_interval_ms)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec)
		return false;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;

	const int gop_size = gop_frames_from_interval(enc->fps_num, enc->fps_den, gop_interval_ms);
	ctx->width = static_cast<int>(enc->width);
	ctx->height = static_cast<int>(enc->height);
	ctx->pix_fmt = AV_PIX_FMT_D3D11;
	ctx->time_base = AVRational{static_cast<int>(enc->fps_den), static_cast<int>(enc->fps_num)};
	ctx->framerate = AVRational{static_cast<int>(enc->fps_num), static_cast<int>(enc->fps_den)};
	ctx->gop_size = gop_size > 0 ? gop_size : 1;
	ctx->max_b_frames = 0;
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
	ctx->color_range = AVCOL_RANGE_MPEG;
	ctx->colorspace = AVCOL_SPC_BT709;
	ctx->color_primaries = AVCOL_PRI_BT709;
	ctx->color_trc = AVCOL_TRC_BT709;
	ctx->thread_count = 0;
	ctx->hw_frames_ctx = av_buffer_ref(enc->hw_frames);
	if (!ctx->hw_frames_ctx) {
		avcodec_free_context(&ctx);
		return false;
	}

	configure_encoder_options(ctx, name, qp);
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return false;
	}

	enc->ctx = ctx;
	enc->codec = codec;
	const double actual_gop_ms = 1000.0 * static_cast<double>(gop_size) * static_cast<double>(enc->fps_den) /
				     static_cast<double>(enc->fps_num);
	blog(LOG_INFO,
	     "Pitel Instant Replay: opened GPU replay encoder '%s' (%ux%u, qp %d, GOP %d frames / %.1f ms, D3D11 NV12, B=0)",
	     codec->name, enc->width, enc->height, qp, gop_size, actual_gop_ms);
	return true;
}

static bool render_target_to_texture(sr_gpu_encoder *enc, obs_source_t *target, ID3D11Texture2D **texture)
{
	*texture = nullptr;
	if (!target || !enc->render)
		return true;

	gs_texrender_reset(enc->render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(enc->render, enc->width, enc->height, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(enc->width), 0.0f, static_cast<float>(enc->height), -100.0f, 100.0f);
		obs_source_video_render(target);
		gs_texrender_end(enc->render);
		rendered = true;
	}
	gs_blend_state_pop();

	if (!rendered)
		return true;

	gs_texture_t *gs_texture = gs_texrender_get_texture(enc->render);
	if (!gs_texture)
		return true;

	ID3D11Texture2D *d3d_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(gs_texture));
	if (!d3d_texture)
		return false;

	ID3D11Device *texture_device = nullptr;
	d3d_texture->GetDevice(&texture_device);
	const bool same_device = texture_device == enc->device;
	if (texture_device)
		texture_device->Release();
	if (!same_device)
		return false;

	*texture = d3d_texture;
	return true;
}

static bool convert_bgra_to_hw_nv12(sr_gpu_encoder *enc, ID3D11Texture2D *input, AVFrame *output)
{
	if (!input || !output || !output->data[0])
		return false;

	ID3D11Texture2D *output_texture = reinterpret_cast<ID3D11Texture2D *>(output->data[0]);
	const UINT output_slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(output->data[1]));

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
	input_desc.FourCC = 0;
	input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	input_desc.Texture2D.MipSlice = 0;
	input_desc.Texture2D.ArraySlice = 0;

	ID3D11VideoProcessorInputView *input_view = nullptr;
	if (FAILED(enc->video_device->CreateVideoProcessorInputView(input, enc->enumerator, &input_desc, &input_view)))
		return false;

	D3D11_TEXTURE2D_DESC output_texture_desc = {};
	output_texture->GetDesc(&output_texture_desc);
	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
	if (output_texture_desc.ArraySize > 1) {
		output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
		output_desc.Texture2DArray.MipSlice = 0;
		output_desc.Texture2DArray.FirstArraySlice = output_slice;
		output_desc.Texture2DArray.ArraySize = 1;
	} else {
		output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		output_desc.Texture2D.MipSlice = 0;
	}

	ID3D11VideoProcessorOutputView *output_view = nullptr;
	if (FAILED(enc->video_device->CreateVideoProcessorOutputView(output_texture, enc->enumerator, &output_desc,
								     &output_view))) {
		input_view->Release();
		return false;
	}

	RECT rect = {0, 0, static_cast<LONG>(enc->width), static_cast<LONG>(enc->height)};
	enc->video_context->VideoProcessorSetStreamFrameFormat(enc->processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	enc->video_context->VideoProcessorSetStreamSourceRect(enc->processor, 0, TRUE, &rect);
	enc->video_context->VideoProcessorSetStreamDestRect(enc->processor, 0, TRUE, &rect);
	enc->video_context->VideoProcessorSetOutputTargetRect(enc->processor, TRUE, &rect);

	/* The intermediate OBS render texture is full-range RGB. The replay file
	 * contract is BT.709 limited-range NV12, so perform both matrix/range
	 * conversion and packing in the D3D11 video processor. */
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color = {};
	input_color.RGB_Range = 0;
	input_color.Nominal_Range = 2;
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
	output_color.YCbCr_Matrix = 1;
	output_color.Nominal_Range = 1;
	enc->video_context->VideoProcessorSetStreamColorSpace(enc->processor, 0, &input_color);
	enc->video_context->VideoProcessorSetOutputColorSpace(enc->processor, &output_color);

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = input_view;
	const HRESULT hr = enc->video_context->VideoProcessorBlt(enc->processor, output_view, 0, 1, &stream);

	output_view->Release();
	input_view->Release();
	return SUCCEEDED(hr);
}

static sr_gpu_encoder *create_for_name(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
				       const char *name, int qp, uint32_t gop_interval_ms)
{
	if (!width || !height || !fps_num || !fps_den || !name)
		return nullptr;

	sr_gpu_encoder *enc = new sr_gpu_encoder();
	enc->width = width & ~1u;
	enc->height = height & ~1u;
	enc->fps_num = fps_num;
	enc->fps_den = fps_den;
	if (!enc->width || !enc->height) {
		delete enc;
		return nullptr;
	}

	obs_enter_graphics();
	if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
		obs_leave_graphics();
		delete enc;
		return nullptr;
	}

	enc->hw_device = sr_gpu_create_replay_decode_device();
	if (!enc->hw_device || !init_hw_frames(enc) || !init_video_processor(enc)) {
		obs_leave_graphics();
		sr_gpu_encoder_destroy(enc);
		return nullptr;
	}

	enc->render = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);
	if (!enc->render || !open_gpu_codec(enc, name, qp, gop_interval_ms)) {
		obs_leave_graphics();
		sr_gpu_encoder_destroy(enc);
		return nullptr;
	}
	obs_leave_graphics();
	return enc;
}
#endif

extern "C" sr_gpu_encoder *sr_gpu_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
						 enum sr_encoder_backend backend, int qp, uint32_t gop_interval_ms)
{
#ifdef _WIN32
	const char *names[2] = {nullptr, nullptr};
	size_t count = 0;
	switch (backend) {
	case SR_ENC_AUTO:
		names[count++] = "h264_nvenc";
		names[count++] = "h264_amf";
		break;
	case SR_ENC_NVENC:
		names[count++] = "h264_nvenc";
		break;
	case SR_ENC_AMF:
		names[count++] = "h264_amf";
		break;
	case SR_ENC_QSV:
	case SR_ENC_X264:
		return nullptr;
	}

	for (size_t i = 0; i < count; i++) {
		sr_gpu_encoder *enc = create_for_name(width, height, fps_num, fps_den, names[i], qp, gop_interval_ms);
		if (enc)
			return enc;
	}
#else
	(void)width;
	(void)height;
	(void)fps_num;
	(void)fps_den;
	(void)backend;
	(void)qp;
	(void)gop_interval_ms;
#endif
	return nullptr;
}

extern "C" void sr_gpu_encoder_destroy(sr_gpu_encoder *enc)
{
	if (!enc)
		return;
#ifdef _WIN32
	obs_enter_graphics();
	if (enc->render)
		gs_texrender_destroy(enc->render);
	enc->render = nullptr;
	com_release(enc->processor);
	com_release(enc->enumerator);
	com_release(enc->video_context);
	com_release(enc->video_device);
	avcodec_free_context(&enc->ctx);
	av_buffer_unref(&enc->hw_frames);
	av_buffer_unref(&enc->hw_device);
	enc->device = nullptr;
	obs_leave_graphics();
#endif
	delete enc;
}

extern "C" bool sr_gpu_encoder_render_encode(sr_gpu_encoder *enc, obs_source_t *target, AVPacket **packet)
{
	if (packet)
		*packet = nullptr;
#ifdef _WIN32
	if (!enc || !enc->ctx || !packet)
		return false;

	ID3D11Texture2D *source_texture = nullptr;
	if (!render_target_to_texture(enc, target, &source_texture))
		return false;
	if (!source_texture)
		return true;

	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;
	if (av_hwframe_get_buffer(enc->hw_frames, frame, 0) < 0) {
		av_frame_free(&frame);
		return false;
	}

	frame->pts = enc->next_pts++;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->colorspace = AVCOL_SPC_BT709;
	frame->color_primaries = AVCOL_PRI_BT709;
	frame->color_trc = AVCOL_TRC_BT709;
	if (!convert_bgra_to_hw_nv12(enc, source_texture, frame)) {
		av_frame_free(&frame);
		if (!enc->render_failure_logged) {
			blog(LOG_WARNING,
			     "Pitel Instant Replay: D3D11 capture-to-NV12 conversion failed; disabling GPU capture encoder");
			enc->render_failure_logged = true;
		}
		return false;
	}

	const int send_ret = avcodec_send_frame(enc->ctx, frame);
	av_frame_free(&frame);
	if (send_ret < 0)
		return false;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;
	const int receive_ret = avcodec_receive_packet(enc->ctx, pkt);
	if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
		av_packet_free(&pkt);
		return true;
	}
	if (receive_ret < 0) {
		av_packet_free(&pkt);
		return false;
	}

	*packet = pkt;
	return true;
#else
	(void)enc;
	(void)target;
	return false;
#endif
}

#ifdef _WIN32
static bool normalize_program_texture(sr_gpu_encoder *enc, gs_texture_t *texture, ID3D11Texture2D **output)
{
	*output = nullptr;
	if (!enc || !enc->render || !texture)
		return false;

	/* obs_get_main_texture() is the final composited Program image, but its
	 * native D3D11 format/bind flags are an OBS implementation detail and are
	 * not guaranteed to be accepted directly by ID3D11VideoProcessor. Normalize
	 * it into the same known BGRA render target used by the proven ISO-camera
	 * GPU path. This is a GPU-only shader blit; there is no GPU->CPU readback. */
	gs_texrender_reset(enc->render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(enc->render, enc->width, enc->height, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(enc->width), 0.0f, static_cast<float>(enc->height), -100.0f, 100.0f);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = effect ? gs_effect_get_param_by_name(effect, "image") : nullptr;
		if (effect && image) {
			gs_effect_set_texture_srgb(image, texture);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(texture, 0, enc->width, enc->height);
			rendered = true;
		}
		gs_texrender_end(enc->render);
	}
	gs_blend_state_pop();

	if (!rendered)
		return false;

	gs_texture_t *normalized = gs_texrender_get_texture(enc->render);
	if (!normalized)
		return false;

	ID3D11Texture2D *d3d_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(normalized));
	if (!d3d_texture)
		return false;

	ID3D11Device *texture_device = nullptr;
	d3d_texture->GetDevice(&texture_device);
	const bool same_device = texture_device == enc->device;
	if (texture_device)
		texture_device->Release();
	if (!same_device)
		return false;

	*output = d3d_texture;
	return true;
}
#endif

extern "C" bool sr_gpu_encoder_texture_encode(sr_gpu_encoder *enc, gs_texture_t *texture, AVPacket **packet)
{
	if (packet)
		*packet = nullptr;
#ifdef _WIN32
	if (!enc || !enc->ctx || !packet || !texture)
		return false;

	ID3D11Texture2D *source_texture = nullptr;
	if (!normalize_program_texture(enc, texture, &source_texture)) {
		if (!enc->render_failure_logged) {
			blog(LOG_WARNING,
			     "Pitel Instant Replay: PROGRAM main texture could not be normalized to the BGRA replay target");
			enc->render_failure_logged = true;
		}
		return false;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;
	if (av_hwframe_get_buffer(enc->hw_frames, frame, 0) < 0) {
		av_frame_free(&frame);
		return false;
	}
	frame->pts = enc->next_pts++;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->colorspace = AVCOL_SPC_BT709;
	frame->color_primaries = AVCOL_PRI_BT709;
	frame->color_trc = AVCOL_TRC_BT709;
	if (!convert_bgra_to_hw_nv12(enc, source_texture, frame)) {
		av_frame_free(&frame);
		if (!enc->render_failure_logged) {
			blog(LOG_WARNING,
			     "Pitel Instant Replay: D3D11 Program texture-to-NV12 conversion failed; disabling Program encoder");
			enc->render_failure_logged = true;
		}
		return false;
	}
	const int send_ret = avcodec_send_frame(enc->ctx, frame);
	av_frame_free(&frame);
	if (send_ret < 0)
		return false;
	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;
	const int receive_ret = avcodec_receive_packet(enc->ctx, pkt);
	if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
		av_packet_free(&pkt);
		return true;
	}
	if (receive_ret < 0) {
		av_packet_free(&pkt);
		return false;
	}
	*packet = pkt;
	return true;
#else
	(void)enc;
	(void)texture;
	return false;
#endif
}

extern "C" enum AVCodecID sr_gpu_encoder_codec_id(const sr_gpu_encoder *enc)
{
#ifdef _WIN32
	return enc && enc->ctx ? enc->ctx->codec_id : AV_CODEC_ID_NONE;
#else
	(void)enc;
	return AV_CODEC_ID_NONE;
#endif
}

extern "C" const char *sr_gpu_encoder_name(const sr_gpu_encoder *enc)
{
#ifdef _WIN32
	return enc && enc->codec ? enc->codec->name : "";
#else
	(void)enc;
	return "";
#endif
}

extern "C" void sr_gpu_encoder_get_extradata(const sr_gpu_encoder *enc, const uint8_t **data, int *size)
{
	if (data)
		*data = nullptr;
	if (size)
		*size = 0;
#ifdef _WIN32
	if (!enc || !enc->ctx)
		return;
	if (data)
		*data = enc->ctx->extradata;
	if (size)
		*size = enc->ctx->extradata_size;
#else
	(void)enc;
#endif
}
