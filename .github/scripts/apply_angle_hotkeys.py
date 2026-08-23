from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"expected block not found in {path}: {old[:140]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


camera_list_h = r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_camera_list {
    char **names;
    size_t count;
};

/* Enumerates OBS parent sources that currently own a Sports Replay capture
 * filter. The returned list has deterministic strcmp ordering so Qt angle
 * buttons and hardware hotkeys use exactly the same CAM1..CAM8 mapping. */
bool sr_camera_list_capture(struct sr_camera_list *list);
void sr_camera_list_free(struct sr_camera_list *list);

#ifdef __cplusplus
}
#endif
'''

camera_list_c = r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-list.h"

#include "sr-capture.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <stdlib.h>
#include <string.h>

struct camera_list_builder {
    char **names;
    size_t count;
    size_t capacity;
    bool failed;
};

static bool contains_name(const struct camera_list_builder *builder, const char *name)
{
    for (size_t i = 0; i < builder->count; i++) {
        if (strcmp(builder->names[i], name) == 0)
            return true;
    }
    return false;
}

static void append_name(struct camera_list_builder *builder, const char *name)
{
    if (builder->failed || !name || !*name || contains_name(builder, name))
        return;

    if (builder->count == builder->capacity) {
        const size_t next_capacity = builder->capacity ? builder->capacity * 2 : 8;
        char **next = brealloc(builder->names, next_capacity * sizeof(*next));
        if (!next) {
            builder->failed = true;
            return;
        }
        builder->names = next;
        builder->capacity = next_capacity;
    }

    char *copy = bstrdup(name);
    if (!copy) {
        builder->failed = true;
        return;
    }
    builder->names[builder->count++] = copy;
}

static void enum_capture_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
    struct camera_list_builder *builder = param;
    if (!builder || builder->failed || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
        return;
    append_name(builder, obs_source_get_name(parent));
}

static bool enum_source(void *param, obs_source_t *source)
{
    struct camera_list_builder *builder = param;
    if (!builder || builder->failed)
        return false;
    obs_source_enum_filters(source, enum_capture_filter, builder);
    return !builder->failed;
}

static int compare_names(const void *a, const void *b)
{
    const char *const *lhs = a;
    const char *const *rhs = b;
    return strcmp(*lhs, *rhs);
}

bool sr_camera_list_capture(struct sr_camera_list *list)
{
    if (!list)
        return false;
    memset(list, 0, sizeof(*list));

    struct camera_list_builder builder = {0};
    obs_enum_sources(enum_source, &builder);
    if (builder.failed) {
        for (size_t i = 0; i < builder.count; i++)
            bfree(builder.names[i]);
        bfree(builder.names);
        return false;
    }

    if (builder.count > 1)
        qsort(builder.names, builder.count, sizeof(*builder.names), compare_names);
    list->names = builder.names;
    list->count = builder.count;
    return true;
}

void sr_camera_list_free(struct sr_camera_list *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        bfree(list->names[i]);
    bfree(list->names);
    memset(list, 0, sizeof(*list));
}
'''

Path("src/sr-camera-list.h").write_text(camera_list_h, encoding="utf-8")
Path("src/sr-camera-list.c").write_text(camera_list_c, encoding="utf-8")

cmake = Path("CMakeLists.txt")
replace_once(
    cmake,
    "    src/plugin-main.c\n    src/sr-buffer.c\n",
    "    src/plugin-main.c\n    src/sr-camera-list.c\n    src/sr-buffer.c\n",
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'Hotkey.ReturnLive="Replay Event: RETURN LIVE"\n',
    'Hotkey.ReturnLive="Replay Event: RETURN LIVE"\n'
    'Hotkey.Angle1="Replay Event: active bus angle 1"\n'
    'Hotkey.Angle2="Replay Event: active bus angle 2"\n'
    'Hotkey.Angle3="Replay Event: active bus angle 3"\n'
    'Hotkey.Angle4="Replay Event: active bus angle 4"\n'
    'Hotkey.Angle5="Replay Event: active bus angle 5"\n'
    'Hotkey.Angle6="Replay Event: active bus angle 6"\n'
    'Hotkey.Angle7="Replay Event: active bus angle 7"\n'
    'Hotkey.Angle8="Replay Event: active bus angle 8"\n',
)

# Keep dock camera ordering identical to angle hotkey ordering.
dock = Path("src/sr-event-dock.cpp")
replace_once(
    dock,
    '#include "sr-capture.h"\n#include "sr-event-controller.h"\n',
    '#include "sr-camera-list.h"\n#include "sr-capture.h"\n#include "sr-event-controller.h"\n',
)
old_camera_enum = r'''struct camera_enum_ctx {
	QStringList *names;
};

void enum_camera_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *ctx = static_cast<camera_enum_ctx *>(param);
	if (!ctx || !ctx->names || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;

	const QString name = QString::fromUtf8(obs_source_get_name(parent));
	if (!name.isEmpty() && !ctx->names->contains(name))
		ctx->names->append(name);
}

bool enum_camera_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, enum_camera_filter, param);
	return true;
}

QStringList captureCameraNames()
{
	QStringList names;
	camera_enum_ctx ctx;
	ctx.names = &names;
	obs_enum_sources(enum_camera_source, &ctx);
	names.sort(Qt::CaseInsensitive);
	return names;
}
'''
new_camera_enum = r'''QStringList captureCameraNames()
{
	QStringList names;
	sr_camera_list cameras = {};
	if (!sr_camera_list_capture(&cameras))
		return names;
	for (size_t i = 0; i < cameras.count; i++)
		names.append(QString::fromUtf8(cameras.names[i]));
	sr_camera_list_free(&cameras);
	return names;
}
'''
replace_once(dock, old_camera_enum, new_camera_enum)
replace_once(
    dock,
    '\t\t\tbutton->setText(QStringLiteral("%1 %2").arg(marker, camera));\n\t\t\tbutton->setToolTip(tooltip);\n',
    '\t\t\tbutton->setText(QStringLiteral("%1 %2").arg(marker, camera));\n\t\t\tbutton->setProperty("coverageTooltip", tooltip);\n\t\t\tbutton->setToolTip(tooltip);\n',
)
replace_once(
    dock,
    '''\t\t\tconst QString camera = button->property("cameraName").toString();\n\t\t\tconst bool atPlayhead = !sameEvent ||\n\t\t\t\t\t\t(state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);\n\t\t\tbutton->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);\n\t\t\tbutton->setChecked(sameEvent && activeCamera == camera);\n\t\t\tif (sameEvent && coverage != SR_REPLAY_COVERAGE_NONE && !atPlayhead)\n\t\t\t\tbutton->setToolTip(T("EventDock.AngleUnavailable").arg(camera));\n''',
    '''\t\t\tconst QString camera = button->property("cameraName").toString();\n\t\t\tconst bool atPlayhead = !sameEvent ||\n\t\t\t\t\t\t(state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);\n\t\t\tbutton->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);\n\t\t\tbutton->setChecked(sameEvent && activeCamera == camera);\n\t\t\tbutton->setToolTip(button->property("coverageTooltip").toString());\n\t\t\tif (sameEvent && coverage != SR_REPLAY_COVERAGE_NONE && !atPlayhead)\n\t\t\t\tbutton->setToolTip(T("EventDock.AngleUnavailable").arg(camera));\n''',
)

# Expose the bus that hardware angle hotkeys should control.
take_h = Path("src/sr-replay-take.h")
replace_once(
    take_h,
    '''/* If A is on program, TAKE B; if B is on program, TAKE A. Outside either
 * replay scene, prefer A when cued, otherwise B. */
bool sr_replay_take_toggle(struct sr_event_controller *events);
''',
    '''/* Returns the replay bus currently on Program. Outside a replay scene,
 * falls back to A when it is cued, otherwise B. Used by hardware angle
 * hotkeys so one CAM1..CAM8 bank follows the active replay transport. */
bool sr_replay_take_current_bus(enum sr_replay_bus *bus);

/* If A is on program, TAKE B; if B is on program, TAKE A. Outside either
 * replay scene, prefer A when cued, otherwise B. */
bool sr_replay_take_toggle(struct sr_event_controller *events);
''',
)

take_c = Path("src/sr-replay-take.c")
insert_before_toggle = r'''bool sr_replay_take_current_bus(enum sr_replay_bus *bus)
{
	if (!bus)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;

	bool found = false;
	if (current_name && scene_a && strcmp(current_name, scene_a) == 0) {
		*bus = SR_REPLAY_BUS_A;
		found = true;
	} else if (current_name && scene_b && strcmp(current_name, scene_b) == 0) {
		*bus = SR_REPLAY_BUS_B;
		found = true;
	} else {
		struct sr_replay_channel_state a = {0};
		struct sr_replay_channel_state b = {0};
		const bool have_a = sr_replay_channel_get_state(SR_REPLAY_BUS_A, &a) && a.cued;
		const bool have_b = sr_replay_channel_get_state(SR_REPLAY_BUS_B, &b) && b.cued;
		if (have_a) {
			*bus = SR_REPLAY_BUS_A;
			found = true;
		} else if (have_b) {
			*bus = SR_REPLAY_BUS_B;
			found = true;
		}
	}

	obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	return found;
}

'''
replace_once(take_c, 'bool sr_replay_take_toggle(struct sr_event_controller *events)\n{', insert_before_toggle + 'bool sr_replay_take_toggle(struct sr_event_controller *events)\n{')

# Add one CAM1..CAM8 hotkey bank that follows the active replay bus.
main = Path("src/plugin-main.c")
replace_once(
    main,
    '#include "sr-config.h"\n#include "sr-dock.h"\n',
    '#include "sr-camera-list.h"\n#include "sr-config.h"\n#include "sr-dock.h"\n',
)
replace_once(
    main,
    '#define NS_PER_SECOND 1000000000ULL\n',
    '#define NS_PER_SECOND 1000000000ULL\n#define SR_ANGLE_HOTKEY_COUNT 8u\n',
)
replace_once(
    main,
    'static obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;\n',
    'static obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;\nstatic obs_hotkey_id hk_angles[SR_ANGLE_HOTKEY_COUNT];\n',
)
angle_callback = r'''static void angle_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;

	const size_t index = (size_t)(uintptr_t)data;
	if (index >= SR_ANGLE_HOTKEY_COUNT)
		return;

	enum sr_replay_bus bus;
	if (!sr_replay_take_current_bus(&bus)) {
		obs_log(LOG_WARNING, "Sports Replay: angle %zu hotkey ignored: no replay bus is cued", index + 1);
		return;
	}

	struct sr_camera_list cameras = {0};
	if (!sr_camera_list_capture(&cameras)) {
		obs_log(LOG_WARNING, "Sports Replay: angle %zu hotkey could not enumerate replay cameras", index + 1);
		return;
	}
	if (index >= cameras.count) {
		obs_log(LOG_WARNING, "Sports Replay: angle %zu hotkey ignored: only %zu replay camera(s) are available",
			index + 1, cameras.count);
		sr_camera_list_free(&cameras);
		return;
	}

	const char *camera = cameras.names[index];
	if (!sr_replay_channel_switch_camera(bus, camera))
		obs_log(LOG_WARNING, "Sports Replay: bus %c could not switch to angle %zu ('%s') at the current playhead",
			bus == SR_REPLAY_BUS_A ? 'A' : 'B', index + 1, camera);
	sr_camera_list_free(&cameras);
}

'''
replace_once(main, 'static void register_event_hotkeys(void)\n{', angle_callback + 'static void register_event_hotkeys(void)\n{')
replace_once(
    main,
    '''\thk_return_live = obs_hotkey_register_frontend("SportsReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"),
\t\t\t\t\t\t      return_live_cb, NULL);\n''',
    '''\thk_return_live = obs_hotkey_register_frontend("SportsReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"),
\t\t\t\t\t\t      return_live_cb, NULL);\n\n\tstatic const char *const angle_ids[SR_ANGLE_HOTKEY_COUNT] = {\n\t\t"SportsReplay.Angle1", "SportsReplay.Angle2", "SportsReplay.Angle3", "SportsReplay.Angle4",\n\t\t"SportsReplay.Angle5", "SportsReplay.Angle6", "SportsReplay.Angle7", "SportsReplay.Angle8",\n\t};\n\tstatic const char *const angle_text[SR_ANGLE_HOTKEY_COUNT] = {\n\t\t"Hotkey.Angle1", "Hotkey.Angle2", "Hotkey.Angle3", "Hotkey.Angle4",\n\t\t"Hotkey.Angle5", "Hotkey.Angle6", "Hotkey.Angle7", "Hotkey.Angle8",\n\t};\n\tfor (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++)\n\t\thk_angles[i] = obs_hotkey_register_frontend(angle_ids[i], obs_module_text(angle_text[i]), angle_hotkey_cb,\n\t\t\t\t\t\t\t       (void *)(uintptr_t)i);\n''',
)
replace_once(
    main,
    '''\tif (hk_return_live != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_return_live);\n\n\thk_event_in = OBS_INVALID_HOTKEY_ID;\n''',
    '''\tif (hk_return_live != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_return_live);\n\tfor (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++) {\n\t\tif (hk_angles[i] != OBS_INVALID_HOTKEY_ID)\n\t\t\tobs_hotkey_unregister(hk_angles[i]);\n\t\thk_angles[i] = OBS_INVALID_HOTKEY_ID;\n\t}\n\n\thk_event_in = OBS_INVALID_HOTKEY_ID;\n''',
)

# Warm and validate a candidate camera before atomically swapping it on air.
channel = Path("src/sr-replay-channel.c")
replace_once(
    channel,
    '''\tuint64_t expected_event_id = 0;\n\tuint64_t event_in_ns = 0;\n\tuint64_t event_out_ns = 0;\n''',
    '''\tuint64_t expected_event_id = 0;\n\tuint64_t event_in_ns = 0;\n\tuint64_t event_out_ns = 0;\n\tuint64_t expected_playhead_ns = 0;\n''',
)
replace_once(
    channel,
    '''\texpected_event_id = channel->event_id;\n\tevent_in_ns = channel->event_in_ns;\n\tevent_out_ns = channel->event_out_ns;\n\tpthread_mutex_unlock(&channel->mutex);\n''',
    '''\texpected_event_id = channel->event_id;\n\tevent_in_ns = channel->event_in_ns;\n\tevent_out_ns = channel->event_out_ns;\n\texpected_playhead_ns = channel->playhead_ns;\n\tpthread_mutex_unlock(&channel->mutex);\n''',
)
replace_once(
    channel,
    '''\tconst uint64_t new_in_ns = event_in_ns < first_ns ? first_ns : event_in_ns;\n\tconst uint64_t new_out_ns = event_out_ns > last_ns ? last_ns : event_out_ns;\n\tchar *new_camera_name = bstrdup(camera_name);\n''',
    '''\tconst uint64_t new_in_ns = event_in_ns < first_ns ? first_ns : event_in_ns;\n\tconst uint64_t new_out_ns = event_out_ns > last_ns ? last_ns : event_out_ns;\n\tif (expected_playhead_ns < new_in_ns || expected_playhead_ns > new_out_ns) {\n\t\tsr_disk_player_destroy(new_player);\n\t\treturn false;\n\t}\n\n\t/* Bounds alone are insufficient when a camera dropped packets or has an\n\t * internal recording gap. Decode the current target before swapping the\n\t * player so an on-air angle button can never replace a good frame with a\n\t * camera that cannot actually produce this playhead. The decode also warms\n\t * the new player's GOP/frame cache for the first rendered frame. */\n\tAVFrame *probe_frame = NULL;\n\tif (!sr_disk_player_decode_at(new_player, expected_playhead_ns, &probe_frame, NULL) || !probe_frame) {\n\t\tav_frame_free(&probe_frame);\n\t\tsr_disk_player_destroy(new_player);\n\t\tblog(LOG_WARNING, "Sports Replay: rejected bus %c angle '%s': no decodable frame at %.3f s",\n\t\t     bus == SR_REPLAY_BUS_A ? 'A' : 'B', camera_name,\n\t\t     (double)(expected_playhead_ns - event_in_ns) / 1e9);\n\t\treturn false;\n\t}\n\tav_frame_free(&probe_frame);\n\n\tchar *new_camera_name = bstrdup(camera_name);\n''',
)

print("angle hotkeys and switch hardening applied")
