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

enum sr_event_export_error {
	SR_EVENT_EXPORT_OK = 0,
	SR_EVENT_EXPORT_INVALID_ARGUMENT,
	SR_EVENT_EXPORT_DESTINATION_EXISTS,
	SR_EVENT_EXPORT_NO_VIDEO,
	SR_EVENT_EXPORT_UNSUPPORTED_CHANGE,
	SR_EVENT_EXPORT_OPEN_FAILED,
	SR_EVENT_EXPORT_WRITE_FAILED,
	SR_EVENT_EXPORT_CANCELLED,
};

struct sr_event_export_spec {
	const char *session_dir;
	const char *camera_name;
	const char *output_path;
	uint64_t event_in_ns;
	uint64_t event_out_ns;
	int64_t camera_sync_offset_ns;
	bool include_master_audio;
};

struct sr_event_export_result {
	enum sr_event_export_error error;
	size_t video_packets;
	size_t audio_packets;
	bool audio_included;
};

typedef bool (*sr_event_export_cancel_cb)(void *data);
typedef void (*sr_event_export_progress_cb)(void *data, unsigned percent);

/* Synchronous core operation. Call from a worker thread: catalog scans,
 * packet reads and MP4 writes intentionally never run on Qt or OBS realtime
 * callbacks. The destination is published only after a successful trailer;
 * output_path.part is removed on failure/cancel. Existing destinations are
 * never overwritten. */
bool sr_event_export_fast(const struct sr_event_export_spec *spec, sr_event_export_cancel_cb should_cancel,
			  sr_event_export_progress_cb progress, void *callback_data,
			  struct sr_event_export_result *result);

const char *sr_event_export_error_text(enum sr_event_export_error error);

#ifdef __cplusplus
}
#endif
