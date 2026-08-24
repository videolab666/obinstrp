/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-module.h>
#include <plugin-support.h>

#include "sr-camera-list.h"
#include "sr-config.h"
#include "sr-dock.h"
#include "sr-event-controller.h"
#include "sr-master-audio.h"
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
extern struct obs_source_info sr_playback_info;
extern struct obs_source_info sr_event_output_info;

#define NS_PER_SECOND 1000000000ULL
#define SR_ANGLE_HOTKEY_COUNT 8u

static struct sr_event_controller *event_controller;
static obs_hotkey_id hk_event_in = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_event_out = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_event_5 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_event_10 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_event_20 = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_take_a = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_take_b = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_take_toggle = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_playlist_a = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_playlist_b = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_playlist_next = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_playlist_stop = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id hk_angles[SR_ANGLE_HOTKEY_COUNT];

static void log_created_event(const char *action, uint64_t event_id)
{
	obs_log(LOG_INFO, "Event %llu created by %s in list %u", (unsigned long long)event_id, action,
		sr_event_controller_get_current_list(event_controller));
}

static uint64_t event_now_ns(void)
{
	/* Segment indexes are written from obs_source_frame::timestamp. Use the
	 * libobs video clock for operator marks as well so Event IN/OUT lives on
	 * the same timeline as the recorded camera packets. */
	return obs_get_video_frame_time();
}

static void event_mark_in_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed || !event_controller)
		return;

	if (!sr_event_controller_mark_in(event_controller, event_now_ns()))
		obs_log(LOG_WARNING, "Could not set replay Event IN mark");
}

static void event_mark_out_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed || !event_controller)
		return;

	uint64_t event_id = 0;
	if (sr_event_controller_mark_out(event_controller, event_now_ns(), &event_id))
		log_created_event("OUT", event_id);
	else
		obs_log(LOG_WARNING, "Could not create replay Event OUT (set IN first)");
}

static void quick_mark(uint64_t pre_roll_ns, const char *action)
{
	if (!event_controller)
		return;

	uint64_t event_id = 0;
	if (sr_event_controller_quick_mark(event_controller, event_now_ns(), pre_roll_ns, 0, &event_id))
		log_created_event(action, event_id);
	else
		obs_log(LOG_WARNING, "Could not create replay Event %s", action);
}

static void event_mark_5_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(5 * NS_PER_SECOND, "-5");
}

static void event_mark_10_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(10 * NS_PER_SECOND, "-10");
}

static void event_mark_20_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		quick_mark(20 * NS_PER_SECOND, "-20");
}

static void take_a_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && event_controller && !sr_replay_take_bus(event_controller, SR_REPLAY_BUS_A))
		obs_log(LOG_WARNING, "Sports Replay: TAKE A failed");
}

static void take_b_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && event_controller && !sr_replay_take_bus(event_controller, SR_REPLAY_BUS_B))
		obs_log(LOG_WARNING, "Sports Replay: TAKE B failed");
}

static void take_toggle_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && event_controller && !sr_replay_take_toggle(event_controller))
		obs_log(LOG_WARNING, "Sports Replay: TAKE A/B toggle failed");
}

static void return_live_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed && event_controller && !sr_replay_take_return(event_controller))
		obs_log(LOG_WARNING, "Sports Replay: RETURN LIVE failed");
}

static void start_playlist_bus(enum sr_replay_bus bus)
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

static void angle_hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
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
		obs_log(LOG_WARNING,
			"Sports Replay: bus %c could not switch to angle %zu ('%s') at the current playhead",
			bus == SR_REPLAY_BUS_A ? 'A' : 'B', index + 1, camera);
	sr_camera_list_free(&cameras);
}

static void register_event_hotkeys(void)
{
	for (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++)
		hk_angles[i] = OBS_INVALID_HOTKEY_ID;

	hk_event_in = obs_hotkey_register_frontend("SportsReplay.EventIn", obs_module_text("Hotkey.EventIn"),
						   event_mark_in_cb, NULL);
	hk_event_out = obs_hotkey_register_frontend("SportsReplay.EventOut", obs_module_text("Hotkey.EventOut"),
						    event_mark_out_cb, NULL);
	hk_event_5 = obs_hotkey_register_frontend("SportsReplay.EventLast5", obs_module_text("Hotkey.EventLast5"),
						  event_mark_5_cb, NULL);
	hk_event_10 = obs_hotkey_register_frontend("SportsReplay.EventLast10", obs_module_text("Hotkey.EventLast10"),
						   event_mark_10_cb, NULL);
	hk_event_20 = obs_hotkey_register_frontend("SportsReplay.EventLast20", obs_module_text("Hotkey.EventLast20"),
						   event_mark_20_cb, NULL);
	hk_take_a =
		obs_hotkey_register_frontend("SportsReplay.TakeA", obs_module_text("Hotkey.TakeA"), take_a_cb, NULL);
	hk_take_b =
		obs_hotkey_register_frontend("SportsReplay.TakeB", obs_module_text("Hotkey.TakeB"), take_b_cb, NULL);
	hk_take_toggle = obs_hotkey_register_frontend("SportsReplay.TakeToggle", obs_module_text("Hotkey.TakeToggle"),
						      take_toggle_cb, NULL);
	hk_return_live = obs_hotkey_register_frontend("SportsReplay.ReturnLive", obs_module_text("Hotkey.ReturnLive"),
						      return_live_cb, NULL);
	hk_playlist_a = obs_hotkey_register_frontend("SportsReplay.PlaylistA", obs_module_text("Hotkey.PlaylistA"),
						     playlist_a_cb, NULL);
	hk_playlist_b = obs_hotkey_register_frontend("SportsReplay.PlaylistB", obs_module_text("Hotkey.PlaylistB"),
						     playlist_b_cb, NULL);
	hk_playlist_next = obs_hotkey_register_frontend("SportsReplay.PlaylistNext",
							obs_module_text("Hotkey.PlaylistNext"), playlist_next_cb, NULL);
	hk_playlist_stop = obs_hotkey_register_frontend("SportsReplay.PlaylistStop",
							obs_module_text("Hotkey.PlaylistStop"), playlist_stop_cb, NULL);

	static const char *const angle_ids[SR_ANGLE_HOTKEY_COUNT] = {
		"SportsReplay.Angle1", "SportsReplay.Angle2", "SportsReplay.Angle3", "SportsReplay.Angle4",
		"SportsReplay.Angle5", "SportsReplay.Angle6", "SportsReplay.Angle7", "SportsReplay.Angle8",
	};
	static const char *const angle_text[SR_ANGLE_HOTKEY_COUNT] = {
		"Hotkey.Angle1", "Hotkey.Angle2", "Hotkey.Angle3", "Hotkey.Angle4",
		"Hotkey.Angle5", "Hotkey.Angle6", "Hotkey.Angle7", "Hotkey.Angle8",
	};
	for (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++)
		hk_angles[i] = obs_hotkey_register_frontend(angle_ids[i], obs_module_text(angle_text[i]),
							    angle_hotkey_cb, (void *)(uintptr_t)i);
}

static void unregister_event_hotkeys(void)
{
	if (hk_event_in != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_event_in);
	if (hk_event_out != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_event_out);
	if (hk_event_5 != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_event_5);
	if (hk_event_10 != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_event_10);
	if (hk_event_20 != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_event_20);
	if (hk_take_a != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_take_a);
	if (hk_take_b != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_take_b);
	if (hk_take_toggle != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_take_toggle);
	if (hk_return_live != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_return_live);
	if (hk_playlist_a != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_playlist_a);
	if (hk_playlist_b != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_playlist_b);
	if (hk_playlist_next != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_playlist_next);
	if (hk_playlist_stop != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_playlist_stop);
	for (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++) {
		if (hk_angles[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hk_angles[i]);
		hk_angles[i] = OBS_INVALID_HOTKEY_ID;
	}

	hk_event_in = OBS_INVALID_HOTKEY_ID;
	hk_event_out = OBS_INVALID_HOTKEY_ID;
	hk_event_5 = OBS_INVALID_HOTKEY_ID;
	hk_event_10 = OBS_INVALID_HOTKEY_ID;
	hk_event_20 = OBS_INVALID_HOTKEY_ID;
	hk_take_a = OBS_INVALID_HOTKEY_ID;
	hk_take_b = OBS_INVALID_HOTKEY_ID;
	hk_take_toggle = OBS_INVALID_HOTKEY_ID;
	hk_return_live = OBS_INVALID_HOTKEY_ID;
	hk_playlist_a = OBS_INVALID_HOTKEY_ID;
	hk_playlist_b = OBS_INVALID_HOTKEY_ID;
	hk_playlist_next = OBS_INVALID_HOTKEY_ID;
	hk_playlist_stop = OBS_INVALID_HOTKEY_ID;
}

bool obs_module_load(void)
{
	sr_config_init();
	sr_session_init();
	if (!sr_master_audio_init()) {
		sr_session_free();
		sr_config_free();
		obs_log(LOG_ERROR, "Sports Replay: could not initialize master replay audio capture");
		return false;
	}
	if (!sr_storage_cleanup_init()) {
		sr_master_audio_free();
		sr_session_free();
		sr_config_free();
		obs_log(LOG_ERROR, "Sports Replay: could not initialize storage synchronization");
		return false;
	}
	event_controller = sr_event_controller_create();
	if (!event_controller || !sr_replay_channels_init(event_controller) ||
	    !sr_replay_playlist_init(event_controller) || !sr_storage_manager_start()) {
		sr_storage_manager_stop();
		sr_replay_playlist_shutdown();
		sr_replay_channels_shutdown();
		sr_event_controller_destroy(event_controller);
		event_controller = NULL;
		sr_storage_cleanup_free();
		sr_master_audio_free();
		sr_session_free();
		sr_config_free();
		obs_log(LOG_ERROR, "Sports Replay: could not initialize replay Event controller");
		return false;
	}

	register_event_hotkeys();
	obs_register_source(&sr_capture_info);
	obs_register_source(&sr_playback_info);
	obs_register_source(&sr_event_output_info);
	obs_log(LOG_INFO, "Sports Replay loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	sr_scene_tracker_start();
	sr_dock_register(event_controller);
}

void obs_module_unload(void)
{
	sr_scene_tracker_stop();
	unregister_event_hotkeys();
	sr_storage_manager_stop();
	sr_replay_take_reset();
	sr_replay_playlist_shutdown();
	sr_replay_channels_shutdown();
	sr_event_controller_destroy(event_controller);
	event_controller = NULL;
	sr_storage_cleanup_free();
	sr_master_audio_free();
	sr_session_free();
	sr_config_free();
	obs_log(LOG_INFO, "Sports Replay unloaded");
}

const char *obs_module_name(void)
{
	return "Sports Replay";
}

const char *obs_module_description(void)
{
	return obs_module_text("Description");
}
