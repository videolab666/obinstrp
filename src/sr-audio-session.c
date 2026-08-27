/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-master-audio.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/threading.h>

/*
 * Audio callbacks do not necessarily execute on the OBS video thread, but the
 * timestamps delivered by libobs use the same nanosecond timeline that replay
 * Events use. Map them at the producer boundary, exactly like video packets in
 * sr-segment-writer.c, so a resumed Recording Run stays A/V aligned after an
 * OBS restart or STOP/START cycle.
 */
static pthread_mutex_t g_raw_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static audio_output_callback_t g_raw_audio_callback;
static void *g_raw_audio_param;

static void sr_session_raw_audio_proxy(void *unused, size_t mix_idx, struct audio_data *data)
{
	UNUSED_PARAMETER(unused);
	if (!data)
		return;

	pthread_mutex_lock(&g_raw_audio_mutex);
	audio_output_callback_t callback = g_raw_audio_callback;
	void *param = g_raw_audio_param;
	pthread_mutex_unlock(&g_raw_audio_mutex);
	if (!callback)
		return;

	struct audio_data mapped = *data;
	mapped.timestamp = sr_session_map_recording_timestamp(data->timestamp);
	callback(param, mix_idx, &mapped);
}

/* sr-master-audio.c is compiled with obs_add/remove_raw_audio_callback renamed
 * to these functions. The underlying OBS callback remains registered only
 * once; this proxy changes timestamp ownership without touching the mature AAC
 * encoder/queue implementation. */
void sr_session_add_raw_audio_callback(size_t mix_idx, const struct audio_convert_info *conversion,
				       audio_output_callback_t callback, void *param)
{
	pthread_mutex_lock(&g_raw_audio_mutex);
	g_raw_audio_callback = callback;
	g_raw_audio_param = param;
	pthread_mutex_unlock(&g_raw_audio_mutex);
	obs_add_raw_audio_callback(mix_idx, conversion, sr_session_raw_audio_proxy, NULL);
}

void sr_session_remove_raw_audio_callback(size_t mix_idx, audio_output_callback_t callback, void *param)
{
	UNUSED_PARAMETER(callback);
	UNUSED_PARAMETER(param);
	obs_remove_raw_audio_callback(mix_idx, sr_session_raw_audio_proxy, NULL);
	pthread_mutex_lock(&g_raw_audio_mutex);
	g_raw_audio_callback = NULL;
	g_raw_audio_param = NULL;
	pthread_mutex_unlock(&g_raw_audio_mutex);
}

/* capture-filter.c is compiled with sr_camera_audio_writer_push redirected to
 * this adapter. Camera audio therefore uses the exact same Run mapping as
 * video and master audio before it enters any asynchronous encoder queue. */
bool sr_session_camera_audio_writer_push(struct sr_camera_audio_writer *writer, const struct obs_audio_data *audio,
					 size_t channels, uint64_t timestamp_ns)
{
	if (!audio)
		return false;
	const uint64_t source_timestamp = timestamp_ns ? timestamp_ns : audio->timestamp;
	return sr_camera_audio_writer_push(writer, audio, channels, sr_session_map_recording_timestamp(source_timestamp));
}
