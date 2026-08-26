/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "sr-storage-cleanup.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process-wide background storage policy worker. It performs potentially slow
 * directory/index scanning outside capture, render and Qt threads. */
bool sr_storage_manager_start(void);
void sr_storage_manager_stop(void);

struct sr_storage_manager_status {
	uint64_t cleanup_passes;
	uint64_t last_cleanup_unix;
	struct sr_storage_cleanup_result last_cleanup;
};

/* Snapshot of automatic low-space cleanup activity for the operator UI. */
void sr_storage_manager_get_status(struct sr_storage_manager_status *status);

#ifdef __cplusplus
}
#endif
