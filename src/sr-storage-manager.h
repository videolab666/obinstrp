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

/* Process-wide background storage policy worker. It performs potentially slow
 * directory/index scanning outside capture, render and Qt threads. */
bool sr_storage_manager_start(void);
void sr_storage_manager_stop(void);

#ifdef __cplusplus
}
#endif
