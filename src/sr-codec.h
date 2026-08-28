/*
 * Pitel Instant Replay - media codec interface
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#include <obs-module.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sr_encoder_backend {
	SR_ENC_AUTO = 0,
	SR_ENC_NVENC,
	SR_ENC_AMF,
	SR_ENC_QSV,
	SR_ENC_X264,
};

enum sr_gop_interval {
	SR_GOP_ALL_I = 0,
	SR_GOP_250MS = 250,
	SR_GOP_500MS = 500,
	SR_GOP_1000MS = 1000,
};

struct sr_encoder;
struct sr_gpu_encoder;
struct sr_decoder;

struct sr_encoder *sr_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
				     enum sr_encoder_backend backend, int qp, uint32_t gop_interval_ms);
void sr_encoder_destroy(struct sr_encoder *encoder);
AVPacket *sr_encoder_encode(struct sr_encoder *encoder, const struct obs_source_frame *frame);
enum AVCodecID sr_encoder_codec_id(const struct sr_encoder *encoder);
const char *sr_encoder_name(const struct sr_encoder *encoder);
void sr_encoder_get_extradata(const struct sr_encoder *encoder, const uint8_t **data, int *size);

struct sr_gpu_encoder *sr_gpu_encoder_create(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
					     enum sr_encoder_backend backend, int qp, uint32_t gop_interval_ms);
void sr_gpu_encoder_destroy(struct sr_gpu_encoder *encoder);
bool sr_gpu_encoder_render_encode(struct sr_gpu_encoder *encoder, obs_source_t *target, AVPacket **packet);
bool sr_gpu_encoder_texture_encode(struct sr_gpu_encoder *encoder, gs_texture_t *texture, AVPacket **packet);
enum AVCodecID sr_gpu_encoder_codec_id(const struct sr_gpu_encoder *encoder);
const char *sr_gpu_encoder_name(const struct sr_gpu_encoder *encoder);
void sr_gpu_encoder_get_extradata(const struct sr_gpu_encoder *encoder, const uint8_t **data, int *size);

/* CPU-addressable decoder. On Intel/Windows this still uses an isolated
 * D3D11VA device internally and downloads the decoded frame, avoiding the
 * expensive software H.264 path without sharing OBS's D3D11 context. */
struct sr_decoder *sr_decoder_create(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size);

/* Replay decoder. NVIDIA/AMD may expose native D3D11 frames for zero-copy
 * rendering. Intel deliberately decodes on an isolated D3D11VA device and
 * returns a transferred software frame for driver/context stability. */
struct sr_decoder *sr_decoder_create_replay(enum AVCodecID codec_id, const uint8_t *extradata, int extradata_size);
void sr_decoder_destroy(struct sr_decoder *decoder);
bool sr_decoder_is_hardware(const struct sr_decoder *decoder);
void sr_decoder_flush(struct sr_decoder *decoder);
bool sr_decoder_decode(struct sr_decoder *decoder, const AVPacket *packet, AVFrame **frame);

#ifdef __cplusplus
}
#endif
