/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-setup.h"

#include "sr-capture.h"
#include "sr-event-output.h"
#include "sr-program-recorder.h"
#include "sr-scene-tracker.h"

#include <graphics/vec2.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <stdio.h>
#include <string.h>

static const char *canonical_scene_name(enum sr_replay_bus bus)
{
	return bus == SR_REPLAY_BUS_B ? SR_REPLAY_SETUP_SCENE_B : SR_REPLAY_SETUP_SCENE_A;
}

static const char *canonical_output_name(enum sr_replay_bus bus)
{
	return bus == SR_REPLAY_BUS_B ? SR_REPLAY_SETUP_OUTPUT_B : SR_REPLAY_SETUP_OUTPUT_A;
}

static void copy_name(char *dst, size_t dst_size, const char *src)
{
	if (!dst || !dst_size)
		return;
	dst[0] = '\0';
	if (!src)
		return;
	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static bool source_is_event_output_bus(obs_source_t *source, enum sr_replay_bus bus)
{
	if (!source || strcmp(obs_source_get_unversioned_id(source), SR_EVENT_OUTPUT_ID) != 0)
		return false;
	obs_data_t *settings = obs_source_get_settings(source);
	const int configured_bus = settings ? (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_BUS) : -1;
	obs_data_release(settings);
	return configured_bus == (int)bus;
}

struct scene_bus_find_ctx {
	enum sr_replay_bus bus;
	obs_source_t *source;
};

static bool find_bus_scene_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct scene_bus_find_ctx *ctx = param;
	if (!ctx || ctx->source)
		return false;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source_is_event_output_bus(source, ctx->bus)) {
		ctx->source = obs_source_get_ref(source);
		return false;
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, find_bus_scene_item, ctx);
	return ctx->source == NULL;
}

static obs_source_t *find_output_in_scene_source(obs_source_t *scene_source, enum sr_replay_bus bus)
{
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : NULL;
	if (!scene)
		return NULL;
	struct scene_bus_find_ctx ctx = {.bus = bus};
	obs_scene_enum_items(scene, find_bus_scene_item, &ctx);
	return ctx.source;
}

struct global_output_ctx {
	enum sr_replay_bus bus;
	obs_source_t *source;
};

static bool find_global_output(void *param, obs_source_t *source)
{
	struct global_output_ctx *ctx = param;
	if (!ctx || ctx->source)
		return false;
	if (!source_is_event_output_bus(source, ctx->bus))
		return true;
	ctx->source = obs_source_get_ref(source);
	return false;
}

static obs_source_t *find_any_output(enum sr_replay_bus bus)
{
	/* Prefer the canonical source name if it already exists. */
	obs_source_t *named = obs_get_source_by_name(canonical_output_name(bus));
	if (named) {
		if (source_is_event_output_bus(named, bus))
			return named;
		obs_source_release(named);
	}

	struct global_output_ctx ctx = {.bus = bus};
	obs_enum_sources(find_global_output, &ctx);
	return ctx.source;
}

char *sr_replay_setup_find_output_source_name(enum sr_replay_bus bus)
{
	if (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B)
		return NULL;

	/* A canonical Setup scene is deterministic even if an older Event Output
	 * with the same bus is still referenced by another user scene. */
	obs_source_t *canonical = obs_get_source_by_name(canonical_scene_name(bus));
	if (canonical) {
		obs_source_t *output = find_output_in_scene_source(canonical, bus);
		obs_source_release(canonical);
		if (output) {
			char *name = bstrdup(obs_source_get_name(output));
			obs_source_release(output);
			return name;
		}
	}

	obs_source_t *output = find_any_output(bus);
	if (!output)
		return NULL;
	char *name = bstrdup(obs_source_get_name(output));
	obs_source_release(output);
	return name;
}

char *sr_replay_setup_find_output_scene_name(enum sr_replay_bus bus)
{
	if (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B)
		return NULL;

	obs_source_t *canonical = obs_get_source_by_name(canonical_scene_name(bus));
	if (canonical) {
		obs_source_t *output = find_output_in_scene_source(canonical, bus);
		if (output) {
			char *name = bstrdup(obs_source_get_name(canonical));
			obs_source_release(output);
			obs_source_release(canonical);
			return name;
		}
		obs_source_release(canonical);
	}

	char *source_name = sr_replay_setup_find_output_source_name(bus);
	if (!source_name)
		return NULL;
	char *scene_name = sr_find_scene_with_source(source_name);
	bfree(source_name);
	return scene_name;
}

struct capture_filter_ctx {
	obs_source_t *filter;
	size_t count;
};

static void find_capture_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct capture_filter_ctx *ctx = param;
	if (!ctx || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;
	ctx->count++;
	if (!ctx->filter)
		ctx->filter = obs_source_get_ref(child);
}

static obs_source_t *get_capture_filter(obs_source_t *source, size_t *count)
{
	struct capture_filter_ctx ctx = {0};
	if (source)
		obs_source_enum_filters(source, find_capture_filter, &ctx);
	if (count)
		*count = ctx.count;
	return ctx.filter;
}

static bool source_is_candidate(obs_source_t *source, bool has_capture, bool *compatible)
{
	if (compatible)
		*compatible = false;
	if (!source || obs_obj_is_private(source))
		return false;
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT)
		return false;

	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, SR_EVENT_OUTPUT_ID) == 0 || strcmp(id, "pitel_instant_replay") == 0 || strcmp(id, "scene") == 0 ||
	    strcmp(id, "group") == 0)
		return false;

	const uint32_t flags = obs_source_get_output_flags(source);
	const bool async_video = (flags & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
	if (compatible)
		*compatible = async_video;
	return async_video || has_capture;
}

struct setup_snapshot_ctx {
	struct sr_replay_setup_snapshot *snapshot;
};

static bool collect_setup_source(void *param, obs_source_t *source)
{
	struct setup_snapshot_ctx *ctx = param;
	if (!ctx || !ctx->snapshot)
		return false;

	size_t filter_count = 0;
	obs_source_t *filter = get_capture_filter(source, &filter_count);
	const bool has_capture = filter_count > 0;
	bool compatible = false;
	if (!source_is_candidate(source, has_capture, &compatible)) {
		obs_source_release(filter);
		return true;
	}

	const size_t next_count = ctx->snapshot->source_count + 1;
	struct sr_replay_setup_source *entries =
		brealloc(ctx->snapshot->sources, next_count * sizeof(*ctx->snapshot->sources));
	if (!entries) {
		obs_source_release(filter);
		return true;
	}
	ctx->snapshot->sources = entries;
	struct sr_replay_setup_source *entry = &entries[ctx->snapshot->source_count];
	memset(entry, 0, sizeof(*entry));
	copy_name(entry->name, sizeof(entry->name), obs_source_get_name(source));
	copy_name(entry->type_id, sizeof(entry->type_id), obs_source_get_unversioned_id(source));
	entry->compatible = compatible;
	entry->has_capture = has_capture;
	entry->capture_enabled = filter ? obs_source_enabled(filter) : false;
	ctx->snapshot->source_count = next_count;
	if (compatible)
		ctx->snapshot->compatible_source_count++;
	if (has_capture)
		ctx->snapshot->capture_source_count++;
	if (has_capture && entry->capture_enabled)
		ctx->snapshot->enabled_capture_source_count++;
	obs_source_release(filter);
	return true;
}

bool sr_replay_setup_get_snapshot(struct sr_replay_setup_snapshot *snapshot)
{
	if (!snapshot)
		return false;
	memset(snapshot, 0, sizeof(*snapshot));
	struct setup_snapshot_ctx ctx = {.snapshot = snapshot};
	obs_enum_sources(collect_setup_source, &ctx);
	snapshot->program_output_supported = sr_program_recorder_supported();
	snapshot->program_output_enabled = sr_program_recorder_selected();

	char *scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_B);
	copy_name(snapshot->scene_a, sizeof(snapshot->scene_a), scene_a);
	copy_name(snapshot->scene_b, sizeof(snapshot->scene_b), scene_b);
	snapshot->bus_a_ready = scene_a && *scene_a;
	snapshot->bus_b_ready = scene_b && *scene_b;
	snapshot->event_transition_ready = snapshot->bus_a_ready && snapshot->bus_b_ready &&
					   strcmp(scene_a, scene_b) != 0;
	bfree(scene_a);
	bfree(scene_b);
	return true;
}

void sr_replay_setup_free_snapshot(struct sr_replay_setup_snapshot *snapshot)
{
	if (!snapshot)
		return;
	bfree(snapshot->sources);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void disable_capture_before_remove(obs_source_t *filter)
{
	if (!filter)
		return;
	obs_data_t *settings = obs_source_get_settings(filter);
	if (settings) {
		obs_data_set_bool(settings, SR_CAPTURE_SETTING_DISK_RECORDING, false);
		obs_source_update(filter, settings);
		obs_data_release(settings);
	}
}

bool sr_replay_setup_set_capture(const char *source_name, bool enabled)
{
	if (!source_name || !*source_name)
		return false;
	obs_source_t *source = obs_get_source_by_name(source_name);
	if (!source)
		return false;

	size_t count = 0;
	obs_source_t *filter = get_capture_filter(source, &count);
	if (enabled) {
		bool compatible = false;
		if (!source_is_candidate(source, count > 0, &compatible) || !compatible) {
			obs_source_release(filter);
			obs_source_release(source);
			return false;
		}
		if (filter) {
			/* A disabled filter can retain an old recording intent if it was
			 * disabled outside the dock. Clear that intent before enabling it so
			 * Replay Setup never starts disk recording implicitly. */
			disable_capture_before_remove(filter);
			obs_source_set_enabled(filter, true);
			obs_source_release(filter);
			obs_source_release(source);
			obs_frontend_save();
			return true;
		}

		char filter_name[SR_REPLAY_SETUP_NAME_MAX];
		copy_name(filter_name, sizeof(filter_name), "Pitel Instant Replay Capture");
		for (unsigned suffix = 2;; suffix++) {
			obs_source_t *collision = obs_source_get_filter_by_name(source, filter_name);
			if (!collision)
				break;
			obs_source_release(collision);
			snprintf(filter_name, sizeof(filter_name), "Pitel Instant Replay Capture %u", suffix);
		}
		obs_source_t *created = obs_source_create(SR_CAPTURE_ID, filter_name, NULL, NULL);
		if (!created) {
			obs_source_release(source);
			return false;
		}
		obs_source_filter_add(source, created);
		obs_source_set_enabled(created, true);
		obs_source_release(created);
		obs_source_release(source);
		obs_frontend_save();
		return true;
	}

	/* Remove every Pitel Capture filter on this source. This also repairs an
	 * accidental duplicate-filter setup without touching unrelated filters. */
	while (filter) {
		disable_capture_before_remove(filter);
		obs_source_filter_remove(source, filter);
		obs_source_release(filter);
		filter = get_capture_filter(source, NULL);
	}
	obs_source_release(source);
	obs_frontend_save();
	return true;
}

bool sr_replay_setup_set_program_output(bool enabled)
{
	if (enabled && !sr_program_recorder_supported())
		return false;
	sr_program_recorder_set_selected(enabled);
	obs_frontend_save();
	return sr_program_recorder_selected() == enabled;
}

static obs_source_t *get_or_create_scene_source(const char *base_name, bool *created)
{
	if (created)
		*created = false;
	char name[SR_REPLAY_SETUP_NAME_MAX];
	copy_name(name, sizeof(name), base_name);
	for (unsigned suffix = 2;; suffix++) {
		obs_source_t *existing = obs_get_source_by_name(name);
		if (!existing)
			break;
		if (obs_scene_from_source(existing))
			return existing;
		obs_source_release(existing);
		snprintf(name, sizeof(name), "%s %u", base_name, suffix);
	}

	obs_scene_t *scene = obs_scene_create(name);
	if (!scene)
		return NULL;
	obs_source_t *scene_source = obs_source_get_ref(obs_scene_get_source(scene));
	obs_scene_release(scene);
	if (created)
		*created = scene_source != NULL;
	return scene_source;
}

static void fit_scene_item_to_canvas(obs_sceneitem_t *item)
{
	if (!item)
		return;
	struct obs_video_info video = {0};
	if (!obs_get_video_info(&video) || !video.base_width || !video.base_height)
		return;
	struct vec2 position = {0.0f, 0.0f};
	struct vec2 bounds = {(float)video.base_width, (float)video.base_height};
	obs_sceneitem_set_pos(item, &position);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
}

static bool ensure_output_in_scene(enum sr_replay_bus bus, const char *scene_base, bool *created_scene,
				   bool *added_output)
{
	if (created_scene)
		*created_scene = false;
	if (added_output)
		*added_output = false;

	obs_source_t *scene_source = get_or_create_scene_source(scene_base, created_scene);
	if (!scene_source)
		return false;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene) {
		obs_source_release(scene_source);
		return false;
	}

	obs_source_t *already = find_output_in_scene_source(scene_source, bus);
	if (already) {
		obs_source_release(already);
		obs_source_release(scene_source);
		return true;
	}

	/* Re-use an existing Event Output when possible. One OBS source can be
	 * referenced by multiple scenes; canonical scene preference then makes the
	 * A/B routing deterministic without deleting the user's old scene item. */
	obs_source_t *output = find_any_output(bus);
	bool created_output = false;
	if (!output) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, SR_EVENT_OUTPUT_SETTING_BUS, bus);
		char name[SR_REPLAY_SETUP_NAME_MAX];
		copy_name(name, sizeof(name), canonical_output_name(bus));
		for (unsigned suffix = 2;; suffix++) {
			obs_source_t *collision = obs_get_source_by_name(name);
			if (!collision)
				break;
			obs_source_release(collision);
			snprintf(name, sizeof(name), "%s %u", canonical_output_name(bus), suffix);
		}
		output = obs_source_create(SR_EVENT_OUTPUT_ID, name, settings, NULL);
		obs_data_release(settings);
		created_output = output != NULL;
	}
	if (!output) {
		obs_source_release(scene_source);
		return false;
	}

	obs_sceneitem_t *item = obs_scene_add(scene, output);
	if (item)
		fit_scene_item_to_canvas(item);
	obs_source_release(output);
	obs_source_release(scene_source);
	if (!item)
		return false;
	if (added_output)
		*added_output = true;
	UNUSED_PARAMETER(created_output);
	return true;
}

bool sr_replay_setup_ensure_event_scenes(struct sr_replay_setup_result *result)
{
	struct sr_replay_setup_result local = {0};
	char *scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_B);

	if (!scene_a || !*scene_a) {
		if (!ensure_output_in_scene(SR_REPLAY_BUS_A, SR_REPLAY_SETUP_SCENE_A, &local.created_scene_a,
					    &local.added_output_a))
			goto fail;
		local.changed = local.created_scene_a || local.added_output_a;
		bfree(scene_a);
		scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);
	}
	if (!scene_b || !*scene_b) {
		if (!ensure_output_in_scene(SR_REPLAY_BUS_B, SR_REPLAY_SETUP_SCENE_B, &local.created_scene_b,
					    &local.added_output_b))
			goto fail;
		local.changed = local.changed || local.created_scene_b || local.added_output_b;
		bfree(scene_b);
		scene_b = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_B);
	}

	/* Both buses can exist but still be unusable for a transition when they are
	 * in the same scene. Preserve that scene and add only one canonical side. */
	if (scene_a && scene_b && *scene_a && *scene_b && strcmp(scene_a, scene_b) == 0) {
		const bool shared_is_b = strcmp(scene_b, SR_REPLAY_SETUP_SCENE_B) == 0;
		if (shared_is_b) {
			if (!ensure_output_in_scene(SR_REPLAY_BUS_A, SR_REPLAY_SETUP_SCENE_A, &local.created_scene_a,
						    &local.added_output_a))
				goto fail;
			local.changed = local.changed || local.created_scene_a || local.added_output_a;
		} else {
			if (!ensure_output_in_scene(SR_REPLAY_BUS_B, SR_REPLAY_SETUP_SCENE_B, &local.created_scene_b,
						    &local.added_output_b))
				goto fail;
			local.changed = local.changed || local.created_scene_b || local.added_output_b;
		}
		bfree(scene_a);
		bfree(scene_b);
		scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);
		scene_b = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_B);
	}

	copy_name(local.scene_a, sizeof(local.scene_a), scene_a);
	copy_name(local.scene_b, sizeof(local.scene_b), scene_b);
	local.event_transition_ready = scene_a && *scene_a && scene_b && *scene_b && strcmp(scene_a, scene_b) != 0;
	bfree(scene_a);
	bfree(scene_b);
	if (local.changed)
		obs_frontend_save();
	if (result)
		*result = local;
	return local.event_transition_ready;

fail:
	bfree(scene_a);
	bfree(scene_b);
	if (result)
		*result = local;
	return false;
}
