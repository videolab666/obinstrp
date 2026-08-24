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

#ifdef __cplusplus
extern "C" {
#endif

struct sr_camera_list {
	char **names;
	size_t count;
};

/* Enumerates OBS parent sources that currently own a Sports Replay capture
 * filter. The returned names are ordered by persistent OBS source UUID rather
 * than by display name, so renaming a camera does not silently remap the Qt
 * CAM buttons or Angle 1..8 hardware hotkeys. */
bool sr_camera_list_capture(struct sr_camera_list *list);
void sr_camera_list_free(struct sr_camera_list *list);

#ifdef __cplusplus
}
#endif
