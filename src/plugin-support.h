/*
 * Pitel Instant Replay - small build-generated support API
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

#define PLUGIN_WEBSITE "https://github.com/videolab666/obinstrp"

/* Prefixes one log message with the plugin name before forwarding to libobs. */
void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
