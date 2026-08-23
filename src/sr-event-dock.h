/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

/* Registers the metadata-based replay Event operator dock. The controller is
 * owned by plugin-main and must outlive the dock. */
void sr_event_dock_register(struct sr_event_controller *controller);

#ifdef __cplusplus
}
#endif
