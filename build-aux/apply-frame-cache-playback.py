#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str, label: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"{path}: expected anchor not found: {label}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


path = "src/playback-source.c"

replace_once(
    path,
    "#define SR_EMPTY_BOUNCE_SEC 0.5f\n",
    "#define SR_EMPTY_BOUNCE_SEC 0.5f\n\n"
    "/* A 192 MiB bound keeps roughly one 1080p60 one-second GOP worth of\n"
    " * NV12/YUV420 pictures hot without allowing reverse/jog caching to grow\n"
    " * with replay duration. At 4K the same byte budget naturally retains a\n"
    " * much smaller window. */\n"
    "#define SR_REPLAY_FRAME_CACHE_BYTES (192ULL * 1024ULL * 1024ULL)\n",
    "frame cache budget",
)

replace_once(
    path,
    "\tstruct sr_decoder *decoder;\n"
    "\tint64_t cur_idx; /* index of last packet represented in decoder reference state */\n",
    "\tstruct sr_decoder *decoder;\n"
    "\tstruct sr_frame_cache frame_cache;\n"
    "\tint64_t cur_idx; /* index of last packet represented in decoder reference state */\n"
    "\tint64_t display_idx; /* index of the picture most recently sent to OBS */\n",
    "playback cache state",
)

replace_once(
    path,
    "\tp->replay = *replay;\n"
    "\tp->have_replay = true;\n"
    "\tp->end_action_override = end_action_override;\n"
    "\tp->bounce_countdown = 0.0f;\n\n"
    "\tsr_decoder_destroy(p->decoder);\n",
    "\tp->replay = *replay;\n"
    "\tp->have_replay = true;\n"
    "\tp->end_action_override = end_action_override;\n"
    "\tp->bounce_countdown = 0.0f;\n"
    "\tp->cur_idx = -1;\n"
    "\tp->display_idx = -1;\n"
    "\tsr_frame_cache_clear(&p->frame_cache);\n\n"
    "\tsr_decoder_destroy(p->decoder);\n",
    "clear cache on replay install",
)

replace_once(
    path,
    "\tif (!sr_replay_decode_frame_at(p->decoder, &p->replay, &p->cur_idx, idx, &decoded))\n"
    "\t\treturn;\n\n"
    "\toutput_avframe(p, decoded);\n",
    "\tif (!sr_replay_decode_frame_at(p->decoder, &p->replay, &p->frame_cache, &p->cur_idx, idx, &decoded))\n"
    "\t\treturn;\n\n"
    "\toutput_avframe(p, decoded);\n"
    "\tp->display_idx = (int64_t)idx;\n",
    "cache-aware frame output",
)

replace_once(
    path,
    "\t\tp->cur_idx = -1;\n"
    "\t\tp->audio_idx = 0;\n"
    "\t\tp->playhead = p->backward ? p->replay.last_ts : p->replay.first_ts;\n"
    "\t\tp->phase = PHASE_REPLAY;\n",
    "\t\tp->cur_idx = -1;\n"
    "\t\tp->display_idx = -1;\n"
    "\t\tp->audio_idx = 0;\n"
    "\t\tp->playhead = p->backward ? p->replay.last_ts : p->replay.first_ts;\n"
    "\t\tp->phase = PHASE_REPLAY;\n",
    "sequence display reset",
)

replace_once(
    path,
    "static void sr_playback_begin_replay_phase(struct sr_playback *p)\n"
    "{\n"
    "\tp->cur_idx = -1;\n"
    "\tp->audio_idx = 0;\n",
    "static void sr_playback_begin_replay_phase(struct sr_playback *p)\n"
    "{\n"
    "\tp->cur_idx = -1;\n"
    "\tp->display_idx = -1;\n"
    "\tp->audio_idx = 0;\n",
    "replay phase display reset",
)

replace_once(
    path,
    "\tif ((int64_t)idx != p->cur_idx)\n"
    "\t\tsr_playback_output_frame_at(p, idx);\n",
    "\tif ((int64_t)idx != p->display_idx)\n"
    "\t\tsr_playback_output_frame_at(p, idx);\n",
    "compare displayed frame rather than decoder state",
)

replace_once(
    path,
    "\t\t\tp->audio_idx = 0;\n"
    "\t\t\tp->cur_idx = -1;\n"
    "\t\t} else if (p->outro_clip) {\n",
    "\t\t\tp->audio_idx = 0;\n"
    "\t\t\tp->cur_idx = -1;\n"
    "\t\t\tp->display_idx = -1;\n"
    "\t\t} else if (p->outro_clip) {\n",
    "loop display reset",
)

replace_once(
    path,
    "\tp->speed_percent = 100.0;\n"
    "\tp->cur_idx = -1;\n"
    "\tp->end_action_override = -1;\n"
    "\tpthread_mutex_init(&p->mutex, NULL);\n",
    "\tp->speed_percent = 100.0;\n"
    "\tp->cur_idx = -1;\n"
    "\tp->display_idx = -1;\n"
    "\tp->end_action_override = -1;\n"
    "\tsr_frame_cache_init(&p->frame_cache, (size_t)SR_REPLAY_FRAME_CACHE_BYTES);\n"
    "\tpthread_mutex_init(&p->mutex, NULL);\n",
    "initialize frame cache",
)

replace_once(
    path,
    "\tif (p->have_replay)\n"
    "\t\tsr_replay_free(&p->replay);\n"
    "\tsr_decoder_destroy(p->decoder);\n",
    "\tif (p->have_replay)\n"
    "\t\tsr_replay_free(&p->replay);\n"
    "\tsr_decoder_destroy(p->decoder);\n"
    "\tsr_frame_cache_free(&p->frame_cache);\n",
    "free frame cache",
)
