from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


fmt = Path("src/sr-audio-format.h")
replace_once(
    fmt,
    '''\tuint32_t bit_rate;\n\tuint32_t flags;\n\tuint64_t segment_start_ns;\n''',
    '''\tuint32_t bit_rate;\n\tuint32_t flags;\n\tuint32_t sequence;\n\tuint64_t segment_start_ns;\n''',
)
replace_once(
    fmt,
    '''\tuint32_t version;\n\tuint32_t reserved;\n\tuint64_t segment_start_ns;\n''',
    '''\tuint32_t version;\n\tuint32_t sequence;\n\tuint64_t segment_start_ns;\n''',
)
replace_once(
    fmt,
    '''static_assert(sizeof(struct sr_audio_file_header) == 48, "unexpected sr_audio_file_header layout");''',
    '''static_assert(sizeof(struct sr_audio_file_header) == 52, "unexpected sr_audio_file_header layout");''',
)
replace_once(
    fmt,
    '''_Static_assert(sizeof(struct sr_audio_file_header) == 48, "unexpected sr_audio_file_header layout");''',
    '''_Static_assert(sizeof(struct sr_audio_file_header) == 52, "unexpected sr_audio_file_header layout");''',
)

src = Path("src/sr-master-audio.c")
replace_once(
    src,
    '''\tchar *index_final_path;\n\tuint64_t segment_start_ns;\n\n\tstruct sr_master_audio_stats stats;\n''',
    '''\tchar *index_final_path;\n\tuint64_t segment_start_ns;\n\tuint64_t last_flush_ns;\n\n\tstruct sr_master_audio_stats stats;\n''',
)
replace_once(
    src,
    '''\tclear_segment_paths(state);\n\tstate->segment_start_ns = 0;\n}\n''',
    '''\tclear_segment_paths(state);\n\tstate->segment_start_ns = 0;\n\tstate->last_flush_ns = 0;\n}\n''',
)
replace_once(
    src,
    '''\theader.bit_rate = (uint32_t)state->encoder->bit_rate;\n\theader.flags = state->next_segment_discontinuity ? SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY : 0;\n\theader.segment_start_ns = start_ns;\n''',
    '''\theader.bit_rate = (uint32_t)state->encoder->bit_rate;\n\theader.flags = state->next_segment_discontinuity ? SR_AUDIO_SEGMENT_FLAG_DISCONTINUITY : 0;\n\theader.sequence = sequence;\n\theader.segment_start_ns = start_ns;\n''',
)
replace_once(
    src,
    '''\tindex.version = SR_AUDIO_FORMAT_VERSION;\n\tindex.segment_start_ns = start_ns;\n''',
    '''\tindex.version = SR_AUDIO_FORMAT_VERSION;\n\tindex.sequence = sequence;\n\tindex.segment_start_ns = start_ns;\n''',
)
replace_once(
    src,
    '''\tstate->segment_start_ns = start_ns;\n\tstate->next_segment_discontinuity = false;\n''',
    '''\tstate->segment_start_ns = start_ns;\n\tstate->last_flush_ns = start_ns;\n\tstate->next_segment_discontinuity = false;\n''',
)
replace_once(
    src,
    '''\tstats_add_packet(state, sizeof(header) + (uint64_t)packet->size + sizeof(index));\n\treturn true;\n}\n''',
    '''\tstats_add_packet(state, sizeof(header) + (uint64_t)packet->size + sizeof(index));\n\n\t/* Keep the active .part pair readable for near-live replay. Audio has no\n\t * video-style IDR boundary, so publish complete packet/index records about\n\t * four times per second. */\n\tif (timestamp_ns >= state->last_flush_ns && timestamp_ns - state->last_flush_ns >= 250000000ULL) {\n\t\tfflush(state->audio_file);\n\t\tfflush(state->index_file);\n\t\tstate->last_flush_ns = timestamp_ns;\n\t}\n\treturn true;\n}\n''',
)
