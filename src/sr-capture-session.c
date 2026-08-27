/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-capture.h"
#include "sr-session.h"

#include <obs-module.h>

/* capture-filter.c is compiled with these two public symbols renamed. Keeping
 * the stable recorder implementation untouched is intentional: this adapter
 * owns Session Manager policy while the existing render/encode callbacks keep
 * their proven Intel/NVIDIA locking and fallback behavior. */
bool sr_capture_set_all_disk_recording_impl(bool enabled, size_t *camera_count);
bool sr_capture_get_recording_summary_impl(struct sr_capture_recording_summary *summary);

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)
{
	if (enabled) {
		if (!sr_session_prepare_recording(obs_get_video_frame_time()))
			return false;
		const bool ok = sr_capture_set_all_disk_recording_impl(true, camera_count);
		if (!ok || (camera_count && *camera_count == 0)) {
			size_t ignored = 0;
			sr_capture_set_all_disk_recording_impl(false, &ignored);
			sr_session_finish_recording(obs_get_video_frame_time());
		}
		return ok;
	}

	const bool ok = sr_capture_set_all_disk_recording_impl(false, camera_count);
	/* Settings updates are synchronous; individual writers drain packets whose
	 * timestamps were already mapped when queued. Closing the run here makes a
	 * subsequent START create a clean discontinuity and compact timeline. */
	sr_session_finish_recording(obs_get_video_frame_time());
	return ok;
}

bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary)
{
	if (!sr_capture_get_recording_summary_impl(summary))
		return false;
	if (!summary || !summary->requested_count || !sr_session_recording_is_active())
		return true;

	const uint64_t start = sr_session_recording_start_ns();
	const uint64_t now = sr_session_map_recording_timestamp(obs_get_video_frame_time());
	summary->recording_start_ns = start;
	summary->recording_duration_ns = now >= start ? now - start : 0;
	return true;
}
