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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavutil/frame.h>

struct sr_disk_player;

struct sr_disk_player_performance {
	bool decoder_open;
	bool hardware_decode;
	uint64_t requests;
	uint64_t cache_hits;
	uint64_t decoded_frames;
};

/* Persistent keyframe-aware reader for one camera in one continuous replay
 * session. Unlike sr_disk_decode_frame_at(), this object keeps the catalog,
 * current segment, decoder state and last decoded frame warm between seeks.
 * It is the core that will later replace RAM-snapshot playback for timeline
 * events and jog/shuttle operation. */
struct sr_disk_player *sr_disk_player_create(const char *session_dir, const char *camera_name);

/* Same persistent player with a caller-selected software-frame cache budget.
 * Multiview uses a much smaller cache per camera than the A/B playout buses so
 * a 6-9 angle editor cannot multiply the default replay cache by camera count. */
struct sr_disk_player *sr_disk_player_create_with_cache(const char *session_dir, const char *camera_name,
							size_t max_cache_bytes);
void sr_disk_player_destroy(struct sr_disk_player *player);

/* Rescans the camera's segment directory, including a readable active .part
 * pair. Call periodically while recording is still in progress or before a
 * seek close to the live edge. */
bool sr_disk_player_refresh(struct sr_disk_player *player);

/* Returns the current indexed media range for this camera. */
bool sr_disk_player_get_bounds(const struct sr_disk_player *player, uint64_t *first_ns, uint64_t *last_ns);

/* Lightweight transport diagnostics. The caller must serialize access with
 * decode/seek operations (sr_replay_channel already does this with its bus
 * mutex). hardware_decode becomes true only after a native D3D11 frame has
 * actually been produced by the decoder. */
void sr_disk_player_get_performance(const struct sr_disk_player *player,
				    struct sr_disk_player_performance *performance);

/* Decodes the newest frame at/before target_ns. Random/backward seeks start at
 * the nearest preceding keyframe and decode forward. Sequential forward calls
 * reuse the existing decoder state. The returned AVFrame is a clone owned by
 * the caller and must be released with av_frame_free(). */
bool sr_disk_player_decode_at(struct sr_disk_player *player, uint64_t target_ns, AVFrame **frame,
			      uint64_t *actual_timestamp_ns);

/* Finds the timestamp of the immediately adjacent indexed frame without
 * disturbing the persistent decoder state. direction must be -1 or +1. */
bool sr_disk_player_neighbor_timestamp(struct sr_disk_player *player, uint64_t current_ns, int direction,
				       uint64_t *timestamp_ns);

#ifdef __cplusplus
}
#endif
