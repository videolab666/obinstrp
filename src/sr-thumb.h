/*
 * Pitel Instant Replay - thumbnail service interface
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sr_thumbnail_rgba(const char *path, int width, int height, uint8_t **rgba);
bool sr_disk_thumbnail_rgba(const char *session_dir, const char *camera_name, uint64_t timestamp_ns, int width,
			    int height, uint8_t **rgba);

#ifdef __cplusplus
}
#endif
