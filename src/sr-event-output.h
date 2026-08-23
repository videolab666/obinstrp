/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#define SR_EVENT_OUTPUT_ID "sports_replay_event_output"
#define SR_EVENT_OUTPUT_SETTING_BUS "replay_bus"
#define SR_EVENT_OUTPUT_SETTING_AUDIO_MODE "audio_mode"

enum sr_event_output_audio_mode {
	SR_EVENT_OUTPUT_AUDIO_OFF = 0,
	SR_EVENT_OUTPUT_AUDIO_MASTER = 1,
};
