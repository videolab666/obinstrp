/*
Pitel Instant Replay
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

/* Global, process-local replay session. The session directory is created
 * lazily when the first continuous recorder needs it. Returned strings are
 * bstrdup allocations owned by the caller. */
void sr_session_init(void);
void sr_session_free(void);

char *sr_session_get_or_create_path(void);
char *sr_session_get_id(void);

/* Non-creating identity check used by background maintenance. It prevents a
 * recovery/retention scan from touching the session currently being written. */
bool sr_session_path_is_active(const char *path);

#ifdef __cplusplus
}
#endif
