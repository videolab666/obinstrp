from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"expected block not found in {path}: {old[:180]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


playlist_h = r'''/*
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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

struct sr_replay_playlist_state {
    bool active;
    unsigned list_id;
    size_t position; /* zero based */
    size_t count;
    uint64_t event_id;
};

/* Treats one of the existing 20 ordered Event Lists as a highlight reel.
 * Media is never duplicated: the playlist snapshots only Event ids and cues
 * the existing disk-backed A/B channel for each item in order. */
bool sr_replay_playlist_init(struct sr_event_controller *events);
void sr_replay_playlist_shutdown(void);

/* Starts the list at its first currently playable, non-Pending Event and
 * starts that bus. preferred_camera may be NULL/empty; the selector prefers
 * the bus's current camera, then the requested camera, then another camera
 * with FULL coverage before falling back to PARTIAL coverage. */
bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera);

/* Operator skip and automatic end-of-Event advance. Both skip deleted,
 * Pending or currently unplayable Events. */
bool sr_replay_playlist_next(enum sr_replay_bus bus);
bool sr_replay_playlist_advance_on_end(enum sr_replay_bus bus);

/* Stops only automatic list advancement; it does not clear the currently
 * cued Event. */
void sr_replay_playlist_stop(enum sr_replay_bus bus);
bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state);

#ifdef __cplusplus
}
#endif
'''

playlist_c = r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-playlist.h"

#include "sr-camera-list.h"
#include "sr-event-controller.h"
#include "sr-replay-coverage.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <string.h>

struct sr_playlist_bus {
    bool active;
    unsigned list_id;
    uint64_t *event_ids;
    size_t count;
    size_t position;
    uint64_t event_id;
    char *preferred_camera;
};

static pthread_mutex_t g_mutex;
static bool g_started;
static struct sr_event_controller *g_events;
static struct sr_playlist_bus g_buses[SR_REPLAY_BUS_COUNT];

static struct sr_playlist_bus *get_bus(enum sr_replay_bus bus)
{
    return bus >= SR_REPLAY_BUS_A && bus < SR_REPLAY_BUS_COUNT ? &g_buses[bus] : NULL;
}

static void clear_bus_locked(struct sr_playlist_bus *bus)
{
    if (!bus)
        return;
    bfree(bus->event_ids);
    bfree(bus->preferred_camera);
    memset(bus, 0, sizeof(*bus));
}

static bool same_camera(const char *a, const char *b)
{
    return a && *a && b && *b && strcmp(a, b) == 0;
}

static bool try_camera(enum sr_replay_bus bus, uint64_t event_id, const struct sr_event_record *event,
                       const char *camera_name, enum sr_replay_coverage wanted)
{
    if (!camera_name || !*camera_name)
        return false;

    struct sr_replay_coverage_info coverage = {0};
    if (!sr_replay_coverage_query(camera_name, event->in_ns, event->out_ns, &coverage) || coverage.coverage != wanted)
        return false;
    return sr_replay_channel_cue(bus, event_id, camera_name);
}

static bool cue_best_camera_locked(enum sr_replay_bus bus, uint64_t event_id, const char *preferred_camera)
{
    struct sr_event_record event = {0};
    if (!sr_event_controller_get_event(g_events, event_id, &event))
        return false;
    if (event.pending) {
        sr_event_controller_free_event(&event);
        return false;
    }

    struct sr_replay_channel_state current = {0};
    const bool have_current = sr_replay_channel_get_state(bus, &current) && current.cued && current.camera_name[0];
    const char *current_camera = have_current ? current.camera_name : NULL;

    struct sr_camera_list cameras = {0};
    if (!sr_camera_list_capture(&cameras)) {
        sr_event_controller_free_event(&event);
        return false;
    }

    bool cued = false;
    const enum sr_replay_coverage passes[] = {SR_REPLAY_COVERAGE_FULL, SR_REPLAY_COVERAGE_PARTIAL};
    for (size_t pass = 0; pass < sizeof(passes) / sizeof(passes[0]) && !cued; pass++) {
        const enum sr_replay_coverage wanted = passes[pass];
        if (current_camera)
            cued = try_camera(bus, event_id, &event, current_camera, wanted);
        if (!cued && preferred_camera && *preferred_camera && !same_camera(preferred_camera, current_camera))
            cued = try_camera(bus, event_id, &event, preferred_camera, wanted);
        for (size_t i = 0; i < cameras.count && !cued; i++) {
            const char *candidate = cameras.names[i];
            if (same_camera(candidate, current_camera) || same_camera(candidate, preferred_camera))
                continue;
            cued = try_camera(bus, event_id, &event, candidate, wanted);
        }
    }

    sr_camera_list_free(&cameras);
    sr_event_controller_free_event(&event);
    return cued;
}

bool sr_replay_playlist_init(struct sr_event_controller *events)
{
    if (g_started)
        return true;
    if (!events)
        return false;
    if (pthread_mutex_init(&g_mutex, NULL) != 0)
        return false;
    memset(g_buses, 0, sizeof(g_buses));
    g_events = events;
    g_started = true;
    return true;
}

void sr_replay_playlist_shutdown(void)
{
    if (!g_started)
        return;
    pthread_mutex_lock(&g_mutex);
    for (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++)
        clear_bus_locked(&g_buses[i]);
    g_events = NULL;
    g_started = false;
    pthread_mutex_unlock(&g_mutex);
    pthread_mutex_destroy(&g_mutex);
}

bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera)
{
    struct sr_playlist_bus *playlist = get_bus(bus);
    if (!g_started || !playlist || !g_events)
        return false;

    uint64_t *event_ids = NULL;
    size_t count = 0;
    if (!sr_event_controller_get_list_events(g_events, list_id, &event_ids, &count) || !count) {
        bfree(event_ids);
        return false;
    }

    pthread_mutex_lock(&g_mutex);
    clear_bus_locked(playlist);

    size_t first = 0;
    bool cued = false;
    for (; first < count; first++) {
        if (cue_best_camera_locked(bus, event_ids[first], preferred_camera) && sr_replay_channel_play(bus)) {
            cued = true;
            break;
        }
    }

    if (!cued) {
        pthread_mutex_unlock(&g_mutex);
        bfree(event_ids);
        return false;
    }

    playlist->active = true;
    playlist->list_id = list_id;
    playlist->event_ids = event_ids;
    playlist->count = count;
    playlist->position = first;
    playlist->event_id = event_ids[first];
    playlist->preferred_camera = bstrdup(preferred_camera ? preferred_camera : "");
    pthread_mutex_unlock(&g_mutex);

    blog(LOG_INFO, "Sports Replay: started Event List %u highlight reel on bus %c at item %zu/%zu (Event %llu)",
         list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', first + 1, count, (unsigned long long)event_ids[first]);
    return true;
}

static bool advance_locked(enum sr_replay_bus bus, struct sr_playlist_bus *playlist)
{
    if (!playlist->active || playlist->position >= playlist->count)
        return false;

    struct sr_replay_channel_state current = {0};
    if (!sr_replay_channel_get_state(bus, &current) || !current.cued || current.event_id != playlist->event_id) {
        clear_bus_locked(playlist);
        return false;
    }

    for (size_t next = playlist->position + 1; next < playlist->count; next++) {
        if (!cue_best_camera_locked(bus, playlist->event_ids[next], playlist->preferred_camera))
            continue;
        if (!sr_replay_channel_play(bus))
            continue;

        playlist->position = next;
        playlist->event_id = playlist->event_ids[next];
        if (!sr_event_controller_set_played(g_events, playlist->event_id, true))
            blog(LOG_WARNING, "Sports Replay: playlist Event %llu could not be marked played",
                 (unsigned long long)playlist->event_id);
        blog(LOG_INFO, "Sports Replay: Event List %u advanced bus %c to item %zu/%zu (Event %llu)",
             playlist->list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', next + 1, playlist->count,
             (unsigned long long)playlist->event_id);
        return true;
    }

    blog(LOG_INFO, "Sports Replay: Event List %u finished on bus %c", playlist->list_id,
         bus == SR_REPLAY_BUS_A ? 'A' : 'B');
    clear_bus_locked(playlist);
    return false;
}

bool sr_replay_playlist_next(enum sr_replay_bus bus)
{
    struct sr_playlist_bus *playlist = get_bus(bus);
    if (!g_started || !playlist)
        return false;
    pthread_mutex_lock(&g_mutex);
    const bool advanced = advance_locked(bus, playlist);
    pthread_mutex_unlock(&g_mutex);
    return advanced;
}

bool sr_replay_playlist_advance_on_end(enum sr_replay_bus bus)
{
    return sr_replay_playlist_next(bus);
}

void sr_replay_playlist_stop(enum sr_replay_bus bus)
{
    struct sr_playlist_bus *playlist = get_bus(bus);
    if (!g_started || !playlist)
        return;
    pthread_mutex_lock(&g_mutex);
    clear_bus_locked(playlist);
    pthread_mutex_unlock(&g_mutex);
}

bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state)
{
    struct sr_playlist_bus *playlist = get_bus(bus);
    if (!g_started || !playlist || !state)
        return false;
    memset(state, 0, sizeof(*state));
    pthread_mutex_lock(&g_mutex);
    state->active = playlist->active;
    state->list_id = playlist->list_id;
    state->position = playlist->position;
    state->count = playlist->count;
    state->event_id = playlist->event_id;
    pthread_mutex_unlock(&g_mutex);
    return true;
}
'''

Path("src/sr-replay-playlist.h").write_text(playlist_h, encoding="utf-8")
Path("src/sr-replay-playlist.c").write_text(playlist_c, encoding="utf-8")

cmake = Path("CMakeLists.txt")
replace_once(
    cmake,
    "    src/sr-replay-coverage.c\n    src/sr-replay-take.c\n",
    "    src/sr-replay-coverage.c\n    src/sr-replay-playlist.c\n    src/sr-replay-take.c\n",
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'EventDock.ReturnLive="RETURN LIVE"\n',
    'EventDock.ReturnLive="RETURN LIVE"\n'
    'EventDock.PlaylistA="PLAY LIST A"\n'
    'EventDock.PlaylistB="PLAY LIST B"\n'
    'EventDock.PlaylistNext="NEXT LIST"\n'
    'EventDock.PlaylistStop="STOP LIST"\n'
    'EventDock.PlaylistStarted="Event List %1 started on bus %2"\n'
    'EventDock.PlaylistAdvanced="Advanced to next Event in the highlight reel"\n'
    'EventDock.PlaylistStopped="Highlight reel auto-advance stopped"\n'
    'EventDock.PlaylistFailed="No playable Event is available in this Event List"\n'
    'EventDock.PlaylistFinished="No later playable Event is available in this Event List"\n'
    'EventDock.PlaylistState="List %1 %2/%3"\n',
)
replace_once(
    locale,
    'Hotkey.ReturnLive="Replay Event: RETURN LIVE"\n',
    'Hotkey.ReturnLive="Replay Event: RETURN LIVE"\n'
    'Hotkey.PlaylistA="Replay Event: play current Event List on A"\n'
    'Hotkey.PlaylistB="Replay Event: play current Event List on B"\n'
    'Hotkey.PlaylistNext="Replay Event: next highlight on active bus"\n'
    'Hotkey.PlaylistStop="Replay Event: stop highlight auto-advance on active bus"\n',
)

# Make every normal cue start on a real decodable frame. This is important for
# playlist fallback because coarse coverage can include an internal gap.
channel = Path("src/sr-replay-channel.c")
replace_once(
    channel,
    '''\tconst double speed = event.speed_percent;\n\tconst uint64_t event_in_ns = event.in_ns;\n\tconst uint64_t event_out_ns = event.out_ns;\n\tsr_event_controller_free_event(&event);\n\n\tchar *new_camera_name = bstrdup(camera_name);\n''',
    '''\tconst double speed = event.speed_percent;\n\tconst uint64_t event_in_ns = event.in_ns;\n\tconst uint64_t event_out_ns = event.out_ns;\n\tsr_event_controller_free_event(&event);\n\n\t/* Refuse a cue that cannot actually decode its first visible frame. Apart\n\t * from avoiding a black first frame, this lets Event List playback safely\n\t * fall through to another camera when coarse catalog bounds hide a gap. */\n\tAVFrame *first_frame = NULL;\n\tif (!sr_disk_player_decode_at(player, in_ns, &first_frame, NULL) || !first_frame) {\n\t\tav_frame_free(&first_frame);\n\t\tsr_disk_player_destroy(player);\n\t\tblog(LOG_WARNING, "Sports Replay: could not cue Event %llu on '%s': no decodable start frame",\n\t\t     (unsigned long long)event_id, camera_name);\n\t\treturn false;\n\t}\n\tav_frame_free(&first_frame);\n\n\tchar *new_camera_name = bstrdup(camera_name);\n''',
)

# Event Output chains the next playlist Event before reporting media-ended.
event_output = Path("src/sr-event-output.c")
replace_once(
    event_output,
    '#include "sr-replay-channel.h"\n#include "sr-scene-tracker.h"\n',
    '#include "sr-replay-channel.h"\n#include "sr-replay-playlist.h"\n#include "sr-scene-tracker.h"\n',
)
replace_once(
    event_output,
    '''\tstruct sr_replay_channel_state state;\n\tif (sr_replay_channel_get_state(output->bus, &state))\n\t\toutput_master_audio(output, &state, clock_ns);\n\telse\n\t\treset_audio_transport(output);\n\n\tif (ended)\n\t\tobs_source_media_ended(output->self);\n''',
    '''\tif (ended && sr_replay_playlist_advance_on_end(output->bus)) {\n\t\tended = false;\n\t\treset_audio_transport(output);\n\t}\n\n\tstruct sr_replay_channel_state state;\n\tif (sr_replay_channel_get_state(output->bus, &state))\n\t\toutput_master_audio(output, &state, clock_ns);\n\telse\n\t\treset_audio_transport(output);\n\n\tif (ended)\n\t\tobs_source_media_ended(output->self);\n''',
)
replace_once(
    event_output,
    '''\treset_audio_transport(output);\n\tsr_replay_channel_stop(output->bus);\n\tsr_scene_tracker_end_replay_guard();\n''',
    '''\treset_audio_transport(output);\n\tsr_replay_playlist_stop(output->bus);\n\tsr_replay_channel_stop(output->bus);\n\tsr_scene_tracker_end_replay_guard();\n''',
)

# RETURN LIVE explicitly ends both highlight reels.
take = Path("src/sr-replay-take.c")
replace_once(
    take,
    '#include "sr-event-output.h"\n#include "sr-scene-tracker.h"\n',
    '#include "sr-event-output.h"\n#include "sr-replay-playlist.h"\n#include "sr-scene-tracker.h"\n',
)
replace_once(
    take,
    '''\t/* Do not stop the replay bus before the OUT stinger: the native OBS\n\t * transition must still be able to mix the replay picture/audio. The Event\n\t * Output deactivation stops the bus once it has actually left program. */\n\tsr_scene_tracker_end_replay_guard();\n''',
    '''\t/* Do not stop the replay bus before the OUT stinger: the native OBS\n\t * transition must still be able to mix the replay picture/audio. The Event\n\t * Output deactivation stops the bus once it has actually left program. */\n\tsr_replay_playlist_stop(SR_REPLAY_BUS_A);\n\tsr_replay_playlist_stop(SR_REPLAY_BUS_B);\n\tsr_scene_tracker_end_replay_guard();\n''',
)

# Module lifecycle + hardware hotkeys for highlight reels.
main = Path("src/plugin-main.c")
replace_once(
    main,
    '#include "sr-replay-channel.h"\n#include "sr-replay-take.h"\n',
    '#include "sr-replay-channel.h"\n#include "sr-replay-playlist.h"\n#include "sr-replay-take.h"\n',
)
replace_once(
    main,
    'static obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;\nstatic obs_hotkey_id hk_angles[SR_ANGLE_HOTKEY_COUNT];\n',
    'static obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;\n'
    'static obs_hotkey_id hk_playlist_a = OBS_INVALID_HOTKEY_ID;\n'
    'static obs_hotkey_id hk_playlist_b = OBS_INVALID_HOTKEY_ID;\n'
    'static obs_hotkey_id hk_playlist_next = OBS_INVALID_HOTKEY_ID;\n'
    'static obs_hotkey_id hk_playlist_stop = OBS_INVALID_HOTKEY_ID;\n'
    'static obs_hotkey_id hk_angles[SR_ANGLE_HOTKEY_COUNT];\n',
)
playlist_callbacks = r'''static void start_playlist_bus(enum sr_replay_bus bus)
{
	if (!event_controller)
		return;
	const unsigned list_id = sr_event_controller_get_current_list(event_controller);
	if (!sr_replay_playlist_start(bus, list_id, NULL)) {
		obs_log(LOG_WARNING, "Sports Replay: no playable Event in list %u for bus %c", list_id,
			bus == SR_REPLAY_BUS_A ? 'A' : 'B');
		return;
	}
	if (!sr_replay_take_bus(event_controller, bus)) {
		sr_replay_playlist_stop(bus);
		sr_replay_channel_stop(bus);
		obs_log(LOG_WARNING, "Sports Replay: Event List %u started but TAKE %c failed", list_id,
			bus == SR_REPLAY_BUS_A ? 'A' : 'B');
	}
}

static void playlist_a_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		start_playlist_bus(SR_REPLAY_BUS_A);
}

static void playlist_b_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		start_playlist_bus(SR_REPLAY_BUS_B);
}

static void playlist_next_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	enum sr_replay_bus bus;
	if (!sr_replay_take_current_bus(&bus) || !sr_replay_playlist_next(bus))
		obs_log(LOG_WARNING, "Sports Replay: no later playable highlight on the active bus");
}

static void playlist_stop_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	enum sr_replay_bus bus;
	if (sr_replay_take_current_bus(&bus))
		sr_replay_playlist_stop(bus);
}

'''
replace_once(main, 'static void angle_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)\n{', playlist_callbacks + 'static void angle_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)\n{')
replace_once(
    main,
    '''\thk_return_live = obs_hotkey_register_frontend("SportsReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"),\n\t\t\t\t\t\t      return_live_cb, NULL);\n\n\tstatic const char *const angle_ids''',
    '''\thk_return_live = obs_hotkey_register_frontend("SportsReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"),\n\t\t\t\t\t\t      return_live_cb, NULL);\n\thk_playlist_a = obs_hotkey_register_frontend("SportsReplay.PlaylistA", obs_module_text("Hotkey.PlaylistA"),\n\t\t\t\t\t\t     playlist_a_cb, NULL);\n\thk_playlist_b = obs_hotkey_register_frontend("SportsReplay.PlaylistB", obs_module_text("Hotkey.PlaylistB"),\n\t\t\t\t\t\t     playlist_b_cb, NULL);\n\thk_playlist_next = obs_hotkey_register_frontend("SportsReplay.PlaylistNext", obs_module_text("Hotkey.PlaylistNext"),\n\t\t\t\t\t\t\tplaylist_next_cb, NULL);\n\thk_playlist_stop = obs_hotkey_register_frontend("SportsReplay.PlaylistStop", obs_module_text("Hotkey.PlaylistStop"),\n\t\t\t\t\t\t\tplaylist_stop_cb, NULL);\n\n\tstatic const char *const angle_ids''',
)
replace_once(
    main,
    '''\tif (hk_return_live != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_return_live);\n\tfor (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++) {''',
    '''\tif (hk_return_live != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_return_live);\n\tif (hk_playlist_a != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_playlist_a);\n\tif (hk_playlist_b != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_playlist_b);\n\tif (hk_playlist_next != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_playlist_next);\n\tif (hk_playlist_stop != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_playlist_stop);\n\tfor (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++) {''',
)
replace_once(
    main,
    '''\thk_take_toggle = OBS_INVALID_HOTKEY_ID;\n\thk_return_live = OBS_INVALID_HOTKEY_ID;\n}\n''',
    '''\thk_take_toggle = OBS_INVALID_HOTKEY_ID;\n\thk_return_live = OBS_INVALID_HOTKEY_ID;\n\thk_playlist_a = OBS_INVALID_HOTKEY_ID;\n\thk_playlist_b = OBS_INVALID_HOTKEY_ID;\n\thk_playlist_next = OBS_INVALID_HOTKEY_ID;\n\thk_playlist_stop = OBS_INVALID_HOTKEY_ID;\n}\n''',
)
replace_once(
    main,
    '''\tevent_controller = sr_event_controller_create();\n\tif (!event_controller || !sr_replay_channels_init(event_controller) || !sr_storage_manager_start()) {\n\t\tsr_storage_manager_stop();\n\t\tsr_replay_channels_shutdown();\n''',
    '''\tevent_controller = sr_event_controller_create();\n\tif (!event_controller || !sr_replay_channels_init(event_controller) ||\n\t    !sr_replay_playlist_init(event_controller) || !sr_storage_manager_start()) {\n\t\tsr_storage_manager_stop();\n\t\tsr_replay_playlist_shutdown();\n\t\tsr_replay_channels_shutdown();\n''',
)
replace_once(
    main,
    '''\tsr_storage_manager_stop();\n\tsr_replay_take_reset();\n\tsr_replay_channels_shutdown();\n''',
    '''\tsr_storage_manager_stop();\n\tsr_replay_take_reset();\n\tsr_replay_playlist_shutdown();\n\tsr_replay_channels_shutdown();\n''',
)

# Operator controls: the existing Event List is the highlight reel.
dock = Path("src/sr-event-dock.cpp")
replace_once(
    dock,
    '#include "sr-replay-coverage.h"\n#include "sr-replay-take.h"\n',
    '#include "sr-replay-coverage.h"\n#include "sr-replay-playlist.h"\n#include "sr-replay-take.h"\n',
)
replace_once(
    dock,
    '''\t\tauto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);\n\t\tauto *returnLive = new QPushButton(T("EventDock.ReturnLive"), this);\n\t\ttakeBar->addWidget(takeA);\n\t\ttakeBar->addWidget(takeB);\n\t\ttakeBar->addWidget(takeToggle);\n\t\ttakeBar->addWidget(returnLive);\n''',
    '''\t\tauto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);\n\t\tauto *returnLive = new QPushButton(T("EventDock.ReturnLive"), this);\n\t\tauto *playlistA = new QPushButton(T("EventDock.PlaylistA"), this);\n\t\tauto *playlistB = new QPushButton(T("EventDock.PlaylistB"), this);\n\t\tauto *playlistNext = new QPushButton(T("EventDock.PlaylistNext"), this);\n\t\tauto *playlistStop = new QPushButton(T("EventDock.PlaylistStop"), this);\n\t\ttakeBar->addWidget(playlistA);\n\t\ttakeBar->addWidget(playlistB);\n\t\ttakeBar->addWidget(playlistNext);\n\t\ttakeBar->addWidget(playlistStop);\n\t\ttakeBar->addSpacing(10);\n\t\ttakeBar->addWidget(takeA);\n\t\ttakeBar->addWidget(takeB);\n\t\ttakeBar->addWidget(takeToggle);\n\t\ttakeBar->addWidget(returnLive);\n''',
)
replace_once(
    dock,
    '''\t\tconnect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });\n\t\tconnect(takeB, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_B); });\n\t\tconnect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });\n\t\tconnect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });\n''',
    '''\t\tconnect(playlistA, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_A); });\n\t\tconnect(playlistB, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_B); });\n\t\tconnect(playlistNext, &QPushButton::clicked, this, [this]() { nextPlaylist(); });\n\t\tconnect(playlistStop, &QPushButton::clicked, this, [this]() { stopPlaylist(); });\n\t\tconnect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });\n\t\tconnect(takeB, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_B); });\n\t\tconnect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });\n\t\tconnect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });\n''',
)
replace_once(
    dock,
    '''\tvoid refreshTransportStatus()\n\t{\n\t\tif (!transportStatus)\n\t\t\treturn;\n\t\ttransportStatus->setText(channelSummary(SR_REPLAY_BUS_A, QStringLiteral("A")) + QStringLiteral("    ") +\n\t\t\t\t\t channelSummary(SR_REPLAY_BUS_B, QStringLiteral("B")));\n\t}\n''',
    '''\tQString playlistSummary(enum sr_replay_bus bus) const\n\t{\n\t\tsr_replay_playlist_state state = {};\n\t\tif (!sr_replay_playlist_get_state(bus, &state) || !state.active)\n\t\t\treturn QString();\n\t\treturn T("EventDock.PlaylistState").arg(state.list_id).arg(state.position + 1).arg(state.count);\n\t}\n\n\tvoid refreshTransportStatus()\n\t{\n\t\tif (!transportStatus)\n\t\t\treturn;\n\t\tQString a = channelSummary(SR_REPLAY_BUS_A, QStringLiteral("A"));\n\t\tQString b = channelSummary(SR_REPLAY_BUS_B, QStringLiteral("B"));\n\t\tconst QString pa = playlistSummary(SR_REPLAY_BUS_A);\n\t\tconst QString pb = playlistSummary(SR_REPLAY_BUS_B);\n\t\tif (!pa.isEmpty())\n\t\t\ta += QStringLiteral("  [") + pa + QStringLiteral("]");\n\t\tif (!pb.isEmpty())\n\t\t\tb += QStringLiteral("  [") + pb + QStringLiteral("]");\n\t\ttransportStatus->setText(a + QStringLiteral("    ") + b);\n\t}\n''',
)
replace_once(
    dock,
    '''\tvoid cueSelected(enum sr_replay_bus bus)\n\t{\n\t\tconst uint64_t eventId = selectedEventId();\n''',
    '''\tvoid cueSelected(enum sr_replay_bus bus)\n\t{\n\t\tconst uint64_t eventId = selectedEventId();\n''',
)
# stop an existing reel only once the manual cue has passed basic validation
replace_once(
    dock,
    '''\t\tconst QByteArray cameraUtf8 = camera.toUtf8();\n\t\tif (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {\n''',
    '''\t\tconst QByteArray cameraUtf8 = camera.toUtf8();\n\t\tsr_replay_playlist_stop(bus);\n\t\tif (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {\n''',
)
playlist_methods = r'''	void startPlaylist(enum sr_replay_bus bus)
	{
		const QString camera = selectedCamera();
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
		if (!controller || !sr_replay_playlist_start(bus, currentList(), preferred)) {
			setStatus("EventDock.PlaylistFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		status->setText(T("EventDock.PlaylistStarted")
					.arg(currentList())
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B")));
		refresh();
		refreshTransportStatus();
	}

	void nextPlaylist()
	{
		if (!sr_replay_playlist_next(transportBus())) {
			setStatus("EventDock.PlaylistFinished");
			refreshTransportStatus();
			return;
		}
		setStatus("EventDock.PlaylistAdvanced");
		refresh();
		refreshTransportStatus();
	}

	void stopPlaylist()
	{
		sr_replay_playlist_stop(transportBus());
		setStatus("EventDock.PlaylistStopped");
		refreshTransportStatus();
	}

'''
replace_once(dock, '\tvoid takeBus(enum sr_replay_bus bus)\n{', playlist_methods + '\tvoid takeBus(enum sr_replay_bus bus)\n{')
replace_once(
    dock,
    '''\tvoid stopTransport()\n\t{\n\t\tsr_replay_channel_stop(transportBus());\n''',
    '''\tvoid stopTransport()\n\t{\n\t\tsr_replay_playlist_stop(transportBus());\n\t\tsr_replay_channel_stop(transportBus());\n''',
)

print("highlight playlist integration applied")
