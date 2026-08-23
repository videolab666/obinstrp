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

#ifdef __cplusplus
extern "C" {
#endif

struct sr_master_audio_stats {
	uint64_t chunks_received;
	uint64_t chunks_dropped;
	uint64_t packets_written;
	uint64_t bytes_written;
	uint64_t segments_finalized;
	size_t queue_depth;
	size_t queue_high_watermark;
	bool encoder_failed;
	bool write_failed;
};

/* Registers one raw callback on OBS mix 0 and starts the non-realtime worker.
 * The callback remains idle until at least one continuous camera recorder
 * acquires master audio. Requested conversion is AAC-friendly 48 kHz stereo
 * float planar. */
bool sr_master_audio_init(void);
void sr_master_audio_free(void);

/* Reference-counted by active disk camera writers. The first acquire starts a
 * session-backed master timeline; the last release drains/finalizes it. */
bool sr_master_audio_acquire(void);
void sr_master_audio_release(void);

void sr_master_audio_get_stats(struct sr_master_audio_stats *stats);

#ifdef __cplusplus
}
#endif
