from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


header = Path("src/sr-master-audio.h")
replace_once(
    header,
    '''\tuint64_t packets_written;\n\tuint64_t bytes_written;\n\tuint64_t segments_finalized;\n''',
    '''\tuint64_t packets_written;\n\tuint64_t packets_dropped;\n\tuint64_t bytes_written;\n\tuint64_t segments_finalized;\n''',
)
replace_once(
    header,
    '''\tbool encoder_failed;\n\tbool write_failed;\n''',
    '''\tbool encoder_failed;\n\tbool write_failed;\n\tbool reserve_blocked;\n''',
)

source = Path("src/sr-master-audio.c")
replace_once(
    source,
    '''\tchar *audio_dir;\n\tuint64_t target_segment_ns;\n\tuint32_t next_sequence;\n''',
    '''\tchar *audio_dir;\n\tuint64_t target_segment_ns;\n\tuint64_t min_free_bytes;\n\tbool reserve_blocked;\n\tuint64_t reserve_recheck_after_ns;\n\tuint32_t next_sequence;\n''',
)
replace_once(
    source,
    '''static void stats_add_packet(struct sr_master_audio_state *state, uint64_t bytes)\n{\n\tpthread_mutex_lock(&state->mutex);\n\tstate->stats.packets_written++;\n\tstate->stats.bytes_written += bytes;\n\tpthread_mutex_unlock(&state->mutex);\n}\n''',
    '''static void stats_add_packet(struct sr_master_audio_state *state, uint64_t bytes)\n{\n\tpthread_mutex_lock(&state->mutex);\n\tstate->stats.packets_written++;\n\tstate->stats.bytes_written += bytes;\n\tpthread_mutex_unlock(&state->mutex);\n}\n\nstatic void stats_add_packet_drop(struct sr_master_audio_state *state)\n{\n\tpthread_mutex_lock(&state->mutex);\n\tstate->stats.packets_dropped++;\n\tpthread_mutex_unlock(&state->mutex);\n}\n\nstatic void stats_set_reserve_blocked(struct sr_master_audio_state *state, bool blocked)\n{\n\tpthread_mutex_lock(&state->mutex);\n\tstate->stats.reserve_blocked = blocked;\n\tpthread_mutex_unlock(&state->mutex);\n}\n\nstatic bool storage_reserve_allows(struct sr_master_audio_state *state, uint64_t timestamp_ns)\n{\n\tif (!state->min_free_bytes) {\n\t\tif (state->reserve_blocked) {\n\t\t\tstate->reserve_blocked = false;\n\t\t\tstate->reserve_recheck_after_ns = 0;\n\t\t\tstats_set_reserve_blocked(state, false);\n\t\t}\n\t\treturn true;\n\t}\n\n\tif (state->reserve_blocked && timestamp_ns < state->reserve_recheck_after_ns)\n\t\treturn false;\n\n\tconst uint64_t free_bytes = state->audio_dir ? os_get_free_disk_space(state->audio_dir) : 0;\n\tif (free_bytes < state->min_free_bytes) {\n\t\tif (!state->reserve_blocked) {\n\t\t\tblog(LOG_ERROR,\n\t\t\t     "Sports Replay: master replay audio paused: disk free space %.1f GB is below the %.1f GB reserve",\n\t\t\t     (double)free_bytes / (1024.0 * 1024.0 * 1024.0),\n\t\t\t     (double)state->min_free_bytes / (1024.0 * 1024.0 * 1024.0));\n\t\t}\n\t\tstate->reserve_blocked = true;\n\t\tstate->reserve_recheck_after_ns = timestamp_ns + 1000000000ULL;\n\t\tstate->next_segment_discontinuity = true;\n\t\tstats_set_reserve_blocked(state, true);\n\t\treturn false;\n\t}\n\n\tif (state->reserve_blocked) {\n\t\tblog(LOG_INFO, "Sports Replay: disk reserve restored; master replay audio resumed");\n\t\tstate->reserve_blocked = false;\n\t\tstate->reserve_recheck_after_ns = 0;\n\t\tstate->next_segment_discontinuity = true;\n\t\tstats_set_reserve_blocked(state, false);\n\t}\n\treturn true;\n}\n''',
)
replace_once(
    source,
    '''\tif (!state->audio_file && !open_segment(state, timestamp_ns))\n\t\treturn false;\n''',
    '''\tif (!state->audio_file) {\n\t\tif (!storage_reserve_allows(state, timestamp_ns)) {\n\t\t\tstats_add_packet_drop(state);\n\t\t\treturn true;\n\t\t}\n\t\tif (!open_segment(state, timestamp_ns))\n\t\t\treturn false;\n\t}\n''',
)
replace_once(
    source,
    '''\tstate->have_write_epoch = false;\n\tstate->next_segment_discontinuity = false;\n\tstate->encoder_failed_session = false;\n}\n''',
    '''\tstate->have_write_epoch = false;\n\tstate->next_segment_discontinuity = false;\n\tstate->reserve_blocked = false;\n\tstate->reserve_recheck_after_ns = 0;\n\tstats_set_reserve_blocked(state, false);\n\tstate->encoder_failed_session = false;\n}\n''',
)
replace_once(
    source,
    '''\t\tstate->target_segment_ns = (uint64_t)sr_config_get_segment_duration_ms() * 1000000ULL;\n\t\tstate->next_sequence = find_next_sequence(state->audio_dir);\n''',
    '''\t\tstate->target_segment_ns = (uint64_t)sr_config_get_segment_duration_ms() * 1000000ULL;\n\t\tstate->min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY\n\t\t\t\t\t\t? 0\n\t\t\t\t\t\t: sr_config_get_min_free_bytes();\n\t\tstate->reserve_blocked = false;\n\t\tstate->reserve_recheck_after_ns = 0;\n\t\tstate->next_sequence = find_next_sequence(state->audio_dir);\n''',
)

# Tighten partial init cleanup: do not destroy an uninitialized condition
# variable, and do release a successfully initialized mutex if cond init fails.
replace_once(
    source,
    '''\tstruct sr_master_audio_state *state = bzalloc(sizeof(*state));\n\tstate->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;\n\tif (pthread_mutex_init(&state->mutex, NULL) != 0 || pthread_cond_init(&state->cond, NULL) != 0) {\n\t\tbfree(state);\n\t\treturn false;\n\t}\n''',
    '''\tstruct sr_master_audio_state *state = bzalloc(sizeof(*state));\n\tstate->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;\n\tif (pthread_mutex_init(&state->mutex, NULL) != 0) {\n\t\tbfree(state);\n\t\treturn false;\n\t}\n\tif (pthread_cond_init(&state->cond, NULL) != 0) {\n\t\tpthread_mutex_destroy(&state->mutex);\n\t\tbfree(state);\n\t\treturn false;\n\t}\n''',
)
