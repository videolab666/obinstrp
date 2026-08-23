/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-decode.h"

static bool find_previous_keyframe(const struct sr_replay *replay, size_t target_index, size_t *keyframe_index)
{
	if (!replay || !keyframe_index || target_index >= replay->video.num)
		return false;

	size_t index = target_index;
	for (;;) {
		const AVPacket *pkt = replay->video.array[index].pkt;
		if (pkt && (pkt->flags & AV_PKT_FLAG_KEY)) {
			*keyframe_index = index;
			return true;
		}
		if (index == 0)
			break;
		index--;
	}
	return false;
}

bool sr_replay_decode_frame_at(struct sr_decoder *decoder, const struct sr_replay *replay, struct sr_frame_cache *cache,
			       int64_t *current_index, size_t target_index, AVFrame **frame)
{
	if (!decoder || !replay || !current_index || !frame || target_index >= replay->video.num)
		return false;
	*frame = NULL;

	/* A backward/jog target that is still in the decoded window can be shown
	 * immediately. Do not move current_index backwards: it describes decoder
	 * reference state, not the last picture shown on screen. Keeping the newer
	 * state warm is useful if the operator changes direction again. */
	if (cache) {
		AVFrame *cached = sr_frame_cache_find(cache, (uint64_t)target_index);
		if (cached) {
			*frame = cached;
			return true;
		}
	}

	size_t start_index = 0;
	const bool can_continue_forward = *current_index >= 0 && (size_t)*current_index < target_index;
	if (can_continue_forward) {
		start_index = (size_t)*current_index + 1;
	} else {
		if (!find_previous_keyframe(replay, target_index, &start_index))
			return false;
		sr_decoder_flush(decoder);
		*current_index = -1;
	}

	AVFrame *target_frame = NULL;
	for (size_t index = start_index; index <= target_index; index++) {
		AVPacket *pkt = replay->video.array[index].pkt;
		if (!pkt)
			return false;

		AVFrame *decoded = NULL;
		if (!sr_decoder_decode(decoder, pkt, &decoded) || !decoded)
			return false;

		*current_index = (int64_t)index;
		if (cache)
			sr_frame_cache_store(cache, (uint64_t)index, decoded);
		if (index == target_index)
			target_frame = decoded;
	}

	if (!target_frame)
		return false;

	*frame = target_frame;
	return true;
}
