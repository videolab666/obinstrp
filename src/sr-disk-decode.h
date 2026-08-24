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

/* Correctness-first random-access decoder for the new disk store. It locates
 * the segment covering target_ns, seeks to the preceding keyframe, decodes
 * forward, and returns a clone of the frame at/before the requested timestamp.
 *
 * The caller owns *frame and must release it with av_frame_free(). The current
 * replay-optimized codec policy requires B-frames=0; that keeps packet/frame
 * presentation order identical and is also the policy planned for short-GOP
 * NVENC/QSV/AMF recording.
 *
 * This helper intentionally rescans/reopens storage per request. It proves the
 * keyframe-aware path first; the real ReplayPlayer will keep catalog, reader,
 * decoder and decoded-frame cache warm for jog/shuttle performance. */
bool sr_disk_decode_frame_at(const char *session_dir, const char *camera_name, uint64_t target_ns, AVFrame **frame,
			     uint64_t *actual_timestamp_ns);

#ifdef __cplusplus
}
#endif
