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

struct sr_master_audio_descriptor {
	uint32_t sequence;
	uint32_t flags;
	uint32_t sample_rate;
	uint64_t start_ns;
	uint64_t end_ns;
	bool active;
	char *audio_path;
	char *index_path;
};

/* Scans <session>/audio-master and includes both finalized .sraud pairs and a
 * flushed active .sraud.part/.sraidx.part pair. */
bool sr_master_audio_catalog_scan(const char *session_dir, struct sr_master_audio_descriptor **segments, size_t *count);
void sr_master_audio_catalog_free(struct sr_master_audio_descriptor *segments, size_t count);

const struct sr_master_audio_descriptor *sr_master_audio_catalog_find(const struct sr_master_audio_descriptor *segments,
								      size_t count, uint64_t timestamp_ns);

#ifdef __cplusplus
}
#endif
