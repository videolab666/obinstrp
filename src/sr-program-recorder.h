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

#ifdef __cplusplus
extern "C" {
#endif

struct sr_capture_recording_summary;
struct sr_capture_performance_entry;

bool sr_program_recorder_init(void);
void sr_program_recorder_free(void);

/* Program recording is intentionally GPU-only for the first implementation:
 * Windows/D3D11 -> NVENC/AMF. Other platforms stay build-compatible and report
 * unsupported instead of silently introducing a full-frame CPU readback. */
bool sr_program_recorder_supported(void);

/* Persistent Replay Setup selection, independent from the current REC state. */
bool sr_program_recorder_selected(void);
void sr_program_recorder_set_selected(bool enabled);

/* Global REC transport intent. Resources are opened/closed by the rendered
 * callback so encoder/writer lifetime stays serialized with Program frames. */
bool sr_program_recorder_set_recording(bool enabled);
bool sr_program_recorder_recording_requested(void);

/* Merge one Program pseudo-source into the existing camera-recorder UI model. */
void sr_program_recorder_add_recording_summary(struct sr_capture_recording_summary *summary);
bool sr_program_recorder_get_performance_entry(struct sr_capture_performance_entry *entry);

#ifdef __cplusplus
}
#endif
