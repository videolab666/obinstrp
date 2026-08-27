/*
Pitel Instant Replay
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

/* Creates the synchronized multicamera EDIT monitor. The widget owns only
 * lightweight preview decoders; A/B remain the authoritative playout buses. */
QWidget *sr_multiview_dock_create(struct sr_event_controller *controller, QWidget *parent = nullptr);

/* Shows/raises the OBS dock if it has already been registered. */
void sr_multiview_dock_show(void);
#endif
