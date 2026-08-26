/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#define SR_EVENT_OUTPUT_ID "pitel_instant_replay_event_output"
#define SR_EVENT_OUTPUT_SETTING_BUS "replay_bus"
#define SR_EVENT_OUTPUT_SETTING_AUDIO_MODE "audio_mode"
#define SR_EVENT_OUTPUT_SETTING_REPLAY_GAIN_DB "replay_gain_db"
#define SR_EVENT_OUTPUT_SETTING_LIVE_AUDIO_POLICY "live_audio_policy"
#define SR_EVENT_OUTPUT_SETTING_LIVE_DUCK_DB "live_duck_db"

enum sr_event_output_audio_mode {
	SR_EVENT_OUTPUT_AUDIO_OFF = 0,
	SR_EVENT_OUTPUT_AUDIO_MASTER = 1,
	SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS = 2,
	SR_EVENT_OUTPUT_AUDIO_CAMERA = 3,
};

enum sr_live_audio_policy {
	SR_LIVE_AUDIO_KEEP = 0,
	SR_LIVE_AUDIO_DUCK = 1,
	SR_LIVE_AUDIO_MUTE = 2,
};
