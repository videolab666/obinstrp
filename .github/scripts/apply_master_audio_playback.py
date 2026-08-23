from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# Master audio writer: publish live index/data snapshots and persist real segment
# sequence numbers so the reader can order rotations deterministically.
audio = Path("src/sr-master-audio.c")
replace_once(
    audio,
    "#define MASTER_AUDIO_MAX_QUEUE_CHUNKS 256u\n",
    "#define MASTER_AUDIO_MAX_QUEUE_CHUNKS 256u\n#define MASTER_AUDIO_PUBLISH_NS 250000000ULL\n",
)
replace_once(
    audio,
    "\tuint64_t segment_start_ns;\n\n\tstruct sr_master_audio_stats stats;\n",
    "\tuint64_t segment_start_ns;\n\tuint64_t last_publish_ns;\n\n\tstruct sr_master_audio_stats stats;\n",
)
replace_once(
    audio,
    "\tclear_segment_paths(state);\n\tstate->segment_start_ns = 0;\n}\n",
    "\tclear_segment_paths(state);\n\tstate->segment_start_ns = 0;\n\tstate->last_publish_ns = 0;\n}\n",
)
replace_once(
    audio,
    "\theader.flags = state->next_segment_discontinuity ? SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY : 0;\n\theader.segment_start_ns = start_ns;\n",
    "\theader.flags = state->next_segment_discontinuity ? SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY : 0;\n\theader.sequence = sequence;\n\theader.segment_start_ns = start_ns;\n",
)
replace_once(
    audio,
    "\tindex.version = SR_AUDIO_FORMAT_VERSION;\n\tindex.segment_start_ns = start_ns;\n",
    "\tindex.version = SR_AUDIO_FORMAT_VERSION;\n\tindex.sequence = sequence;\n\tindex.segment_start_ns = start_ns;\n",
)
replace_once(
    audio,
    "\tconst bool ok = write_exact(state->audio_file, &header, sizeof(header)) &&\n\t\t\twrite_exact(state->audio_file, state->encoder->extradata, header.extradata_size) &&\n\t\t\twrite_exact(state->index_file, &index, sizeof(index));\n",
    "\tconst bool ok = write_exact(state->audio_file, &header, sizeof(header)) &&\n\t\t\twrite_exact(state->audio_file, state->encoder->extradata, header.extradata_size) &&\n\t\t\twrite_exact(state->index_file, &index, sizeof(index)) && fflush(state->audio_file) == 0 &&\n\t\t\tfflush(state->index_file) == 0;\n",
)
replace_once(
    audio,
    "\tstate->segment_start_ns = start_ns;\n\tstate->next_segment_discontinuity = false;\n",
    "\tstate->segment_start_ns = start_ns;\n\tstate->last_publish_ns = start_ns;\n\tstate->next_segment_discontinuity = false;\n",
)
replace_once(
    audio,
    "\tstats_add_packet(state, sizeof(header) + (uint64_t)packet->size + sizeof(index));\n\treturn true;\n}\n",
    "\tstats_add_packet(state, sizeof(header) + (uint64_t)packet->size + sizeof(index));\n\tif (timestamp_ns >= state->last_publish_ns &&\n\t    timestamp_ns - state->last_publish_ns >= MASTER_AUDIO_PUBLISH_NS) {\n\t\tif (fflush(state->audio_file) != 0 || fflush(state->index_file) != 0) {\n\t\t\tstats_set_write_failed(state);\n\t\t\treturn false;\n\t\t}\n\t\tstate->last_publish_ns = timestamp_ns;\n\t}\n\treturn true;\n}\n",
)

# Remove a helper that became redundant while the player loop was simplified.
player = Path("src/sr-master-audio-player.c")
text = player.read_text(encoding="utf-8")
start = text.find("static bool receive_frame(")
if start >= 0:
    end = text.find("\nbool sr_master_audio_player_decode_next", start)
    if end < 0:
        raise SystemExit("could not remove receive_frame helper")
    text = text[:start] + text[end + 1 :]
    player.write_text(text, encoding="utf-8")

# Event Output gets an explicit Off/Master mode. Reverse and non-100% transport
# intentionally mute for this first checkpoint; time-stretch is a later stage.
Path("src/sr-event-output.h").write_text(
    r'''/*
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
''',
    encoding="utf-8",
)

Path("src/sr-event-output.c").write_text(
    r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-output.h"

#include "sr-master-audio-player.h"
#include "sr-replay-channel.h"
#include "sr-session.h"

#include <media-io/audio-io.h>
#include <media-io/video-io.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <libavutil/samplefmt.h>

#include <math.h>

#define AUDIO_LEAD_NS 60000000ULL
#define AUDIO_RESYNC_NS 100000000ULL
#define AUDIO_MAX_FRAMES_PER_TICK 8u

struct sr_event_output {
    obs_source_t *self;
    enum sr_replay_bus bus;
    enum sr_event_output_audio_mode audio_mode;
    uint32_t width;
    uint32_t height;

    struct sr_master_audio_player *audio_player;
    uint64_t audio_event_id;
    uint64_t audio_anchor_media_ns;
    uint64_t audio_anchor_clock_ns;
    uint64_t audio_queued_until_ns;
    bool audio_started;
    bool audio_format_warned;
};

static const char *sr_event_output_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("SportsReplayEventOutput");
}

static void output_avframe(struct sr_event_output *output, AVFrame *decoded)
{
    struct obs_source_frame frame = {0};
    frame.width = (uint32_t)decoded->width;
    frame.height = (uint32_t)decoded->height;
    frame.timestamp = os_gettime_ns();

    switch (decoded->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        frame.format = VIDEO_FORMAT_I420;
        break;
    case AV_PIX_FMT_NV12:
        frame.format = VIDEO_FORMAT_NV12;
        break;
    default:
        return;
    }

    for (size_t i = 0; i < MAX_AV_PLANES; i++) {
        frame.data[i] = decoded->data[i];
        frame.linesize[i] = (uint32_t)decoded->linesize[i];
    }

    video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, frame.color_matrix, frame.color_range_min,
                                frame.color_range_max);
    obs_source_output_video(output->self, &frame);
}

static void reset_audio_transport(struct sr_event_output *output)
{
    output->audio_event_id = 0;
    output->audio_anchor_media_ns = 0;
    output->audio_anchor_clock_ns = 0;
    output->audio_queued_until_ns = 0;
    output->audio_started = false;
}

static bool ensure_audio_player(struct sr_event_output *output)
{
    if (output->audio_player)
        return true;

    char *session_dir = sr_session_get_or_create_path();
    if (!session_dir)
        return false;
    output->audio_player = sr_master_audio_player_create(session_dir);
    bfree(session_dir);
    return output->audio_player != NULL;
}

static uint64_t abs_delta_u64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

static bool master_audio_allowed(const struct sr_event_output *output, const struct sr_replay_channel_state *state)
{
    return output->audio_mode == SR_EVENT_OUTPUT_AUDIO_MASTER && state->cued && state->playing && !state->paused &&
           !state->backward && fabs(state->speed_percent - 100.0) < 0.01;
}

static bool seek_audio_to_playhead(struct sr_event_output *output, const struct sr_replay_channel_state *state,
                                   uint64_t clock_ns)
{
    if (!ensure_audio_player(output) || !sr_master_audio_player_seek(output->audio_player, state->playhead_ns)) {
        reset_audio_transport(output);
        return false;
    }

    output->audio_event_id = state->event_id;
    output->audio_anchor_media_ns = state->playhead_ns;
    output->audio_anchor_clock_ns = clock_ns;
    output->audio_queued_until_ns = state->playhead_ns;
    output->audio_started = true;
    return true;
}

static void output_audio_frame(struct sr_event_output *output, AVFrame *decoded, uint64_t media_timestamp_ns,
                               const struct sr_replay_channel_state *state)
{
    if (!decoded || decoded->format != AV_SAMPLE_FMT_FLTP || decoded->ch_layout.nb_channels != 2 ||
        decoded->sample_rate <= 0 || decoded->nb_samples <= 0) {
        if (!output->audio_format_warned) {
            blog(LOG_WARNING, "Sports Replay: master replay decoder produced an unsupported audio format; muting audio");
            output->audio_format_warned = true;
        }
        return;
    }

    const uint32_t sample_rate = (uint32_t)decoded->sample_rate;
    uint32_t skip_frames = 0;
    if (media_timestamp_ns < state->playhead_ns) {
        const uint64_t delta_ns = state->playhead_ns - media_timestamp_ns;
        const uint64_t skip = ns_to_audio_frames(sample_rate, delta_ns);
        skip_frames = skip >= (uint64_t)decoded->nb_samples ? (uint32_t)decoded->nb_samples : (uint32_t)skip;
    }

    if (skip_frames >= (uint32_t)decoded->nb_samples)
        return;

    uint64_t start_ns = media_timestamp_ns + audio_frames_to_ns(sample_rate, skip_frames);
    if (start_ns >= state->out_ns)
        return;

    uint32_t frames = (uint32_t)decoded->nb_samples - skip_frames;
    const uint64_t full_end_ns = start_ns + audio_frames_to_ns(sample_rate, frames);
    if (full_end_ns > state->out_ns) {
        const uint64_t allowed = ns_to_audio_frames(sample_rate, state->out_ns - start_ns);
        if (!allowed)
            return;
        if (allowed < frames)
            frames = (uint32_t)allowed;
    }

    struct obs_source_audio audio = {0};
    audio.data[0] = decoded->extended_data[0] + (size_t)skip_frames * sizeof(float);
    audio.data[1] = decoded->extended_data[1] + (size_t)skip_frames * sizeof(float);
    audio.frames = frames;
    audio.speakers = SPEAKERS_STEREO;
    audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
    audio.samples_per_sec = sample_rate;

    if (start_ns >= output->audio_anchor_media_ns)
        audio.timestamp = output->audio_anchor_clock_ns + (start_ns - output->audio_anchor_media_ns);
    else
        audio.timestamp = output->audio_anchor_clock_ns;

    obs_source_output_audio(output->self, &audio);
    output->audio_queued_until_ns = start_ns + audio_frames_to_ns(sample_rate, frames);
}

static void output_master_audio(struct sr_event_output *output, const struct sr_replay_channel_state *state,
                                uint64_t clock_ns)
{
    if (!master_audio_allowed(output, state)) {
        reset_audio_transport(output);
        return;
    }

    bool resync = !output->audio_started || output->audio_event_id != state->event_id;
    if (!resync && clock_ns >= output->audio_anchor_clock_ns) {
        const uint64_t expected_media =
            output->audio_anchor_media_ns + (clock_ns - output->audio_anchor_clock_ns);
        if (abs_delta_u64(expected_media, state->playhead_ns) > AUDIO_RESYNC_NS)
            resync = true;
    }
    if (resync && !seek_audio_to_playhead(output, state, clock_ns))
        return;

    uint64_t target_ns = state->playhead_ns + AUDIO_LEAD_NS;
    if (target_ns < state->playhead_ns || target_ns > state->out_ns)
        target_ns = state->out_ns;

    for (unsigned i = 0; i < AUDIO_MAX_FRAMES_PER_TICK && output->audio_queued_until_ns < target_ns; i++) {
        AVFrame *decoded = NULL;
        uint64_t media_timestamp_ns = 0;
        if (!sr_master_audio_player_decode_next(output->audio_player, &decoded, &media_timestamp_ns) || !decoded)
            break;
        output_audio_frame(output, decoded, media_timestamp_ns, state);
        av_frame_free(&decoded);
    }
}

static void sr_event_output_update(void *data, obs_data_t *settings)
{
    struct sr_event_output *output = data;
    int bus = (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_BUS);
    if (bus < SR_REPLAY_BUS_A || bus >= SR_REPLAY_BUS_COUNT)
        bus = SR_REPLAY_BUS_A;

    int audio_mode = (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_AUDIO_MODE);
    if (audio_mode < SR_EVENT_OUTPUT_AUDIO_OFF || audio_mode > SR_EVENT_OUTPUT_AUDIO_MASTER)
        audio_mode = SR_EVENT_OUTPUT_AUDIO_MASTER;

    if (output->bus != (enum sr_replay_bus)bus || output->audio_mode != (enum sr_event_output_audio_mode)audio_mode)
        reset_audio_transport(output);
    output->bus = (enum sr_replay_bus)bus;
    output->audio_mode = (enum sr_event_output_audio_mode)audio_mode;
}

static void *sr_event_output_create(obs_data_t *settings, obs_source_t *source)
{
    struct sr_event_output *output = bzalloc(sizeof(*output));
    output->self = source;
    output->bus = SR_REPLAY_BUS_A;
    output->audio_mode = SR_EVENT_OUTPUT_AUDIO_MASTER;
    sr_event_output_update(output, settings);
    return output;
}

static void sr_event_output_destroy(void *data)
{
    struct sr_event_output *output = data;
    sr_master_audio_player_destroy(output->audio_player);
    bfree(output);
}

static void sr_event_output_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct sr_event_output *output = data;
    const uint64_t clock_ns = os_gettime_ns();

    AVFrame *decoded = NULL;
    bool ended = false;
    if (sr_replay_channel_render(output->bus, clock_ns, &decoded, NULL, &ended) && decoded) {
        output->width = (uint32_t)decoded->width;
        output->height = (uint32_t)decoded->height;
        output_avframe(output, decoded);
        av_frame_free(&decoded);
    }

    struct sr_replay_channel_state state;
    if (sr_replay_channel_get_state(output->bus, &state))
        output_master_audio(output, &state, clock_ns);
    else
        reset_audio_transport(output);

    if (ended)
        obs_source_media_ended(output->self);
}

static obs_properties_t *sr_event_output_properties(void *unused)
{
    UNUSED_PARAMETER(unused);
    obs_properties_t *props = obs_properties_create();
    obs_property_t *bus = obs_properties_add_list(props, SR_EVENT_OUTPUT_SETTING_BUS,
                                                  obs_module_text("EventOutput.Bus"), OBS_COMBO_TYPE_LIST,
                                                  OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(bus, obs_module_text("EventOutput.BusA"), SR_REPLAY_BUS_A);
    obs_property_list_add_int(bus, obs_module_text("EventOutput.BusB"), SR_REPLAY_BUS_B);

    obs_property_t *audio = obs_properties_add_list(props, SR_EVENT_OUTPUT_SETTING_AUDIO_MODE,
                                                    obs_module_text("EventOutput.Audio"), OBS_COMBO_TYPE_LIST,
                                                    OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(audio, obs_module_text("EventOutput.AudioOff"), SR_EVENT_OUTPUT_AUDIO_OFF);
    obs_property_list_add_int(audio, obs_module_text("EventOutput.AudioMaster"), SR_EVENT_OUTPUT_AUDIO_MASTER);
    obs_property_set_long_description(audio, obs_module_text("EventOutput.Audio.Description"));
    return props;
}

static void sr_event_output_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, SR_EVENT_OUTPUT_SETTING_BUS, SR_REPLAY_BUS_A);
    obs_data_set_default_int(settings, SR_EVENT_OUTPUT_SETTING_AUDIO_MODE, SR_EVENT_OUTPUT_AUDIO_MASTER);
}

static uint32_t sr_event_output_width(void *data)
{
    struct sr_event_output *output = data;
    if (output->width)
        return output->width;
    struct sr_replay_channel_state state;
    return sr_replay_channel_get_state(output->bus, &state) ? state.width : 0;
}

static uint32_t sr_event_output_height(void *data)
{
    struct sr_event_output *output = data;
    if (output->height)
        return output->height;
    struct sr_replay_channel_state state;
    return sr_replay_channel_get_state(output->bus, &state) ? state.height : 0;
}

struct obs_source_info sr_event_output_info = {
    .id = SR_EVENT_OUTPUT_ID,
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = sr_event_output_get_name,
    .create = sr_event_output_create,
    .destroy = sr_event_output_destroy,
    .update = sr_event_output_update,
    .get_defaults = sr_event_output_defaults,
    .get_properties = sr_event_output_properties,
    .video_tick = sr_event_output_tick,
    .get_width = sr_event_output_width,
    .get_height = sr_event_output_height,
};
''',
    encoding="utf-8",
)

cmake = Path("CMakeLists.txt")
replace_once(
    cmake,
    "    src/sr-master-audio-catalog.c\n    src/sr-session.c\n",
    "    src/sr-master-audio-catalog.c\n    src/sr-master-audio-player.c\n    src/sr-session.c\n",
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'EventOutput.BusB="B"\n',
    'EventOutput.BusB="B"\nEventOutput.Audio="Replay audio"\nEventOutput.AudioOff="Off"\nEventOutput.AudioMaster="Master replay audio"\nEventOutput.Audio.Description="Master audio currently plays only at 100% forward speed. Reverse and slow/fast replay are muted until varispeed/time-stretch support is added."\n',
)
