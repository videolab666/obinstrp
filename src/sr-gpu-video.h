/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/buffer.h>
#include <libavutil/frame.h>

/* On Windows/D3D11 this creates an FFmpeg D3D11VA device backed by OBS's own
 * ID3D11Device. Decoded surfaces therefore stay on the same GPU/device as the
 * OBS compositor and can be presented without a GPU->CPU readback. FFmpeg
 * device access is serialized through OBS's graphics-context lock so Cue and
 * render-thread decoding can safely share the immediate D3D11 context.
 * Returns NULL when the active renderer cannot provide a compatible device. */
AVBufferRef *sr_gpu_create_replay_decode_device(void);

/* True when the current platform/renderer has a native zero-CPU-copy replay
 * presentation path. The decoder still has a software fallback. */
bool sr_gpu_replay_zero_copy_available(void);

#define SR_GPU_VENDOR_ID_INTEL 0x8086u
#define SR_GPU_VENDOR_ID_NVIDIA 0x10DEu
#define SR_GPU_VENDOR_ID_AMD 0x1002u

/* PCI vendor id of the D3D11 adapter that owns OBS's compositor device.
 * Returns 0 when the active renderer is not Windows/D3D11 or the adapter
 * cannot be resolved. */
uint32_t sr_gpu_active_adapter_vendor_id(void);

/* PROGRAM texture encoding is zero-copy only when the encoder and the OBS
 * compositor live on the same D3D11 adapter. The current GPU encoder backend
 * implements NVENC on NVIDIA and AMF on AMD; Intel/QSV texture interop is not
 * implemented yet. */
bool sr_gpu_program_texture_encode_available(void);

/* Multiview can open several independent replay decoders. Intel hybrid/iGPU
 * drivers have shown whole-OBS stalls when several FFmpeg D3D11VA decoders
 * share OBS's immediate/video context. Keep A/B replay on the native path,
 * but use software decode for multiview on Intel and unknown adapters. */
bool sr_gpu_multiview_hardware_decode_safe(void);

/* True for an AVFrame that can be consumed by the native renderer without
 * first transferring it into system memory. */
bool sr_gpu_frame_is_native(const AVFrame *frame);

/* Clone a decoded frame for the long-lived replay LRU. Software frames use a
 * normal AVFrame reference. A D3D11 decoder frame is copied GPU->GPU into an
 * independent one-slice NV12 texture, so the cache does not pin the finite
 * FFmpeg decoder-surface pool. The returned frame owns that texture. */
AVFrame *sr_gpu_frame_clone_for_cache(const AVFrame *frame);

struct sr_gpu_renderer;

/* Renderer creation does not touch the graphics device; GPU resources are
 * created lazily from video_render while the OBS graphics context is active. */
struct sr_gpu_renderer *sr_gpu_renderer_create(void);
void sr_gpu_renderer_destroy(struct sr_gpu_renderer *renderer);

/* Draw one decoded frame into the currently active OBS source render pass.
 * Native D3D11VA frames use a D3D11 video processor into an OBS-owned BGRA
 * texture (GPU-only color conversion). Software frames use a swscale/upload
 * fallback so the Event Output remains functional on every renderer. */
bool sr_gpu_renderer_draw(struct sr_gpu_renderer *renderer, const AVFrame *frame, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif
