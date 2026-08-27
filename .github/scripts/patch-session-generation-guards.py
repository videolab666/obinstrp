from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel, text):
    (ROOT / rel).write_text(text, encoding="utf-8", newline="\n")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


# Make the native OBS clock explicit in legacy producer-local bookkeeping.
rel = "src/capture-filter.c"
text = read(rel)
text = replace_once(text, "\tuint64_t recording_start_ns;", "\tuint64_t recording_obs_start_ns;",
                    "capture native start field")
text = text.replace("c->recording_start_ns", "c->recording_obs_start_ns")
write(rel, text)

rel = "src/sr-program-recorder.c"
text = read(rel)
text = replace_once(text, "\tuint64_t recording_start_ns;", "\tuint64_t recording_obs_start_ns;",
                    "program native start field")
text = text.replace("g_program.recording_start_ns", "g_program.recording_obs_start_ns")
write(rel, text)

# A zero-valued first Run is valid. Clamp pre-roll without treating the numeric
# start timestamp itself as an active/inactive sentinel.
rel = "src/sr-event-controller.c"
text = read(rel)
old = """\tuint64_t effective_pre_roll_ns = pre_roll_ns;\n\tconst uint64_t recording_start = sr_session_recording_start_ns();\n\tif (recording_start) {\n\t\tconst uint64_t available_ns = mapped_now > recording_start ? mapped_now - recording_start : 0;\n\t\tif (effective_pre_roll_ns > available_ns)\n\t\t\teffective_pre_roll_ns = available_ns;\n\t}\n"""
new = """\tuint64_t effective_pre_roll_ns = pre_roll_ns;\n\tconst uint64_t recording_start = sr_session_recording_start_ns();\n\tconst uint64_t available_ns = mapped_now > recording_start ? mapped_now - recording_start : 0;\n\tif (effective_pre_roll_ns > available_ns)\n\t\teffective_pre_roll_ns = available_ns;\n"""
text = replace_once(text, old, new, "quick mark zero start")
write(rel, text)

# Every video writer belongs to exactly one Recording Run generation. Even if
# a future late callback survives a STOP/START boundary, it cannot enqueue into
# a writer created for another Run.
rel = "src/sr-segment-writer.c"
text = read(rel)
text = replace_once(text, "\tuint64_t enqueue_epoch;\n\tuint64_t write_epoch;",
                    "\tuint64_t enqueue_epoch;\n\tuint64_t recording_generation;\n\tuint64_t write_epoch;",
                    "segment writer generation field")
text = replace_once(text, "\tw->initial_discontinuity = config->start_discontinuity;\n",
                    "\tw->initial_discontinuity = config->start_discontinuity;\n"
                    "\tw->recording_generation = sr_session_recording_generation();\n",
                    "segment writer capture generation")
text = replace_once(text,
                    "\tif (!w || !pkt || pkt->size <= 0)\n\t\treturn false;\n\tAVPacket *clone = av_packet_clone(pkt);",
                    "\tif (!w || !pkt || pkt->size <= 0)\n\t\treturn false;\n"
                    "\tif (!sr_session_recording_is_active() ||\n"
                    "\t    sr_session_recording_generation() != w->recording_generation) {\n"
                    "\t\tpthread_mutex_lock(&w->mutex);\n"
                    "\t\tw->stats.packets_dropped++;\n"
                    "\t\tpthread_mutex_unlock(&w->mutex);\n"
                    "\t\treturn false;\n"
                    "\t}\n\tAVPacket *clone = av_packet_clone(pkt);",
                    "segment writer generation guard")
write(rel, text)

# Per-camera audio writers get the same generation ownership as video. Master
# audio already carries a generation and refuses cross-session rebinding.
rel = "src/sr-master-audio.c"
text = read(rel)
text = replace_once(text,
                    "\tstate->next_segment_discontinuity = sr_session_recording_starts_with_discontinuity();\n"
                    "\tstate->active = true;\n\tstate->active_refs = 1;",
                    "\tstate->next_segment_discontinuity = sr_session_recording_starts_with_discontinuity();\n"
                    "\tstate->recording_generation = sr_session_recording_generation();\n"
                    "\tstate->active = true;\n\tstate->active_refs = 1;",
                    "camera audio capture generation")
text = replace_once(text,
                    "\tif (!writer || !writer->state || !audio || !audio->frames || !audio->data[0])\n"
                    "\t\treturn false;\n\tconst uint8_t *right = channels > 1 ? audio->data[1] : audio->data[0];",
                    "\tif (!writer || !writer->state || !audio || !audio->frames || !audio->data[0])\n"
                    "\t\treturn false;\n"
                    "\tif (!sr_session_recording_is_active() ||\n"
                    "\t    writer->state->recording_generation != sr_session_recording_generation())\n"
                    "\t\treturn false;\n"
                    "\tconst uint8_t *right = channels > 1 ? audio->data[1] : audio->data[0];",
                    "camera audio generation guard")
write(rel, text)

# Remove a no-longer-used compatibility helper: Resume now consumes physical
# bounds directly and never recording_runs.timeline_end_ns.
rel = "src/sr-session.c"
text = read(rel)
old = """static uint64_t last_media_timestamp(const char *session_dir)\n{\n\tuint64_t start_ns = 0;\n\tuint64_t end_ns = 0;\n\treturn sr_session_get_media_bounds(session_dir, &start_ns, &end_ns) ? end_ns : 0;\n}\n\n"""
text = replace_once(text, old, "", "unused last media helper")
write(rel, text)

print("Session generation guards applied successfully")
