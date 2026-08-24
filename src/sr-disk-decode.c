/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-disk-decode.h"
#include "sr-codec.h"
#include "sr-segment-catalog.h"
#include "sr-segment-reader.h"

#include <obs-module.h>

bool sr_disk_decode_frame_at(const char *session_dir, const char *camera_name, uint64_t target_ns, AVFrame **frame,
			     uint64_t *actual_timestamp_ns)
{
	if (!frame)
		return false;
	*frame = NULL;
	if (actual_timestamp_ns)
		*actual_timestamp_ns = 0;

	struct sr_segment_descriptor *segments = NULL;
	size_t segment_count = 0;
	if (!sr_segment_catalog_scan(session_dir, camera_name, &segments, &segment_count))
		return false;

	const struct sr_segment_descriptor *segment = sr_segment_catalog_find(segments, segment_count, target_ns);
	if (!segment) {
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	struct sr_segment_reader *reader = sr_segment_reader_open(segment->segment_path, segment->index_path);
	if (!reader) {
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	/* The active .part may have grown since the catalog scan. Refresh once so
	 * the target lookup uses the newest fully indexed packet snapshot. */
	if (segment->active)
		sr_segment_reader_refresh_index(reader);

	struct sr_segment_stream_info info;
	if (!sr_segment_reader_get_info(reader, &info)) {
		sr_segment_reader_close(reader);
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	size_t target_pos = 0;
	struct sr_index_entry target_entry;
	if (!sr_segment_reader_find_position(reader, target_ns, false, &target_pos, &target_entry)) {
		sr_segment_reader_close(reader);
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	size_t key_pos = 0;
	if (!sr_segment_reader_find_position(reader, target_entry.timestamp_ns, true, &key_pos, NULL)) {
		sr_segment_reader_close(reader);
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	struct sr_decoder *decoder = sr_decoder_create(info.codec_id, info.extradata, info.extradata_size);
	if (!decoder) {
		sr_segment_reader_close(reader);
		sr_segment_catalog_free(segments, segment_count);
		return false;
	}

	AVFrame *result = NULL;
	uint64_t result_ts = 0;

	for (size_t pos = key_pos; pos <= target_pos; pos++) {
		struct sr_index_entry entry;
		if (!sr_segment_reader_entry_at(reader, pos, &entry))
			break;

		AVPacket *packet = NULL;
		uint64_t packet_ts = 0;
		if (!sr_segment_reader_read_video_packet(reader, &entry, &packet, &packet_ts, NULL))
			break;

		AVFrame *decoded = NULL;
		const bool got_frame = sr_decoder_decode(decoder, packet, &decoded);
		av_packet_free(&packet);
		if (!got_frame || !decoded)
			continue;

		/* Replay storage deliberately forbids B-frames, so decoded frame order
		 * follows packet/index order. Keep replacing the candidate until the
		 * requested index is reached. */
		av_frame_free(&result);
		result = av_frame_clone(decoded);
		if (!result)
			break;
		result_ts = packet_ts;
	}

	sr_decoder_destroy(decoder);
	sr_segment_reader_close(reader);
	sr_segment_catalog_free(segments, segment_count);

	if (!result)
		return false;

	*frame = result;
	if (actual_timestamp_ns)
		*actual_timestamp_ns = result_ts;
	return true;
}
