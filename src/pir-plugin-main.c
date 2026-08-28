/*
 * Pitel Instant Replay - OBS module entry point
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "sr-camera-list.h"
#include "sr-capture.h"
#include "sr-config.h"
#include "sr-dock.h"
#include "sr-event-controller.h"
#include "sr-master-audio.h"
#include "sr-program-recorder.h"
#include "sr-replay-channel.h"
#include "sr-replay-playlist.h"
#include "sr-replay-take.h"
#include "sr-scene-tracker.h"
#include "sr-session.h"
#include "sr-storage-cleanup.h"
#include "sr-storage-manager.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info sr_capture_info;
extern struct obs_source_info sr_event_output_info;

#define NS_PER_SECOND 1000000000ULL
#define ANGLE_HOTKEYS 8u

static struct sr_event_controller *g_controller;
static obs_hotkey_id g_event_in = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_event_out = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_quick_5 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_quick_10 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_quick_20 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_take_a = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_take_b = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_take_toggle = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_return_live = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_playlist_a = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_playlist_b = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_playlist_next = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_playlist_stop = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_angle[ANGLE_HOTKEYS];

static uint64_t live_timestamp(void)
{
	return obs_get_video_frame_time();
}

static void mark_in(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && g_controller && !sr_event_controller_mark_in(g_controller, live_timestamp()))
		blog(LOG_WARNING, "Pitel Instant Replay: could not set Event IN");
}

static void mark_out(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed || !g_controller)
		return;
	uint64_t event_id = 0;
	if (!sr_event_controller_mark_out(g_controller, live_timestamp(), &event_id))
		blog(LOG_WARNING, "Pitel Instant Replay: could not create Event OUT");
}

static void quick_mark(uint64_t preroll)
{
	if (!g_controller)
		return;
	uint64_t event_id = 0;
	if (!sr_event_controller_quick_mark(g_controller, live_timestamp(), preroll, 0, &event_id))
		blog(LOG_WARNING, "Pitel Instant Replay: quick Event mark failed");
}

static void quick_5(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(5ULL * NS_PER_SECOND);
}

static void quick_10(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(10ULL * NS_PER_SECOND);
}

static void quick_20(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(20ULL * NS_PER_SECOND);
}

static void take_a(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && g_controller && !sr_replay_take_bus(g_controller, SR_REPLAY_BUS_A))
		blog(LOG_WARNING, "Pitel Instant Replay: TAKE A failed");
}

static void take_b(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && g_controller && !sr_replay_take_bus(g_controller, SR_REPLAY_BUS_B))
		blog(LOG_WARNING, "Pitel Instant Replay: TAKE B failed");
}

static void take_toggle(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && g_controller && !sr_replay_take_toggle(g_controller))
		blog(LOG_WARNING, "Pitel Instant Replay: TAKE toggle failed");
}

static void return_live(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && g_controller && !sr_replay_take_return(g_controller))
		blog(LOG_WARNING, "Pitel Instant Replay: RETURN LIVE failed");
}

static void start_playlist(enum sr_replay_bus bus)
{
	if (!g_controller)
		return;
	const unsigned list_id = sr_event_controller_get_current_list(g_controller);
	if (!sr_replay_playlist_start(bus, list_id, NULL)) {
		blog(LOG_WARNING, "Pitel Instant Replay: Event List %u has no playable Event", list_id);
		return;
	}
	if (!sr_replay_take_bus(g_controller, bus)) {
		sr_replay_playlist_stop(bus);
		sr_replay_channel_stop(bus);
		blog(LOG_WARNING, "Pitel Instant Replay: playlist TAKE failed");
	}
}

static void playlist_a(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		start_playlist(SR_REPLAY_BUS_A);
}

static void playlist_b(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		start_playlist(SR_REPLAY_BUS_B);
}

static void playlist_next(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	enum sr_replay_bus bus;
	if (!sr_replay_take_current_bus(&bus) || !sr_replay_playlist_next(bus))
		blog(LOG_WARNING, "Pitel Instant Replay: no next playlist Event");
}

static void playlist_stop(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
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

static void select_angle(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	const size_t index = (size_t)(uintptr_t)data;
	if (index >= ANGLE_HOTKEYS)
		return;

	enum sr_replay_bus bus;
	if (!sr_replay_take_current_bus(&bus))
		return;

	struct sr_camera_list cameras = {0};
	if (!sr_camera_list_capture(&cameras))
		return;
	if (index < cameras.count && !sr_replay_channel_switch_camera(bus, cameras.names[index]))
		blog(LOG_WARNING, "Pitel Instant Replay: angle %zu is unavailable at the current playhead", index + 1);
	sr_camera_list_free(&cameras);
}

static void register_hotkeys(void)
{
	g_event_in = obs_hotkey_register_frontend("PitelInstantReplay.EventIn", obs_module_text("Hotkey.EventIn"), mark_in, NULL);
	g_event_out = obs_hotkey_register_frontend("PitelInstantReplay.EventOut", obs_module_text("Hotkey.EventOut"), mark_out, NULL);
	g_quick_5 = obs_hotkey_register_frontend("PitelInstantReplay.EventLast5", obs_module_text("Hotkey.EventLast5"), quick_5, NULL);
	g_quick_10 = obs_hotkey_register_frontend("PitelInstantReplay.EventLast10", obs_module_text("Hotkey.EventLast10"), quick_10, NULL);
	g_quick_20 = obs_hotkey_register_frontend("PitelInstantReplay.EventLast20", obs_module_text("Hotkey.EventLast20"), quick_20, NULL);
	g_take_a = obs_hotkey_register_frontend("PitelInstantReplay.TakeA", obs_module_text("Hotkey.TakeA"), take_a, NULL);
	g_take_b = obs_hotkey_register_frontend("PitelInstantReplay.TakeB", obs_module_text("Hotkey.TakeB"), take_b, NULL);
	g_take_toggle = obs_hotkey_register_frontend("PitelInstantReplay.TakeToggle", obs_module_text("Hotkey.TakeToggle"), take_toggle, NULL);
	g_return_live = obs_hotkey_register_frontend("PitelInstantReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"), return_live, NULL);
	g_playlist_a = obs_hotkey_register_frontend("PitelInstantReplay.PlaylistA", obs_module_text("Hotkey.PlaylistA"), playlist_a, NULL);
	g_playlist_b = obs_hotkey_register_frontend("PitelInstantReplay.PlaylistB", obs_module_text("Hotkey.PlaylistB"), playlist_b, NULL);
	g_playlist_next = obs_hotkey_register_frontend("PitelInstantReplay.PlaylistNext", obs_module_text("Hotkey.PlaylistNext"), playlist_next, NULL);
	g_playlist_stop = obs_hotkey_register_frontend("PitelInstantReplay.PlaylistStop", obs_module_text("Hotkey.PlaylistStop"), playlist_stop, NULL);

	static const char *const ids[ANGLE_HOTKEYS] = {
		"PitelInstantReplay.Angle1", "PitelInstantReplay.Angle2", "PitelInstantReplay.Angle3", "PitelInstantReplay.Angle4",
		"PitelInstantReplay.Angle5", "PitelInstantReplay.Angle6", "PitelInstantReplay.Angle7", "PitelInstantReplay.Angle8",
	};
	static const char *const labels[ANGLE_HOTKEYS] = {
		"Hotkey.Angle1", "Hotkey.Angle2", "Hotkey.Angle3", "Hotkey.Angle4",
		"Hotkey.Angle5", "Hotkey.Angle6", "Hotkey.Angle7", "Hotkey.Angle8",
	};
	for (size_t i = 0; i < ANGLE_HOTKEYS; i++)
		g_angle[i] = obs_hotkey_register_frontend(ids[i], obs_module_text(labels[i]), select_angle,
							  (void *)(uintptr_t)i);
}

static void unregister_one(obs_hotkey_id *id)
{
	if (*id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(*id);
	*id = OBS_INVALID_HOTKEY_ID;
}

static void unregister_hotkeys(void)
{
	unregister_one(&g_event_in);
	unregister_one(&g_event_out);
	unregister_one(&g_quick_5);
	unregister_one(&g_quick_10);
	unregister_one(&g_quick_20);
	unregister_one(&g_take_a);
	unregister_one(&g_take_b);
	unregister_one(&g_take_toggle);
	unregister_one(&g_return_live);
	unregister_one(&g_playlist_a);
	unregister_one(&g_playlist_b);
	unregister_one(&g_playlist_next);
	unregister_one(&g_playlist_stop);
	for (size_t i = 0; i < ANGLE_HOTKEYS; i++)
		unregister_one(&g_angle[i]);
}

static void frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING && event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		return;

	size_t source_count = 0;
	sr_capture_set_all_disk_recording(false, &source_count);
	blog(LOG_INFO, "Pitel Instant Replay: REC reset to STOPPED for %zu source(s)", source_count);
}

bool obs_module_load(void)
{
	for (size_t i = 0; i < ANGLE_HOTKEYS; i++)
		g_angle[i] = OBS_INVALID_HOTKEY_ID;

	sr_config_init();
	sr_session_init();
	if (!sr_master_audio_init())
		goto fail_session;
	if (!sr_program_recorder_init())
		goto fail_audio;
	if (!sr_storage_cleanup_init())
		goto fail_program;

	g_controller = sr_event_controller_create();
	if (!g_controller)
		goto fail_storage_cleanup;
	if (!sr_replay_channels_init(g_controller))
		goto fail_controller;
	if (!sr_replay_playlist_init(g_controller))
		goto fail_channels;
	if (!sr_storage_manager_start())
		goto fail_playlist;

	register_hotkeys();
	obs_register_source(&sr_capture_info);
	obs_register_source(&sr_event_output_info);
	blog(LOG_INFO, "Pitel Instant Replay loaded (version %s)", PLUGIN_VERSION);
	return true;

fail_playlist:
	sr_replay_playlist_shutdown();
fail_channels:
	sr_replay_channels_shutdown();
fail_controller:
	sr_event_controller_destroy(g_controller);
	g_controller = NULL;
fail_storage_cleanup:
	sr_storage_cleanup_free();
fail_program:
	sr_program_recorder_free();
fail_audio:
	sr_master_audio_free();
fail_session:
	sr_session_free();
	sr_config_free();
	blog(LOG_ERROR, "Pitel Instant Replay: module initialization failed");
	return false;
}

void obs_module_post_load(void)
{
	obs_frontend_add_event_callback(frontend_event, NULL);
	sr_scene_tracker_start();
	sr_dock_register(g_controller);
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, NULL);
	sr_scene_tracker_stop();
	unregister_hotkeys();
	sr_storage_manager_stop();
	sr_replay_take_reset();
	sr_replay_playlist_shutdown();
	sr_replay_channels_shutdown();
	sr_event_controller_destroy(g_controller);
	g_controller = NULL;
	sr_storage_cleanup_free();
	sr_program_recorder_free();
	sr_master_audio_free();
	sr_session_free();
	sr_config_free();
	blog(LOG_INFO, "Pitel Instant Replay unloaded");
}

const char *obs_module_name(void)
{
	return "Pitel Instant Replay";
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}
