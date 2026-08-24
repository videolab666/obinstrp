/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

struct sr_event_controller;

#ifdef __cplusplus
class QWidget;

/* Creates the metadata-based operator surface for embedding in the unified
 * Sports Replay dock. The controller is owned by plugin-main and must outlive
 * the returned widget. */
QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent = nullptr);
#endif
