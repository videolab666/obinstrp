/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-capture.h"
#include "sr-master-audio.h"
#include "sr-session.h"
#include "sr-storage-manager.h"

#include <obs-module.h>
#include <util/platform.h>

/* capture-filter.c is compiled with these two public symbols renamed. Keeping
 * the stable recorder implementation untouched is intentional: this adapter
 * owns Session Manager policy while the existing render/encode callbacks keep
 * their proven Intel/NVIDIA locking and fallback behavior. */
bool sr_capture_set_all_disk_recording_impl(bool enabled, size_t *camera_count);
bool sr_capture_get_recording_summary_impl(struct sr_capture_recording_summary *summary);

static bool wait_for_recording_producers(uint32_t timeout_ms)
{
	const uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	for (;;) {
		struct sr_capture_recording_summary summary = {0};
		if (sr_capture_get_recording_summary_impl(&summary) && summary.active_count == 0)
			break;
		if (os_gettime_ns() >= deadline)
			return false;
		os_sleep_ms(5);
	}

	const uint64_t now = os_gettime_ns();
	const uint32_t remaining_ms = now >= deadline ? 0 : (uint32_t)((deadline - now) / 1000000ULL);
	return sr_master_audio_wait_idle(remaining_ms);
}

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)
{
	size_t local_count = 0;
	size_t *count = camera_count ? camera_count : &local_count;
	if (enabled) {
		if (!sr_storage_manager_wait_initial_recovery(10000)) {
			blog(LOG_WARNING,
			     "Pitel Instant Replay: START REC deferred because crash recovery is still validating previous sessions");
			return false;
		}
		if (!sr_session_prepare_recording(obs_get_video_frame_time()))
			return false;
		const bool ok = sr_capture_set_all_disk_recording_impl(true, count);
		if (!ok || *count == 0) {
			size_t ignored = 0;
			sr_capture_set_all_disk_recording_impl(false, &ignored);
			if (wait_for_recording_producers(3000))
				sr_session_finish_recording(obs_get_video_frame_time());
			else
				blog(LOG_ERROR,
				     "Pitel Instant Replay: recorder rollback did not quiesce; session remains locked to prevent cross-session timestamp corruption");
		}
		return ok;
	}

	const bool ok = sr_capture_set_all_disk_recording_impl(false, count);
	if (!ok)
		return false;
	if (!wait_for_recording_producers(3000)) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: STOP timed out waiting for video/audio writers; session remains active and cannot be switched safely");
		return false;
	}
	sr_session_finish_recording(obs_get_video_frame_time());
	return true;
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
