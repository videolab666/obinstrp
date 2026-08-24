/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CAMERA_STABLE_KEY_MAX 64

/* OBS source UUIDs are the persistent camera identity. Display names remain
 * operator-facing labels only and may be renamed without moving new replay
 * media to another directory. */
bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size);
bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size);

/* Reads the persisted Sports Replay Capture filter timing correction.
 * Positive means camera media timestamps are late and must be addressed as
 * global_timestamp + offset. Ambiguous duplicate capture filters fail closed. */
bool sr_camera_sync_offset_ns(const char *camera_name, int64_t *offset_ns);

uint32_t sr_camera_key_hash(const char *key);

/* Returned paths use bmalloc/bstrdup semantics and must be released with
 * bfree(). The legacy directory helper preserves read compatibility with
 * recordings created before UUID-backed storage was introduced. */
char *sr_camera_directory_for_key(const char *session_dir, const char *key);
char *sr_camera_legacy_directory(const char *session_dir, const char *camera_name);

#ifdef __cplusplus
}
#endif
