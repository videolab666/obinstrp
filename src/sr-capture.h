/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "sr-buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CAPTURE_ID "pitel_instant_replay_capture"
#define SR_PLAYBACK_ID "pitel_instant_replay"
#define SR_CAPTURE_SETTING_DISK_RECORDING "disk_recording"

/* Settings key on a playback source naming the camera source it captures
 * from. Shared with the dock, which needs it to route a saved replay file
 * back to the playback source/scene for the camera it came from. */
#define S_CAPTURE_SOURCE "capture_source"

/* Per-camera replay timing correction. Positive means the camera signal
 * arrives late relative to the global/master timeline, so replay selects a
 * later camera-media timestamp (global + offset). */
#define S_SYNC_OFFSET_MS "sync_offset_ms"
#define SR_CAMERA_SYNC_MAX_MS 5000

/* Returns the ring buffer of a capture filter instance (obs_obj_get_data
 * of a source whose id is SR_CAPTURE_ID). Used by the playback source to
 * take replay snapshots. */
struct sr_buffer *sr_capture_get_buffer(void *capture_data);

/* The GPU capture render callback runs outside libobs filter callbacks. Parent
 * source lifetime therefore stays internal to capture-filter.c: it caches a
 * weak parent reference from valid filter callbacks and resolves a short-lived
 * strong reference only while rendering/encoding. Callers of this header must
 * not retain filter parent/target pointers. */

/* Aggregate health of all capture filters. This is intentionally a snapshot:
 * the dock uses it for the vMix-style global recording controls without
 * touching writer objects owned by the video callbacks. */
struct sr_capture_recording_summary {
	size_t camera_count;
	size_t requested_count;
	size_t active_count;
	size_t failed_count;
	size_t reserve_blocked_count;
	uint64_t packets_written;
	uint64_t bytes_written;
};

enum sr_capture_performance_path {
	SR_CAPTURE_PERF_WAITING = 0,
	SR_CAPTURE_PERF_GPU_D3D11,
	SR_CAPTURE_PERF_CPU,
	SR_CAPTURE_PERF_ERROR,
};

enum sr_capture_gpu_fallback_reason {
	SR_CAPTURE_GPU_FALLBACK_NONE = 0,
	SR_CAPTURE_GPU_FALLBACK_CREATE_FAILED,
	SR_CAPTURE_GPU_FALLBACK_RUNTIME_FAILED,
};

#define SR_CAPTURE_PERF_CAMERA_NAME_MAX 256
#define SR_CAPTURE_PERF_ENCODER_NAME_MAX 64

/* Last callback-published performance state for one capture filter. The values
 * are diagnostic rather than benchmark-grade: encode_time_* measures the CPU
 * submission/callback cost, not asynchronous GPU completion latency. */
struct sr_capture_performance_entry {
	char camera_name[SR_CAPTURE_PERF_CAMERA_NAME_MAX];
	char encoder_name[SR_CAPTURE_PERF_ENCODER_NAME_MAX];
	enum sr_capture_performance_path path;
	enum sr_capture_gpu_fallback_reason gpu_fallback_reason;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t gop_ms;
	int qp;
	bool disk_requested;
	bool writer_active;
	bool reserve_blocked;
	bool writer_failed;
	bool encoder_failed;
	uint64_t packets_written;
	uint64_t bytes_written;
	uint64_t packets_dropped;
	uint64_t segments_finalized;
	size_t queue_depth;
	size_t queue_high_watermark;
	uint64_t ram_bytes;
	uint64_t encode_calls;
	uint64_t encode_time_ns_total;
	uint64_t encode_time_ns_last;
};

struct sr_capture_performance_snapshot {
	struct sr_capture_performance_entry *entries;
	size_t count;
};

/* Enables/disables continuous disk recording on every Pitel capture filter by
 * updating the persistent OBS filter setting. Returns false only if source
 * enumeration failed; a successful call may still report zero cameras. */
bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count);

/* Reads the last video-thread-published recorder state for every capture
 * filter. Safe to call from the Qt frontend thread. */
bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary);

/* Captures one frontend-thread-safe diagnostic row per Pitel capture filter.
 * Producer callbacks refresh the underlying snapshot at roughly 500 ms while
 * video is flowing; the UI only copies that published state and never waits on
 * the encoder/writer render-path mutex. Rows are returned in the current OBS
 * source-enumeration order and their storage is valid only until the matching
 * free call. The caller owns snapshot->entries and must release it with
 * sr_capture_free_performance_snapshot(). */
bool sr_capture_get_performance_snapshot(struct sr_capture_performance_snapshot *snapshot);
void sr_capture_free_performance_snapshot(struct sr_capture_performance_snapshot *snapshot);

/* Loads a saved replay file into the given Pitel Instant Replay playback source and
 * starts playing it (with the same controls as a live replay). Used by the
 * dock's double-click action. No-op if source is not a Pitel Instant Replay source. */
void sr_playback_play_file(obs_source_t *source, const char *path);

#ifdef __cplusplus
}
#endif
