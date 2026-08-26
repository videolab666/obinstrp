from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")

replace_once(
    "src/sr-capture.h",
    "\tuint64_t packets_written;\n\tuint64_t bytes_written;\n\tuint64_t recording_duration_ns;\n",
    "\tuint64_t packets_written;\n\tuint64_t bytes_written;\n\t/* Video-clock timestamp when the current REC run began. Zero while REC is\n\t * off. Consumers use this stable boundary instead of a periodically\n\t * published duration when an Event must not extend before recording began. */\n\tuint64_t recording_start_ns;\n\tuint64_t recording_duration_ns;\n",
)

replace_once(
    "src/capture-filter.c",
    "\t\t.failed_count = (c->writer_failed || c->encoder_failed) ? 1 : 0,\n\t\t.recording_duration_ns = c->disk_recording && c->recording_start_ns &&\n",
    "\t\t.failed_count = (c->writer_failed || c->encoder_failed) ? 1 : 0,\n\t\t.recording_start_ns = c->disk_recording ? c->recording_start_ns : 0,\n\t\t.recording_duration_ns = c->disk_recording && c->recording_start_ns &&\n",
)

replace_once(
    "src/capture-filter.c",
    "\tc->status.requested_count = c->disk_recording ? 1 : 0;\n\tc->performance_status.disk_requested = c->disk_recording;\n",
    "\tc->status.requested_count = c->disk_recording ? 1 : 0;\n\tc->status.recording_start_ns = c->disk_recording ? c->recording_start_ns : 0;\n\tif (!c->disk_recording)\n\t\tc->status.recording_duration_ns = 0;\n\tc->performance_status.disk_requested = c->disk_recording;\n",
)

replace_once(
    "src/capture-filter.c",
    "\t\tctx->summary->packets_written += status.packets_written;\n\t\tctx->summary->bytes_written += status.bytes_written;\n\t\tif (status.recording_duration_ns > ctx->summary->recording_duration_ns)\n",
    "\t\tctx->summary->packets_written += status.packets_written;\n\t\tctx->summary->bytes_written += status.bytes_written;\n\t\tif (status.recording_start_ns &&\n\t\t    (!ctx->summary->recording_start_ns || status.recording_start_ns < ctx->summary->recording_start_ns))\n\t\t\tctx->summary->recording_start_ns = status.recording_start_ns;\n\t\tif (status.recording_duration_ns > ctx->summary->recording_duration_ns)\n",
)

replace_once(
    "src/sr-program-recorder.c",
    "\tif (g_program.recording_requested && g_program.recording_start_ns) {\n\t\tconst uint64_t now = obs_get_video_frame_time();\n",
    "\tif (g_program.recording_requested && g_program.recording_start_ns) {\n\t\tif (!summary->recording_start_ns || g_program.recording_start_ns < summary->recording_start_ns)\n\t\t\tsummary->recording_start_ns = g_program.recording_start_ns;\n\t\tconst uint64_t now = obs_get_video_frame_time();\n",
)

replace_once(
    "src/sr-event-controller.c",
    "#include \"sr-camera-identity.h\"\n#include \"sr-event-db.h\"\n",
    "#include \"sr-camera-identity.h\"\n#include \"sr-capture.h\"\n#include \"sr-event-db.h\"\n",
)

replace_once(
    "src/sr-event-controller.c",
    "\tconst uint64_t in_ns = now_ns > pre_roll_ns ? now_ns - pre_roll_ns : 0;\n\tconst uint64_t out_ns = now_ns + post_roll_ns;\n",
    "\t/* A -5/-10/-20 quick mark is a request for up to that much recorded\n\t * history, not permission to invent time before REC began. Keep the Event\n\t * itself inside the current recording run so its stored/list duration,\n\t * replay bounds and export bounds all agree from the moment it is created.\n\t * recording_start_ns is a stable video-clock boundary; unlike the published\n\t * duration it does not lag the producer status refresh interval. */\n\tuint64_t effective_pre_roll_ns = pre_roll_ns;\n\tstruct sr_capture_recording_summary recording = {0};\n\tif (sr_capture_get_recording_summary(&recording) && recording.recording_start_ns) {\n\t\tconst uint64_t available_ns =\n\t\t\tnow_ns > recording.recording_start_ns ? now_ns - recording.recording_start_ns : 0;\n\t\tif (effective_pre_roll_ns > available_ns)\n\t\t\teffective_pre_roll_ns = available_ns;\n\t}\n\n\tconst uint64_t in_ns = now_ns > effective_pre_roll_ns ? now_ns - effective_pre_roll_ns : 0;\n\tconst uint64_t out_ns = now_ns + post_roll_ns;\n",
)

print("quick-mark duration patch applied")
