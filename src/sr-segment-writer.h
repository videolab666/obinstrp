/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <plugin-support.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_segment_writer;

struct sr_segment_writer_config {
	const char *session_dir;
	const char *camera_name;
	enum AVCodecID codec_id;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	const uint8_t *extradata;
	int extradata_size;
	uint32_t target_segment_ms;
	size_t max_queue_packets;
	uint64_t min_free_bytes;
};

struct sr_segment_writer_stats {
	uint64_t packets_written;
	uint64_t bytes_written;
	uint64_t packets_dropped;
	uint64_t segments_finalized;
	size_t queue_depth;
	size_t queue_high_watermark;
	bool write_failed;
	bool reserve_blocked;
};

/* Creates a per-camera asynchronous writer. Packets passed to push are cloned,
 * so the caller retains ownership and may immediately pass the original to the
 * legacy RAM replay buffer. */
struct sr_segment_writer *sr_segment_writer_create(const struct sr_segment_writer_config *config);
void sr_segment_writer_destroy(struct sr_segment_writer *writer);

/* Non-blocking with respect to disk I/O. Returns false if the packet could not
 * be queued. Each queued packet carries the enqueue continuity epoch, so if a
 * queue overflow creates a gap the writer notices it at the first packet after
 * that gap and resynchronizes on the next IDR without discarding older packets
 * that were already safely queued. */
bool sr_segment_writer_push_video(struct sr_segment_writer *writer, const AVPacket *pkt, uint64_t timestamp_ns);

void sr_segment_writer_get_stats(struct sr_segment_writer *writer, struct sr_segment_writer_stats *stats);

#ifdef __cplusplus
}
#endif
