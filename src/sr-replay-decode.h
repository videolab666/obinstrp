/*
Sports Replay
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

#ifdef __cplusplus
extern "C" {
#endif

/* Decode target_index from a frozen replay while preserving decoder reference
 * state for efficient forward playback. If target_index is behind the current
 * position (or there is no current position), the decoder is flushed and the
 * function starts at the nearest preceding keyframe. Forward jumps decode all
 * intervening packets so P-frame references remain valid.
 *
 * On success, *frame points to the decoder-owned AVFrame and is valid until the
 * next decoder call. *current_index is advanced to target_index. */
bool sr_replay_decode_frame_at(struct sr_decoder *decoder, const struct sr_replay *replay, int64_t *current_index,
			       size_t target_index, AVFrame **frame);

#ifdef __cplusplus
}
#endif
