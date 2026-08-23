/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-take.h"

#include "sr-event-controller.h"
#include "sr-event-output.h"
#include "sr-scene-tracker.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <string.h>

struct find_output_ctx {
	enum sr_replay_bus bus;
	char *source_name;
};

static bool find_output_source(void *param, obs_source_t *source)
{
	struct find_output_ctx *ctx = param;
	if (!ctx || ctx->source_name)
		return false;
	if (strcmp(obs_source_get_unversioned_id(source), SR_EVENT_OUTPUT_ID) != 0)
		return true;

	obs_data_t *settings = obs_source_get_settings(source);
	const int bus = (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_BUS);
	obs_data_release(settings);
	if (bus != (int)ctx->bus)
		return true;

	ctx->source_name = bstrdup(obs_source_get_name(source));
	return false;
}

static char *output_source_name(enum sr_replay_bus bus)
{
	struct find_output_ctx ctx = {.bus = bus};
	obs_enum_sources(find_output_source, &ctx);
	return ctx.source_name;
}

static char *output_scene_name(enum sr_replay_bus bus)
{
	char *source_name = output_source_name(bus);
	if (!source_name)
		return NULL;
	char *scene_name = sr_find_scene_with_source(source_name);
	bfree(source_name);
	return scene_name;
}

bool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus)
{
	if (!events || (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B))
		return false;

	struct sr_replay_channel_state state;
	if (!sr_replay_channel_get_state(bus, &state) || !state.cued || !state.event_id)
		return false;

	char *scene_name = output_scene_name(bus);
	if (!scene_name) {
		blog(LOG_WARNING, "Sports Replay: TAKE %c failed: no scene contains an Event Output configured for bus %c",
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B', bus == SR_REPLAY_BUS_A ? 'A' : 'B');
		return false;
	}

	if (!sr_replay_channel_play(bus)) {
		bfree(scene_name);
		return false;
	}

	/* Arm the same Studio Mode preview guard used by the legacy replay path
	 * before program moves to the Event Output scene. */
	sr_scene_tracker_note_replay_launch();
	sr_switch_to_scene(scene_name);
	bfree(scene_name);

	if (!sr_event_controller_set_played(events, state.event_id, true))
		blog(LOG_WARNING, "Sports Replay: TAKE %c succeeded but Event %llu could not be marked played",
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B', (unsigned long long)state.event_id);
	return true;
}

bool sr_replay_take_toggle(struct sr_event_controller *events)
{
	if (!events)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);

	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;

	enum sr_replay_bus target = SR_REPLAY_BUS_A;
	if (current_name && scene_a && strcmp(current_name, scene_a) == 0)
		target = SR_REPLAY_BUS_B;
	else if (current_name && scene_b && strcmp(current_name, scene_b) == 0)
		target = SR_REPLAY_BUS_A;
	else {
		struct sr_replay_channel_state a;
		struct sr_replay_channel_state b;
		const bool have_a = sr_replay_channel_get_state(SR_REPLAY_BUS_A, &a) && a.cued;
		const bool have_b = sr_replay_channel_get_state(SR_REPLAY_BUS_B, &b) && b.cued;
		if (!have_a && have_b)
			target = SR_REPLAY_BUS_B;
	}

	if (current)
		obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	return sr_replay_take_bus(events, target);
}
