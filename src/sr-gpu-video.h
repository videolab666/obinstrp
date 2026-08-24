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

#include <libavutil/buffer.h>
#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On Windows/D3D11 this creates an FFmpeg D3D11VA device backed by OBS's own
 * ID3D11Device. Decoded surfaces therefore stay on the same GPU/device as the
 * OBS compositor and can be presented without a GPU->CPU readback. Returns
 * NULL when the active OBS renderer cannot provide a compatible device. */
AVBufferRef *sr_gpu_create_replay_decode_device(void);

/* True when the current platform/renderer has a native zero-CPU-copy replay
 * presentation path. The decoder still has a software fallback. */
bool sr_gpu_replay_zero_copy_available(void);

/* True for an AVFrame that can be consumed by the native renderer without
 * first transferring it into system memory. */
bool sr_gpu_frame_is_native(const AVFrame *frame);

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
