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

#include <libavutil/frame.h>

#include "sr-buffer.h"
#include "sr-codec.h"
#include "sr-frame-cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode target_index from a frozen replay while preserving decoder reference
 * state for efficient forward playback. If target_index is behind the decoder
 * state (or there is no state), the decoder is flushed and decoding restarts
 * at the nearest preceding keyframe. Forward jumps decode every intervening
 * packet so P-frame references remain valid.
 *
 * If cache is non-NULL, decoded pictures are retained under a bounded memory
 * budget. A cached backward/random target can be returned without disturbing
 * the decoder's newer forward reference state, which makes reverse and jog
 * practical with short GOPs. The returned AVFrame is owned by either the
 * decoder or cache and must not be freed by the caller. */
bool sr_replay_decode_frame_at(struct sr_decoder *decoder, const struct sr_replay *replay, struct sr_frame_cache *cache,
			       int64_t *current_index, size_t target_index, AVFrame **frame);

#ifdef __cplusplus
}
#endif
