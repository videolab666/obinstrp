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

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_master_audio_player;

/* Persistent sequential decoder for the session-wide master replay audio
 * timeline. The player opens the indexed AAC segment containing a seek target
 * and advances across finalized or live .part segments without tying audio to
 * any camera angle. */
struct sr_master_audio_player *sr_master_audio_player_create(const char *session_dir);
void sr_master_audio_player_destroy(struct sr_master_audio_player *player);

/* Re-scans the master-audio catalog. Existing decoder state is preserved until
 * a seek or segment discontinuity requires a reset. */
bool sr_master_audio_player_refresh(struct sr_master_audio_player *player);

/* Positions the sequential decoder at (or one packet before) timestamp_ns so
 * the caller can trim the first decoded frame to an exact Event IN point. */
bool sr_master_audio_player_seek(struct sr_master_audio_player *player, uint64_t timestamp_ns);

/* Decodes the next AAC frame in timeline order. The returned AVFrame is owned
 * by the caller and must be released with av_frame_free(). timestamp_ns is the
 * media timestamp corresponding to the beginning of the decoded frame. */
bool sr_master_audio_player_decode_next(struct sr_master_audio_player *player, AVFrame **frame, uint64_t *timestamp_ns);

bool sr_master_audio_player_get_bounds(struct sr_master_audio_player *player, uint64_t *first_ns, uint64_t *last_ns);

#ifdef __cplusplus
}
#endif
