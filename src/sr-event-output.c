/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-output.h"

#include "sr-replay-channel.h"

#include <media-io/video-io.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

struct sr_event_output {
	obs_source_t *self;
	enum sr_replay_bus bus;
	uint32_t width;
	uint32_t height;
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

static void sr_event_output_update(void *data, obs_data_t *settings)
{
	struct sr_event_output *output = data;
	int bus = (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_BUS);
	if (bus < SR_REPLAY_BUS_A || bus >= SR_REPLAY_BUS_COUNT)
		bus = SR_REPLAY_BUS_A;
	output->bus = (enum sr_replay_bus)bus;
}

static void *sr_event_output_create(obs_data_t *settings, obs_source_t *source)
{
	struct sr_event_output *output = bzalloc(sizeof(*output));
	output->self = source;
	output->bus = SR_REPLAY_BUS_A;
	sr_event_output_update(output, settings);
	return output;
}

static void sr_event_output_destroy(void *data)
{
	bfree(data);
}

static void sr_event_output_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct sr_event_output *output = data;

	AVFrame *decoded = NULL;
	uint64_t media_timestamp_ns = 0;
	bool ended = false;
	if (!sr_replay_channel_render(output->bus, os_gettime_ns(), &decoded, &media_timestamp_ns, &ended) || !decoded)
		return;

	output->width = (uint32_t)decoded->width;
	output->height = (uint32_t)decoded->height;
	output_avframe(output, decoded);
	av_frame_free(&decoded);

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
	return props;
}

static void sr_event_output_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SR_EVENT_OUTPUT_SETTING_BUS, SR_REPLAY_BUS_A);
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
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
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
