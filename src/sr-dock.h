/*
 * Pitel Instant Replay - OBS dock interface
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

void sr_dock_register(struct sr_event_controller *controller);
void sr_dock_open_settings(void);

#ifdef __cplusplus
}
#endif
