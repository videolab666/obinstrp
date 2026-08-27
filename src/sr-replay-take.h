/*
Pitel Instant Replay
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

/* Starts the selected bus, marks its Event played and switches program to the
 * scene containing a Pitel Instant Replay Event Output source configured for
 * that bus. The first live -> replay switch may use the configured TAKE IN
 * Stinger. The scene must already exist in the OBS scene collection. */
bool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus);

/* UI-thread readiness check for vMix-style Event Transitions. True only when
 * A and B Event Output sources are both present in two distinct OBS scenes,
 * so an Event/angle can be pre-cued on the opposite bus and transitioned to. */
bool sr_replay_take_event_transition_ready(void);

/* Called by the sequence engine after it has pre-cued the next Event/angle on
 * the opposite bus. The actual start + scene switch is queued on the OBS UI
 * thread and uses the configured Event Transition/duration, never TAKE IN. */
void sr_replay_take_advance_event_async(struct sr_event_controller *events, enum sr_replay_bus bus);

/* Returns the replay bus currently on Program. Outside a replay scene,
 * falls back to A when it is cued, otherwise B. Used by hardware angle
 * hotkeys so one CAM1..CAM8 bank follows the active replay transport. */
bool sr_replay_take_current_bus(enum sr_replay_bus *bus);

/* Returns true only when an Event Output scene is actually on Program. Unlike
 * sr_replay_take_current_bus(), this never falls back to a merely cued bus. */
bool sr_replay_take_program_bus(enum sr_replay_bus *bus);

/* If A is on program, TAKE B; if B is on program, TAKE A. Outside either
 * replay scene, prefer A when cued, otherwise B. */
bool sr_replay_take_toggle(struct sr_event_controller *events);

/* Explicit operator return from either replay bus to the program scene that
 * was live before replay. Uses the configured OUT native Stinger when set. */
bool sr_replay_take_return(struct sr_event_controller *events);

/* Queues an automatic return on the OBS UI thread after an Event reaches
 * OUT. The request is ignored if another Event/bus has taken over before the
 * task runs. Event List/angle playback calls this only after its final item. */
void sr_replay_take_return_on_end(enum sr_replay_bus bus, uint64_t event_id);

/* Called by an Event Output after it has actually left Program. The bus check
 * prevents A deactivation from restoring live audio after an A -> B TAKE. */
void sr_replay_take_release_live_audio(enum sr_replay_bus bus);

/* Releases process-local TAKE state during module shutdown. */
void sr_replay_take_reset(void);

#ifdef __cplusplus
}
#endif
