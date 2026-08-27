/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-gpu-video.h"

#include <obs-module.h>
#include <graphics/graphics.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
#include <libswscale/swscale.h>
}

#include <limits>
#include <vector>

struct sr_gpu_renderer {
	/* Native D3D11VA presentation target. Keep this at decoded-frame size and
	 * let the OBS sprite shader do any Multiview scaling. Several D3D11 video
	 * processor drivers apply different range/gamma behaviour when the video
	 * processor also scales, which made Multiview disagree with Replay A. */
	gs_texture_t *texture = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	/* Software decoded frames must not be uploaded into the video-processor
	 * render target. In particular Intel D3D11 can leave updates to that
	 * resource black in an obs_display swapchain. Use a normal dynamic BGRA
	 * texture for CPU uploads instead. */
	gs_texture_t *upload_texture = nullptr;
	uint32_t upload_width = 0;
	uint32_t upload_height = 0;

	SwsContext *sws = nullptr;
	std::vector<uint8_t> bgra;
	bool native_failure_logged = false;

#ifdef _WIN32
	ID3D11Device *device = nullptr; /* borrowed from OBS */
	ID3D11DeviceContext *context = nullptr;
	ID3D11VideoDevice *video_device = nullptr;
	ID3D11VideoContext *video_context = nullptr;
	ID3D11VideoProcessorEnumerator *enumerator = nullptr;
	ID3D11VideoProcessor *processor = nullptr;
	ID3D11VideoProcessorOutputView *output_view = nullptr;
	uint32_t processor_src_width = 0;
	uint32_t processor_src_height = 0;
	uint32_t processor_dst_width = 0;
	uint32_t processor_dst_height = 0;
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

static uint32_t d3d11_device_vendor_id(ID3D11Device *device)
{
	if (!device)
		return 0;

	IDXGIDevice *dxgi_device = nullptr;
	if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))))
		return 0;

	IDXGIAdapter *adapter = nullptr;
	uint32_t vendor = 0;
	if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
		DXGI_ADAPTER_DESC desc = {};
		if (SUCCEEDED(adapter->GetDesc(&desc)))
			vendor = desc.VendorId;
	}
	com_release(adapter);
	com_release(dxgi_device);
	return vendor;
}

static void ffmpeg_d3d11_lock(void *unused)
{
	(void)unused;
	/* FFmpeg may decode during Cue/coverage validation on a non-render
	 * thread. Use libobs's graphics-context lock around immediate/video
	 * context calls. gs_enter_context is recursive for a thread that already
	 * owns the same graphics context, which also makes this safe when decode
	 * happens from video_tick. */
	obs_enter_graphics();
}

static void ffmpeg_d3d11_unlock(void *unused)
{
	(void)unused;
	obs_leave_graphics();
}

static void release_cached_d3d11_texture(void *unused, uint8_t *data)
{
	(void)unused;
	ID3D11Texture2D *texture = reinterpret_cast<ID3D11Texture2D *>(data);
	if (texture)
		texture->Release();
}

static void reset_processor_dimensions(sr_gpu_renderer *renderer)
{
	if (!renderer)
		return;
	renderer->processor_src_width = 0;
	renderer->processor_src_height = 0;
	renderer->processor_dst_width = 0;
	renderer->processor_dst_height = 0;
}

static void release_d3d11_pipeline(sr_gpu_renderer *renderer)
{
	if (!renderer)
		return;
	com_release(renderer->output_view);
	com_release(renderer->processor);
	com_release(renderer->enumerator);
	com_release(renderer->video_context);
	com_release(renderer->video_device);
	com_release(renderer->context);
	renderer->device = nullptr;
	reset_processor_dimensions(renderer);
}
#endif

static void destroy_graphics_resources(sr_gpu_renderer *renderer)
{
	if (!renderer)
		return;
#ifdef _WIN32
	release_d3d11_pipeline(renderer);
#endif
	if (renderer->texture) {
		gs_texture_destroy(renderer->texture);
		renderer->texture = nullptr;
	}
	if (renderer->upload_texture) {
		gs_texture_destroy(renderer->upload_texture);
		renderer->upload_texture = nullptr;
	}
	renderer->width = 0;
	renderer->height = 0;
	renderer->upload_width = 0;
	renderer->upload_height = 0;
}

extern "C" AVBufferRef *sr_gpu_create_replay_decode_device(void)
{
#ifdef _WIN32
	AVBufferRef *device_ref = nullptr;
	obs_enter_graphics();
	if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
		ID3D11Device *device = static_cast<ID3D11Device *>(gs_get_device_obj());
		/* Intel hybrid/iGPU D3D11VA has two problems in this plugin's current
		 * path: multiple replay decoders can stall the shared immediate context,
		 * and Intel's legacy VideoProcessor colour conversion does not match the
		 * OBS compositor. Keep Intel recording on QSV, but decode replay frames
		 * in software until a dedicated oneVPL/D3D11 interop path is added. */
		if (device && d3d11_device_vendor_id(device) != SR_GPU_VENDOR_ID_INTEL) {
			device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
			if (device_ref) {
				AVHWDeviceContext *hw = reinterpret_cast<AVHWDeviceContext *>(device_ref->data);
				AVD3D11VADeviceContext *d3d = static_cast<AVD3D11VADeviceContext *>(hw->hwctx);
				device->AddRef();
				d3d->device = device;
				d3d->lock = ffmpeg_d3d11_lock;
				d3d->unlock = ffmpeg_d3d11_unlock;
				d3d->lock_ctx = nullptr;
				/* FFmpeg initializes the immediate/video contexts from this
				 * borrowed OBS device. Decoder-surface bind flags are selected by
				 * FFmpeg itself; older OBS FFmpeg builds do not expose BindFlags on
				 * AVD3D11VADeviceContext. */
				if (av_hwdevice_ctx_init(device_ref) < 0)
					av_buffer_unref(&device_ref);
			}
		}
	}
	obs_leave_graphics();
	return device_ref;
#else
	return nullptr;
#endif
}

extern "C" bool sr_gpu_replay_zero_copy_available(void)
{
#ifdef _WIN32
	bool available = false;
	obs_enter_graphics();
	if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
		ID3D11Device *device = static_cast<ID3D11Device *>(gs_get_device_obj());
		available = device && d3d11_device_vendor_id(device) != SR_GPU_VENDOR_ID_INTEL;
	}
	obs_leave_graphics();
	return available;
#else
	return false;
#endif
}

extern "C" uint32_t sr_gpu_active_adapter_vendor_id(void)
{
#ifdef _WIN32
	uint32_t vendor = 0;
	obs_enter_graphics();
	if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11)
		vendor = d3d11_device_vendor_id(static_cast<ID3D11Device *>(gs_get_device_obj()));
	obs_leave_graphics();
	return vendor;
#else
	return 0;
#endif
}

extern "C" bool sr_gpu_program_texture_encode_available(void)
{
	const uint32_t vendor = sr_gpu_active_adapter_vendor_id();
	return vendor == SR_GPU_VENDOR_ID_NVIDIA || vendor == SR_GPU_VENDOR_ID_AMD;
}

extern "C" bool sr_gpu_multiview_hardware_decode_safe(void)
{
	const uint32_t vendor = sr_gpu_active_adapter_vendor_id();
	return vendor == SR_GPU_VENDOR_ID_NVIDIA || vendor == SR_GPU_VENDOR_ID_AMD;
}

extern "C" bool sr_gpu_frame_is_native(const AVFrame *frame)
{
#ifdef _WIN32
	return frame && frame->format == AV_PIX_FMT_D3D11 && frame->data[0] != nullptr;
#else
	(void)frame;
	return false;
#endif
}

extern "C" AVFrame *sr_gpu_frame_clone_for_cache(const AVFrame *frame)
{
	if (!frame)
		return nullptr;

#ifdef _WIN32
	if (sr_gpu_frame_is_native(frame)) {
		ID3D11Texture2D *input = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
		const UINT array_slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
		ID3D11Texture2D *cached_texture = nullptr;

		obs_enter_graphics();
		ID3D11Device *obs_device = gs_get_device_type() == GS_DEVICE_DIRECT3D_11
						   ? static_cast<ID3D11Device *>(gs_get_device_obj())
						   : nullptr;
		ID3D11Device *input_device = nullptr;
		input->GetDevice(&input_device);

		if (obs_device && input_device == obs_device) {
			D3D11_TEXTURE2D_DESC desc = {};
			input->GetDesc(&desc);
			const UINT source_mip_levels = desc.MipLevels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			if (SUCCEEDED(obs_device->CreateTexture2D(&desc, nullptr, &cached_texture))) {
				ID3D11DeviceContext *context = nullptr;
				obs_device->GetImmediateContext(&context);
				if (context) {
					const UINT source_subresource =
						D3D11CalcSubresource(0, array_slice, source_mip_levels);
					context->CopySubresourceRegion(cached_texture, 0, 0, 0, 0, input,
								       source_subresource, nullptr);
					context->Release();
				} else {
					cached_texture->Release();
					cached_texture = nullptr;
				}
			}
		}

		if (input_device)
			input_device->Release();
		obs_leave_graphics();

		if (!cached_texture)
			return nullptr;

		AVFrame *copy = av_frame_alloc();
		if (!copy) {
			cached_texture->Release();
			return nullptr;
		}

		AVBufferRef *texture_ref = av_buffer_create(reinterpret_cast<uint8_t *>(cached_texture), 1,
							    release_cached_d3d11_texture, nullptr, 0);
		if (!texture_ref) {
			cached_texture->Release();
			av_frame_free(&copy);
			return nullptr;
		}

		copy->format = AV_PIX_FMT_D3D11;
		copy->width = frame->width;
		copy->height = frame->height;
		copy->data[0] = reinterpret_cast<uint8_t *>(cached_texture);
		copy->data[1] = nullptr; /* standalone texture, array slice zero */
		copy->buf[0] = texture_ref;
		if (frame->hw_frames_ctx)
			copy->hw_frames_ctx = av_buffer_ref(frame->hw_frames_ctx);
		av_frame_copy_props(copy, frame);
		return copy;
	}
#endif

	return av_frame_clone(frame);
}

extern "C" sr_gpu_renderer *sr_gpu_renderer_create(void)
{
	return new sr_gpu_renderer();
}

extern "C" void sr_gpu_renderer_destroy(sr_gpu_renderer *renderer)
{
	if (!renderer)
		return;

	/* Source destruction can happen outside the render thread. libobs uses
	 * this same enter/leave pair for sources that own graphics resources. */
	obs_enter_graphics();
	destroy_graphics_resources(renderer);
	obs_leave_graphics();

	if (renderer->sws)
		sws_freeContext(renderer->sws);
	delete renderer;
}

static bool ensure_target_texture(sr_gpu_renderer *renderer, uint32_t width, uint32_t height)
{
	if (!renderer || !width || !height)
		return false;
	if (renderer->texture && renderer->width == width && renderer->height == height)
		return true;

#ifdef _WIN32
	com_release(renderer->output_view);
	reset_processor_dimensions(renderer);
#endif
	if (renderer->texture)
		gs_texture_destroy(renderer->texture);

	/* A typed BGRA resource can be both an OBS shader input and a D3D11 video
	 * processor output. This is the only GPU-side copy/conversion in the
	 * native path; decoded NV12 never enters system memory. */
	renderer->texture = gs_texture_create(width, height, GS_BGRA_UNORM, 1, nullptr, GS_RENDER_TARGET);
	if (!renderer->texture) {
		renderer->width = 0;
		renderer->height = 0;
		return false;
	}
	renderer->width = width;
	renderer->height = height;
	return true;
}

static bool ensure_upload_texture(sr_gpu_renderer *renderer, uint32_t width, uint32_t height)
{
	if (!renderer || !width || !height)
		return false;
	if (renderer->upload_texture && renderer->upload_width == width && renderer->upload_height == height)
		return true;

	if (renderer->upload_texture)
		gs_texture_destroy(renderer->upload_texture);
	renderer->upload_texture = gs_texture_create(width, height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
	if (!renderer->upload_texture) {
		renderer->upload_width = 0;
		renderer->upload_height = 0;
		return false;
	}
	renderer->upload_width = width;
	renderer->upload_height = height;
	return true;
}

static void draw_sdr_texture(gs_texture_t *texture, uint32_t width, uint32_t height);

#ifdef _WIN32
static bool ensure_d3d11_pipeline(sr_gpu_renderer *renderer, uint32_t src_width, uint32_t src_height,
				  uint32_t dst_width, uint32_t dst_height)
{
	if (!renderer || !src_width || !src_height || !dst_width || !dst_height ||
	    gs_get_device_type() != GS_DEVICE_DIRECT3D_11)
		return false;

	ID3D11Device *current_device = static_cast<ID3D11Device *>(gs_get_device_obj());
	if (!current_device)
		return false;

	if (renderer->device && renderer->device != current_device) {
		/* OBS recreated the graphics device. Drop every native object and let
		 * the next frame rebuild against the new device. */
		release_d3d11_pipeline(renderer);
	}

	if (!renderer->device) {
		renderer->device = current_device;
		current_device->GetImmediateContext(&renderer->context);
		if (!renderer->context ||
		    FAILED(current_device->QueryInterface(__uuidof(ID3D11VideoDevice),
							  reinterpret_cast<void **>(&renderer->video_device))) ||
		    FAILED(renderer->context->QueryInterface(__uuidof(ID3D11VideoContext),
							     reinterpret_cast<void **>(&renderer->video_context)))) {
			release_d3d11_pipeline(renderer);
			return false;
		}
	}

	if (renderer->processor && renderer->processor_src_width == src_width &&
	    renderer->processor_src_height == src_height && renderer->processor_dst_width == dst_width &&
	    renderer->processor_dst_height == dst_height && renderer->output_view)
		return true;

	com_release(renderer->output_view);
	com_release(renderer->processor);
	com_release(renderer->enumerator);

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputFrameRate.Numerator = 60;
	content.InputFrameRate.Denominator = 1;
	content.InputWidth = src_width;
	content.InputHeight = src_height;
	content.OutputFrameRate.Numerator = 60;
	content.OutputFrameRate.Denominator = 1;
	content.OutputWidth = dst_width;
	content.OutputHeight = dst_height;
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	if (FAILED(renderer->video_device->CreateVideoProcessorEnumerator(&content, &renderer->enumerator)) ||
	    FAILED(renderer->video_device->CreateVideoProcessor(renderer->enumerator, 0, &renderer->processor))) {
		com_release(renderer->processor);
		com_release(renderer->enumerator);
		return false;
	}

	ID3D11Texture2D *target = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(renderer->texture));
	if (!target)
		return false;

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
	output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	output_desc.Texture2D.MipSlice = 0;
	if (FAILED(renderer->video_device->CreateVideoProcessorOutputView(target, renderer->enumerator, &output_desc,
									  &renderer->output_view))) {
		return false;
	}

	renderer->processor_src_width = src_width;
	renderer->processor_src_height = src_height;
	renderer->processor_dst_width = dst_width;
	renderer->processor_dst_height = dst_height;
	return true;
}

static bool draw_native_d3d11(sr_gpu_renderer *renderer, const AVFrame *frame, uint32_t width, uint32_t height)
{
	if (!sr_gpu_frame_is_native(frame) || frame->width <= 0 || frame->height <= 0 || !width || !height)
		return false;

	const uint32_t source_width = static_cast<uint32_t>(frame->width);
	const uint32_t source_height = static_cast<uint32_t>(frame->height);

	/* Do colour conversion at native decoded size. Scaling in the D3D11 Video
	 * Processor caused vendor-dependent levels/gamma in Multiview. The final
	 * gs_draw_sprite below scales the already-converted BGRA texture using the
	 * same OBS shader path as Replay A. */
	if (!ensure_target_texture(renderer, source_width, source_height) ||
	    !ensure_d3d11_pipeline(renderer, source_width, source_height, source_width, source_height))
		return false;

	ID3D11Texture2D *input = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
	const UINT array_slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));

	ID3D11Device *input_device = nullptr;
	input->GetDevice(&input_device);
	const bool same_device = input_device == renderer->device;
	if (input_device)
		input_device->Release();
	if (!same_device)
		return false;

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
	input_desc.FourCC = 0;
	input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	input_desc.Texture2D.MipSlice = 0;
	input_desc.Texture2D.ArraySlice = array_slice;

	ID3D11VideoProcessorInputView *input_view = nullptr;
	if (FAILED(renderer->video_device->CreateVideoProcessorInputView(input, renderer->enumerator, &input_desc,
									 &input_view)))
		return false;

	RECT source_rect = {0, 0, static_cast<LONG>(source_width), static_cast<LONG>(source_height)};
	RECT dest_rect = source_rect;
	renderer->video_context->VideoProcessorSetStreamFrameFormat(renderer->processor, 0,
								    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	renderer->video_context->VideoProcessorSetStreamSourceRect(renderer->processor, 0, TRUE, &source_rect);
	renderer->video_context->VideoProcessorSetStreamDestRect(renderer->processor, 0, TRUE, &dest_rect);
	renderer->video_context->VideoProcessorSetOutputTargetRect(renderer->processor, TRUE, &dest_rect);

	/* Stored replay is tagged BT.709 limited-range. Tell the video processor
	 * explicitly rather than relying on driver defaults (often BT.601). */
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color = {};
	input_color.YCbCr_Matrix = 1;  /* BT.709 */
	input_color.Nominal_Range = 1; /* 16-235 */
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
	output_color.RGB_Range = 0;     /* full-range RGB */
	output_color.Nominal_Range = 2; /* 0-255 */
	renderer->video_context->VideoProcessorSetStreamColorSpace(renderer->processor, 0, &input_color);
	renderer->video_context->VideoProcessorSetOutputColorSpace(renderer->processor, &output_color);

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = input_view;
	const HRESULT hr =
		renderer->video_context->VideoProcessorBlt(renderer->processor, renderer->output_view, 0, 1, &stream);
	input_view->Release();
	if (FAILED(hr))
		return false;

	draw_sdr_texture(renderer->texture, width, height);
	return true;
}
#endif

/* The D3D11 video processor and swscale both leave SDR RGB in its normal
 * nonlinear (display-encoded) form. GS_BGRA_UNORM is required by the native
 * video-processor output path, but unlike GS_BGRA it has no alternate sRGB
 * shader-resource view in libobs. Always decode that display-encoded RGB to
 * linear light in the shader and enable sRGB framebuffer encoding on output.
 * This is the same contract OBS uses while rendering an OBS_SOURCE_SRGB scene
 * item, and it also keeps direct obs_display Multiview previews identical to
 * Replay A instead of depending on whichever linear-sRGB state the caller left
 * active. */
static void draw_sdr_texture(gs_texture_t *texture, uint32_t width, uint32_t height)
{
	if (!texture)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!effect)
		return;

	const bool previous_framebuffer_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	while (gs_effect_loop(effect, "DrawSrgbDecompress"))
		gs_draw_sprite(texture, 0, width, height);

	gs_enable_framebuffer_srgb(previous_framebuffer_srgb);
}

static bool draw_software(sr_gpu_renderer *renderer, const AVFrame *frame, uint32_t width, uint32_t height)
{
	if (!renderer || !frame || width == 0 || height == 0)
		return false;

	const AVFrame *source = frame;
	AVFrame *transferred = nullptr;
	if (sr_gpu_frame_is_native(frame)) {
		transferred = av_frame_alloc();
		if (!transferred || av_hwframe_transfer_data(transferred, frame, 0) < 0) {
			av_frame_free(&transferred);
			return false;
		}
		av_frame_copy_props(transferred, frame);
		source = transferred;
	}

	if (source->format < 0 || !ensure_upload_texture(renderer, width, height)) {
		av_frame_free(&transferred);
		return false;
	}

	const uint64_t bytes64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ULL;
	if (bytes64 > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
		av_frame_free(&transferred);
		return false;
	}
	renderer->bgra.resize(static_cast<size_t>(bytes64));

	renderer->sws = sws_getCachedContext(renderer->sws, source->width, source->height,
					     static_cast<AVPixelFormat>(source->format), width, height, AV_PIX_FMT_BGRA,
					     SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!renderer->sws) {
		av_frame_free(&transferred);
		return false;
	}

	const int *coefficients = sws_getCoefficients(SWS_CS_ITU709);
	const int source_full_range = source->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
	sws_setColorspaceDetails(renderer->sws, coefficients, source_full_range, coefficients, 1, 0, 1 << 16, 1 << 16);

	uint8_t *dst_data[4] = {renderer->bgra.data(), nullptr, nullptr, nullptr};
	int dst_linesize[4] = {static_cast<int>(width * 4u), 0, 0, 0};
	const int rows =
		sws_scale(renderer->sws, source->data, source->linesize, 0, source->height, dst_data, dst_linesize);
	av_frame_free(&transferred);
	if (rows <= 0)
		return false;

	gs_texture_set_image(renderer->upload_texture, renderer->bgra.data(), width * 4u, false);
	draw_sdr_texture(renderer->upload_texture, width, height);
	return true;
}

extern "C" bool sr_gpu_renderer_draw(sr_gpu_renderer *renderer, const AVFrame *frame, uint32_t width, uint32_t height)
{
	if (!renderer || !frame)
		return false;

#ifdef _WIN32
	if (sr_gpu_frame_is_native(frame)) {
		if (draw_native_d3d11(renderer, frame, width, height))
			return true;
		if (!renderer->native_failure_logged) {
			blog(LOG_WARNING,
			     "Pitel Instant Replay: D3D11 zero-CPU-copy presentation failed; using GPU->CPU fallback");
			renderer->native_failure_logged = true;
		}
	}
#endif
	return draw_software(renderer, frame, width, height);
}
