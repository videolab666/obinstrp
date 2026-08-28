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

/* libobs provides blogva at final plugin link time. The generated support
 * helper is deliberately header-independent so the OBS template can compile
 * it before libobs include paths are attached to the plugin target. */
extern void blogva(int log_level, const char *format, va_list args);
void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
