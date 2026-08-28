/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

class QWidget;

/* Session-level offline exports. Both functions return immediately after the
 * destination folder is chosen; packet-copy remuxing runs on a worker thread
 * and owns its progress/cancel dialog. Active Recording Sessions are rejected
 * so the exported set has stable physical media bounds. */
void sr_session_export_all_clips(QWidget *parent, const char *session_dir);
void sr_session_export_iso(QWidget *parent, const char *session_dir);
