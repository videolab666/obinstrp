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

#ifdef __cplusplus
extern "C" {
#endif

bool sr_media_guard_init(void);
void sr_media_guard_free(void);
void sr_media_guard_lock(void);
void sr_media_guard_unlock(void);

#ifdef __cplusplus
}
#endif
