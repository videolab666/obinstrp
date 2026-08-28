/*
 * Pitel Instant Replay - OBS capture bridge interface
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CAPTURE_ID "pitel_instant_replay_capture"
#define SR_CAPTURE_SETTING_DISK_RECORDING "disk_recording"
#define S_SYNC_OFFSET_MS "sync_offset_ms"
#define SR_CAMERA_SYNC_MAX_MS 5000

struct sr_capture_recording_summary {
	size_t camera_count;
	size_t requested_count;
	size_t active_count;
	size_t failed_count;
	size_t reserve_blocked_count;
	uint64_t packets_written;
	uint64_t bytes_written;
	uint64_t recording_start_ns;
	uint64_t recording_duration_ns;
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
	uint64_t encode_calls;
	uint64_t encode_time_ns_total;
	uint64_t encode_time_ns_last;
};

struct sr_capture_performance_snapshot {
	struct sr_capture_performance_entry *entries;
	size_t count;
};

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count);
bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary);
bool sr_capture_get_performance_snapshot(struct sr_capture_performance_snapshot *snapshot);
void sr_capture_free_performance_snapshot(struct sr_capture_performance_snapshot *snapshot);

#ifdef __cplusplus
}
#endif
