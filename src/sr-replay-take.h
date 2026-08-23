/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "sr-replay-channel.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

/* Starts the selected bus, marks its Event played and cuts program to the
 * scene containing a Sports Replay Event Output source configured for that
 * bus. The scene must already exist in the OBS scene collection. */
bool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus);

/* If A is on program, TAKE B; if B is on program, TAKE A. Outside either
 * replay scene, prefer A when cued, otherwise B. */
bool sr_replay_take_toggle(struct sr_event_controller *events);

#ifdef __cplusplus
}
#endif
