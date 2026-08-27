/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <libavutil/frame.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

enum sr_replay_bus {
	SR_REPLAY_BUS_A = 0,
	SR_REPLAY_BUS_B = 1,
	SR_REPLAY_BUS_COUNT = 2,
};

enum sr_replay_audio_mode {
	SR_REPLAY_AUDIO_OFF = 0,
	SR_REPLAY_AUDIO_MASTER = 1,
	SR_REPLAY_AUDIO_CAMERA = 2,
};

struct sr_replay_channel_state {
	uint64_t event_id;
	uint64_t in_ns;
	uint64_t out_ns;
	uint64_t playhead_ns;
	int64_t sync_offset_ns;
	double speed_percent;
	enum sr_replay_audio_mode audio_mode;
	uint32_t width;
	uint32_t height;
	bool cued;
	bool playing;
	bool paused;
	bool backward;
	bool loop;
	bool partial_coverage;
	bool preview_mode;
	bool decoder_open;
	bool hardware_decode;
	uint64_t decode_requests;
	uint64_t decode_cache_hits;
	uint64_t decoded_frames;
	char camera_name[256];
};

/* Process-wide A/B transport. It is initialized once by plugin-main after the
 * Event controller exists and is shared by the operator dock and Event Output
 * sources. Each bus owns an independent disk player, camera, event, speed and
 * playhead. */
bool sr_replay_channels_init(struct sr_event_controller *events);
void sr_replay_channels_shutdown(void);

bool sr_replay_channel_cue(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name);

/* Cues a transient EDIT preview range. Unlike a normal Event cue, the visible
 * transport bounds may span the recording around the requested playhead. The
 * Event database is not modified and taking the bus should re-cue the Event
 * normally first. */
bool sr_replay_channel_cue_preview(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name,
				   uint64_t range_in_ns, uint64_t range_out_ns, uint64_t playhead_ns);

/* Replaces only the camera backing an already-cued Event. The Event, playhead,
 * speed, direction, loop and play/pause state are preserved atomically. The
 * switch is rejected when the requested camera does not contain the current
 * playhead, so an on-air angle change never jumps in time. */
bool sr_replay_channel_switch_camera(enum sr_replay_bus bus, const char *camera_name);

void sr_replay_channel_clear(enum sr_replay_bus bus);

bool sr_replay_channel_play(enum sr_replay_bus bus);
bool sr_replay_channel_pause(enum sr_replay_bus bus, bool paused);
void sr_replay_channel_stop(enum sr_replay_bus bus);
void sr_replay_channel_restart(enum sr_replay_bus bus);

bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent);

/* Process-wide operator speed controller. In Global policy it is applied to
 * both buses immediately and is inherited by every newly cued Event/angle. */
bool sr_replay_channel_set_controller_speed(double speed_percent);
double sr_replay_channel_get_controller_speed(void);
bool sr_replay_channel_set_audio_mode(enum sr_replay_bus bus, enum sr_replay_audio_mode audio_mode);
bool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward);
bool sr_replay_channel_set_loop(enum sr_replay_bus bus, bool loop);
bool sr_replay_channel_seek(enum sr_replay_bus bus, uint64_t timestamp_ns);
bool sr_replay_channel_seek_relative(enum sr_replay_bus bus, int64_t delta_ns);
bool sr_replay_channel_step_frames(enum sr_replay_bus bus, int frames);

bool sr_replay_channel_get_state(enum sr_replay_bus bus, struct sr_replay_channel_state *state);

/* Advances the selected bus from a monotonic clock and returns a decoded frame
 * when the visible playhead needs refreshing. Multiple OBS sources may point
 * at one bus: clock_ns makes repeated calls effectively idempotent instead of
 * advancing the transport once per source instance. The returned frame is
 * owned by the caller and must be released with av_frame_free(). */
bool sr_replay_channel_render(enum sr_replay_bus bus, uint64_t clock_ns, AVFrame **frame, uint64_t *media_timestamp_ns,
			      bool *ended);

#ifdef __cplusplus
}
#endif
