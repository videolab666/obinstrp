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

struct sr_event_controller;

#ifdef __cplusplus
class QWidget;

/* Creates the metadata-based operator surface for embedding in the unified
 * Pitel Instant Replay dock. The controller is owned by plugin-main and must outlive
 * the returned widget. */
QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent = nullptr);

/* Shared EDIT state used by Replay Multiview. The Event dock remains the
 * authoritative editor/controller: Multiview reads this snapshot and routes
 * edits back through the commands below, so there is never a second copy of
 * Event IN/OUT/angle state. All calls are UI-thread only. */
struct sr_event_editor_snapshot {
	bool available;
	bool edit_mode;
	bool playing;
	bool paused;
	bool loop;
	uint64_t event_id;
	uint64_t playhead_ns;
	uint64_t in_ns;
	uint64_t out_ns;
	uint64_t record_start_ns;
	uint64_t record_end_ns;
	char selected_camera[256]; /* saved Event angle; empty means AUTO */
	char preview_camera[256];  /* camera currently backing A/B EDIT preview */
};

bool sr_event_dock_get_editor_snapshot(struct sr_event_editor_snapshot *snapshot);
bool sr_event_dock_editor_seek(uint64_t timestamp_ns);
bool sr_event_dock_editor_set_range(uint64_t in_ns, uint64_t out_ns);
bool sr_event_dock_editor_set_marker(bool out_marker);
bool sr_event_dock_editor_goto_marker(bool out_marker);
bool sr_event_dock_editor_step_frames(int frames);
bool sr_event_dock_editor_select_camera(const char *camera_name); /* NULL/empty = AUTO */
bool sr_event_dock_editor_toggle_play(void);
bool sr_event_dock_editor_play_from_in(void);
bool sr_event_dock_editor_set_loop(bool enabled);
#endif
