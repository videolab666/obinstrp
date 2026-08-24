from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# Shared capture-filter setting used by the replay reader/controller side.
capture_h = Path("src/sr-capture.h")
replace_once(
    capture_h,
    '#define S_CAPTURE_SOURCE "capture_source"\n',
    '#define S_CAPTURE_SOURCE "capture_source"\n\n/* Per-camera replay timing correction. Positive means the camera signal\n * arrives late relative to the global/master timeline, so replay selects a\n * later camera-media timestamp (global + offset). */\n#define S_SYNC_OFFSET_MS "sync_offset_ms"\n#define SR_CAMERA_SYNC_MAX_MS 5000\n',
)

# Expose the persisted sync setting in each Sports Replay Capture filter.
capture = Path("src/capture-filter.c")
replace_once(
    capture,
    '''\tp = obs_properties_add_bool(props, S_DISK_RECORDING, obs_module_text("DiskRecording"));\n\tobs_property_set_long_description(p, obs_module_text("DiskRecording.Description"));\n\n\tchar credit[256];\n''',
    '''\tp = obs_properties_add_bool(props, S_DISK_RECORDING, obs_module_text("DiskRecording"));\n\tobs_property_set_long_description(p, obs_module_text("DiskRecording.Description"));\n\n\tp = obs_properties_add_int(props, S_SYNC_OFFSET_MS, obs_module_text("SyncOffset"), -SR_CAMERA_SYNC_MAX_MS,\n\t\t\t\t   SR_CAMERA_SYNC_MAX_MS, 1);\n\tobs_property_int_set_suffix(p, " ms");\n\tobs_property_set_long_description(p, obs_module_text("SyncOffset.Description"));\n\n\tchar credit[256];\n''',
)
replace_once(
    capture,
    '''\tobs_data_set_default_int(settings, S_GOP, SR_GOP_500MS);\n\t/* Keep new continuous recording opt-in while the disk engine is still\n''',
    '''\tobs_data_set_default_int(settings, S_GOP, SR_GOP_500MS);\n\tobs_data_set_default_int(settings, S_SYNC_OFFSET_MS, 0);\n\t/* Keep new continuous recording opt-in while the disk engine is still\n''',
)

# Camera identity helper also resolves the capture filter's persisted sync offset.
identity_h = Path("src/sr-camera-identity.h")
replace_once(
    identity_h,
    '''bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size);\nbool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size);\n\nuint32_t sr_camera_key_hash(const char *key);\n''',
    '''bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size);\nbool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size);\n\n/* Reads the persisted Sports Replay Capture filter timing correction.\n * Positive means camera media timestamps are late and must be addressed as\n * global_timestamp + offset. Ambiguous duplicate capture filters fail closed. */\nbool sr_camera_sync_offset_ns(const char *camera_name, int64_t *offset_ns);\n\nuint32_t sr_camera_key_hash(const char *key);\n''',
)

identity = Path("src/sr-camera-identity.c")
replace_once(identity, '#include "sr-camera-identity.h"\n', '#include "sr-camera-identity.h"\n#include "sr-capture.h"\n')
replace_once(
    identity,
    '''bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size)\n{\n\tif (!camera_name || !*camera_name)\n\t\treturn false;\n\n\tobs_source_t *source = obs_get_source_by_name(camera_name);\n\tif (!source)\n\t\treturn false;\n\tconst bool ok = sr_camera_key_from_source(source, key, key_size);\n\tobs_source_release(source);\n\treturn ok;\n}\n\nuint32_t sr_camera_key_hash(const char *key)\n''',
    '''bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size)\n{\n\tif (!camera_name || !*camera_name)\n\t\treturn false;\n\n\tobs_source_t *source = obs_get_source_by_name(camera_name);\n\tif (!source)\n\t\treturn false;\n\tconst bool ok = sr_camera_key_from_source(source, key, key_size);\n\tobs_source_release(source);\n\treturn ok;\n}\n\nstruct sync_offset_query {\n\tbool found;\n\tbool ambiguous;\n\tint64_t offset_ms;\n};\n\nstatic void read_sync_offset(obs_source_t *parent, obs_source_t *child, void *param)\n{\n\tUNUSED_PARAMETER(parent);\n\tstruct sync_offset_query *query = param;\n\tif (!query || query->ambiguous || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)\n\t\treturn;\n\n\tif (query->found) {\n\t\tquery->ambiguous = true;\n\t\treturn;\n\t}\n\n\tobs_data_t *settings = obs_source_get_settings(child);\n\tif (!settings)\n\t\treturn;\n\tquery->offset_ms = obs_data_get_int(settings, S_SYNC_OFFSET_MS);\n\tquery->found = true;\n\tobs_data_release(settings);\n}\n\nbool sr_camera_sync_offset_ns(const char *camera_name, int64_t *offset_ns)\n{\n\tif (!camera_name || !*camera_name || !offset_ns)\n\t\treturn false;\n\t*offset_ns = 0;\n\n\tobs_source_t *source = obs_get_source_by_name(camera_name);\n\tif (!source)\n\t\treturn false;\n\n\tstruct sync_offset_query query = {0};\n\tobs_source_enum_filters(source, read_sync_offset, &query);\n\tobs_source_release(source);\n\tif (!query.found || query.ambiguous || query.offset_ms < -SR_CAMERA_SYNC_MAX_MS ||\n\t    query.offset_ms > SR_CAMERA_SYNC_MAX_MS)\n\t\treturn false;\n\n\t*offset_ns = query.offset_ms * 1000000LL;\n\treturn true;\n}\n\nuint32_t sr_camera_key_hash(const char *key)\n''',
)

# Gap-aware coverage now operates in camera-media time and translates the
# selected contiguous interval back to the global/master timeline.
Path("src/sr-replay-coverage.c").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-coverage.h"

#include "sr-camera-identity.h"
#include "sr-segment-catalog.h"
#include "sr-segment-format.h"
#include "sr-session.h"

#include <util/bmem.h>

#include <string.h>

#define NS_PER_SECOND 1000000000ULL
#define FALLBACK_HANDOFF_NS 50000000ULL
#define MAX_HANDOFF_NS 250000000ULL

struct coverage_interval {
    uint64_t start_ns;
    uint64_t end_ns;
    bool active;
};

static bool global_to_media(uint64_t global_ns, int64_t offset_ns, uint64_t *media_ns)
{
    if (!media_ns)
        return false;
    if (offset_ns >= 0) {
        const uint64_t magnitude = (uint64_t)offset_ns;
        if (global_ns > UINT64_MAX - magnitude)
            return false;
        *media_ns = global_ns + magnitude;
        return true;
    }

    const uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;
    if (global_ns < magnitude)
        return false;
    *media_ns = global_ns - magnitude;
    return true;
}

static bool media_to_global(uint64_t media_ns, int64_t offset_ns, uint64_t *global_ns)
{
    if (!global_ns)
        return false;
    if (offset_ns >= 0) {
        const uint64_t magnitude = (uint64_t)offset_ns;
        if (media_ns < magnitude)
            return false;
        *global_ns = media_ns - magnitude;
        return true;
    }

    const uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;
    if (media_ns > UINT64_MAX - magnitude)
        return false;
    *global_ns = media_ns + magnitude;
    return true;
}

static uint64_t frame_interval_ns(const struct sr_segment_descriptor *segment)
{
    if (!segment || !segment->fps_num || !segment->fps_den)
        return FALLBACK_HANDOFF_NS / 2;

    const uint64_t numerator = NS_PER_SECOND * (uint64_t)segment->fps_den;
    return (numerator + segment->fps_num - 1) / segment->fps_num;
}

static uint64_t handoff_tolerance_ns(const struct sr_segment_descriptor *previous,
                                     const struct sr_segment_descriptor *next)
{
    uint64_t frame_ns = frame_interval_ns(previous);
    const uint64_t next_frame_ns = frame_interval_ns(next);
    if (next_frame_ns > frame_ns)
        frame_ns = next_frame_ns;
    if (frame_ns > MAX_HANDOFF_NS / 2)
        return MAX_HANDOFF_NS;
    const uint64_t tolerance = frame_ns * 2;
    return tolerance ? tolerance : FALLBACK_HANDOFF_NS;
}

static bool normal_handoff(const struct sr_segment_descriptor *previous, const struct sr_segment_descriptor *next,
                           uint64_t current_end_ns)
{
    if (!previous || !next || (next->flags & SR_SEGMENT_FLAG_DISCONTINUITY))
        return false;
    if (next->start_ns <= current_end_ns)
        return true;
    return next->start_ns - current_end_ns <= handoff_tolerance_ns(previous, next);
}

static void consider_interval(const struct coverage_interval *interval, uint64_t event_in_ns, uint64_t event_out_ns,
                              bool require_timestamp, uint64_t timestamp_ns, struct sr_replay_coverage_info *info,
                              bool *have_best, bool *best_contains_in, uint64_t *best_duration)
{
    if (!interval || interval->end_ns < interval->start_ns)
        return;
    if (require_timestamp && (timestamp_ns < interval->start_ns || timestamp_ns > interval->end_ns))
        return;

    if (interval->start_ns == event_in_ns && interval->end_ns == event_out_ns) {
        info->coverage = SR_REPLAY_COVERAGE_FULL;
        info->playable_in_ns = event_in_ns;
        info->playable_out_ns = event_out_ns;
        info->active = interval->active;
        *have_best = true;
        *best_contains_in = true;
        *best_duration = event_out_ns - event_in_ns;
        return;
    }

    if (info->coverage == SR_REPLAY_COVERAGE_FULL)
        return;

    const bool contains_in = interval->start_ns <= event_in_ns && interval->end_ns >= event_in_ns;
    const uint64_t duration = interval->end_ns - interval->start_ns;
    const bool better = !*have_best || (contains_in && !*best_contains_in) ||
                        (contains_in == *best_contains_in && duration > *best_duration);
    if (!better)
        return;

    info->coverage = SR_REPLAY_COVERAGE_PARTIAL;
    info->playable_in_ns = interval->start_ns;
    info->playable_out_ns = interval->end_ns;
    info->active = interval->active;
    *have_best = true;
    *best_contains_in = contains_in;
    *best_duration = duration;
}

static bool coverage_query_internal(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
                                    bool require_timestamp, uint64_t timestamp_ns,
                                    struct sr_replay_coverage_info *info)
{
    if (!info)
        return false;

    memset(info, 0, sizeof(*info));
    info->coverage = SR_REPLAY_COVERAGE_NONE;

    if (!camera_name || !*camera_name || event_out_ns < event_in_ns)
        return false;
    if (require_timestamp && (timestamp_ns < event_in_ns || timestamp_ns > event_out_ns))
        return false;

    int64_t sync_offset_ns = 0;
    if (!sr_camera_sync_offset_ns(camera_name, &sync_offset_ns))
        return false;
    info->sync_offset_ns = sync_offset_ns;

    uint64_t media_event_in_ns = 0;
    uint64_t media_event_out_ns = 0;
    uint64_t media_timestamp_ns = 0;
    if (!global_to_media(event_in_ns, sync_offset_ns, &media_event_in_ns) ||
        !global_to_media(event_out_ns, sync_offset_ns, &media_event_out_ns) ||
        (require_timestamp && !global_to_media(timestamp_ns, sync_offset_ns, &media_timestamp_ns)))
        return false;

    char *session_dir = sr_session_get_or_create_path();
    if (!session_dir)
        return false;

    struct sr_segment_descriptor *segments = NULL;
    size_t count = 0;
    const bool scanned = sr_segment_catalog_scan(session_dir, camera_name, &segments, &count);
    bfree(session_dir);
    if (!scanned)
        return false;

    struct coverage_interval current = {0};
    const struct sr_segment_descriptor *previous = NULL;
    bool have_current = false;
    bool have_best = false;
    bool best_contains_in = false;
    uint64_t best_duration = 0;

    for (size_t i = 0; i < count; i++) {
        const struct sr_segment_descriptor *segment = &segments[i];
        if (segment->end_ns < media_event_in_ns || segment->start_ns > media_event_out_ns)
            continue;

        const uint64_t start_ns = segment->start_ns < media_event_in_ns ? media_event_in_ns : segment->start_ns;
        const uint64_t end_ns = segment->end_ns > media_event_out_ns ? media_event_out_ns : segment->end_ns;
        if (!have_current) {
            current.start_ns = start_ns;
            current.end_ns = end_ns;
            current.active = segment->active;
            have_current = true;
            previous = segment;
            continue;
        }

        if (normal_handoff(previous, segment, current.end_ns)) {
            if (end_ns > current.end_ns)
                current.end_ns = end_ns;
            current.active = current.active || segment->active;
            previous = segment;
            continue;
        }

        consider_interval(&current, media_event_in_ns, media_event_out_ns, require_timestamp, media_timestamp_ns, info,
                          &have_best, &best_contains_in, &best_duration);
        current.start_ns = start_ns;
        current.end_ns = end_ns;
        current.active = segment->active;
        previous = segment;
    }

    if (have_current)
        consider_interval(&current, media_event_in_ns, media_event_out_ns, require_timestamp, media_timestamp_ns, info,
                          &have_best, &best_contains_in, &best_duration);

    sr_segment_catalog_free(segments, count);

    if (info->coverage != SR_REPLAY_COVERAGE_NONE) {
        uint64_t global_in_ns = 0;
        uint64_t global_out_ns = 0;
        if (!media_to_global(info->playable_in_ns, sync_offset_ns, &global_in_ns) ||
            !media_to_global(info->playable_out_ns, sync_offset_ns, &global_out_ns)) {
            memset(info, 0, sizeof(*info));
            return false;
        }
        info->playable_in_ns = global_in_ns;
        info->playable_out_ns = global_out_ns;
        info->sync_offset_ns = sync_offset_ns;
    }
    return true;
}

bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
                              struct sr_replay_coverage_info *info)
{
    return coverage_query_internal(camera_name, event_in_ns, event_out_ns, false, 0, info);
}

bool sr_replay_coverage_query_at(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
                                 uint64_t timestamp_ns, struct sr_replay_coverage_info *info)
{
    return coverage_query_internal(camera_name, event_in_ns, event_out_ns, true, timestamp_ns, info);
}
''', encoding="utf-8")

coverage_h = Path("src/sr-replay-coverage.h")
replace_once(
    coverage_h,
    '''\tuint64_t playable_in_ns;\n\tuint64_t playable_out_ns;\n\tbool active;\n''',
    '''\tuint64_t playable_in_ns;\n\tuint64_t playable_out_ns;\n\tint64_t sync_offset_ns;\n\tbool active;\n''',
)
replace_once(
    coverage_h,
    ''' * whole Event. For PARTIAL coverage, the interval containing Event IN is\n * preferred; otherwise the longest available interval is returned. */\n''',
    ''' * whole Event. Camera media timestamps are translated through the capture\n * filter's persisted sync offset while returned playable bounds remain on the\n * global/master timeline. For PARTIAL coverage, the interval containing Event\n * IN is preferred; otherwise the longest available interval is returned. */\n''',
)

# Keep bus playheads on the global/master timeline while shifting only camera
# media access by the selected camera's offset.
channel_h = Path("src/sr-replay-channel.h")
replace_once(
    channel_h,
    '''\tuint64_t playhead_ns;\n\tdouble speed_percent;\n''',
    '''\tuint64_t playhead_ns;\n\tint64_t sync_offset_ns;\n\tdouble speed_percent;\n''',
)

channel = Path("src/sr-replay-channel.c")
replace_once(
    channel,
    '''\tuint64_t playhead_ns;\n\tuint64_t last_clock_ns;\n\n\tdouble speed_percent;\n''',
    '''\tuint64_t playhead_ns;\n\tuint64_t last_clock_ns;\n\tint64_t sync_offset_ns;\n\n\tdouble speed_percent;\n''',
)
replace_once(
    channel,
    '''static struct sr_replay_channel *get_bus(enum sr_replay_bus bus)\n{\n\tif (!g_channels || !valid_bus(bus))\n\t\treturn NULL;\n\treturn &g_channels->buses[bus];\n}\n\nstatic void clear_locked(struct sr_replay_channel *channel)\n''',
    '''static struct sr_replay_channel *get_bus(enum sr_replay_bus bus)\n{\n\tif (!g_channels || !valid_bus(bus))\n\t\treturn NULL;\n\treturn &g_channels->buses[bus];\n}\n\nstatic bool global_to_camera_media(uint64_t global_ns, int64_t offset_ns, uint64_t *media_ns)\n{\n\tif (!media_ns)\n\t\treturn false;\n\tif (offset_ns >= 0) {\n\t\tconst uint64_t magnitude = (uint64_t)offset_ns;\n\t\tif (global_ns > UINT64_MAX - magnitude)\n\t\t\treturn false;\n\t\t*media_ns = global_ns + magnitude;\n\t\treturn true;\n\t}\n\n\tconst uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;\n\tif (global_ns < magnitude)\n\t\treturn false;\n\t*media_ns = global_ns - magnitude;\n\treturn true;\n}\n\nstatic bool camera_media_to_global(uint64_t media_ns, int64_t offset_ns, uint64_t *global_ns)\n{\n\tif (!global_ns)\n\t\treturn false;\n\tif (offset_ns >= 0) {\n\t\tconst uint64_t magnitude = (uint64_t)offset_ns;\n\t\tif (media_ns < magnitude)\n\t\t\treturn false;\n\t\t*global_ns = media_ns - magnitude;\n\t\treturn true;\n\t}\n\n\tconst uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;\n\tif (media_ns > UINT64_MAX - magnitude)\n\t\treturn false;\n\t*global_ns = media_ns + magnitude;\n\treturn true;\n}\n\nstatic void clear_locked(struct sr_replay_channel *channel)\n''',
)
replace_once(channel, "\tchannel->last_clock_ns = 0;\n\tchannel->speed_percent = 100.0;\n", "\tchannel->last_clock_ns = 0;\n\tchannel->sync_offset_ns = 0;\n\tchannel->speed_percent = 100.0;\n")
replace_once(
    channel,
    '''\tAVFrame *first_frame = NULL;\n\tif (!sr_disk_player_decode_at(player, in_ns, &first_frame, NULL) || !first_frame) {\n''',
    '''\tuint64_t first_media_ns = 0;\n\tAVFrame *first_frame = NULL;\n\tif (!global_to_camera_media(in_ns, coverage.sync_offset_ns, &first_media_ns) ||\n\t    !sr_disk_player_decode_at(player, first_media_ns, &first_frame, NULL) || !first_frame) {\n''',
)
replace_once(
    channel,
    '''\tchannel->playhead_ns = in_ns;\n\tchannel->speed_percent = speed;\n''',
    '''\tchannel->playhead_ns = in_ns;\n\tchannel->sync_offset_ns = coverage.sync_offset_ns;\n\tchannel->speed_percent = speed;\n''',
)
replace_once(
    channel,
    '''\tconst uint64_t new_in_ns = coverage.playable_in_ns;\n\tconst uint64_t new_out_ns = coverage.playable_out_ns;\n\tconst bool new_partial = coverage.coverage != SR_REPLAY_COVERAGE_FULL;\n''',
    '''\tconst uint64_t new_in_ns = coverage.playable_in_ns;\n\tconst uint64_t new_out_ns = coverage.playable_out_ns;\n\tconst int64_t new_sync_offset_ns = coverage.sync_offset_ns;\n\tconst bool new_partial = coverage.coverage != SR_REPLAY_COVERAGE_FULL;\n''',
)
replace_once(
    channel,
    '''\tAVFrame *probe_frame = NULL;\n\tif (!sr_disk_player_decode_at(new_player, expected_playhead_ns, &probe_frame, NULL) || !probe_frame) {\n''',
    '''\tuint64_t probe_media_ns = 0;\n\tAVFrame *probe_frame = NULL;\n\tif (!global_to_camera_media(expected_playhead_ns, new_sync_offset_ns, &probe_media_ns) ||\n\t    !sr_disk_player_decode_at(new_player, probe_media_ns, &probe_frame, NULL) || !probe_frame) {\n''',
)
replace_once(
    channel,
    '''\t\tAVFrame *commit_probe = NULL;\n\t\tconst bool commit_ready =\n\t\t\tsr_disk_player_decode_at(new_player, channel->playhead_ns, &commit_probe, NULL) && commit_probe;\n''',
    '''\t\tuint64_t commit_media_ns = 0;\n\t\tAVFrame *commit_probe = NULL;\n\t\tconst bool commit_ready =\n\t\t\tglobal_to_camera_media(channel->playhead_ns, new_sync_offset_ns, &commit_media_ns) &&\n\t\t\tsr_disk_player_decode_at(new_player, commit_media_ns, &commit_probe, NULL) && commit_probe;\n''',
)
replace_once(
    channel,
    '''\t\t\tchannel->out_ns = new_out_ns;\n\t\t\tchannel->width = 0;\n''',
    '''\t\t\tchannel->out_ns = new_out_ns;\n\t\t\tchannel->sync_offset_ns = new_sync_offset_ns;\n\t\t\tchannel->width = 0;\n''',
)
replace_once(
    channel,
    '''\twhile (remaining--) {\n\t\tuint64_t adjacent = 0;\n\t\tif (!sr_disk_player_neighbor_timestamp(channel->player, channel->playhead_ns, direction, &adjacent))\n\t\t\tbreak;\n\t\tif (adjacent < channel->in_ns || adjacent > channel->out_ns)\n\t\t\tbreak;\n\t\tchannel->playhead_ns = adjacent;\n\t\tmoved = true;\n\t}\n''',
    '''\twhile (remaining--) {\n\t\tuint64_t media_playhead_ns = 0;\n\t\tuint64_t adjacent_media_ns = 0;\n\t\tuint64_t adjacent_global_ns = 0;\n\t\tif (!global_to_camera_media(channel->playhead_ns, channel->sync_offset_ns, &media_playhead_ns) ||\n\t\t    !sr_disk_player_neighbor_timestamp(channel->player, media_playhead_ns, direction, &adjacent_media_ns) ||\n\t\t    !camera_media_to_global(adjacent_media_ns, channel->sync_offset_ns, &adjacent_global_ns))\n\t\t\tbreak;\n\t\tif (adjacent_global_ns < channel->in_ns || adjacent_global_ns > channel->out_ns)\n\t\t\tbreak;\n\t\tchannel->playhead_ns = adjacent_global_ns;\n\t\tmoved = true;\n\t}\n''',
)
replace_once(
    channel,
    '''\tstate->playhead_ns = channel->playhead_ns;\n\tstate->speed_percent = channel->speed_percent;\n''',
    '''\tstate->playhead_ns = channel->playhead_ns;\n\tstate->sync_offset_ns = channel->sync_offset_ns;\n\tstate->speed_percent = channel->speed_percent;\n''',
)
replace_once(
    channel,
    '''\tAVFrame *decoded = NULL;\n\tuint64_t actual_ns = 0;\n\tbool ok = sr_disk_player_decode_at(channel->player, channel->playhead_ns, &decoded, &actual_ns);\n\tif (!ok) {\n\t\tsr_disk_player_refresh(channel->player);\n\t\tok = sr_disk_player_decode_at(channel->player, channel->playhead_ns, &decoded, &actual_ns);\n\t}\n''',
    '''\tuint64_t target_media_ns = 0;\n\tif (!global_to_camera_media(channel->playhead_ns, channel->sync_offset_ns, &target_media_ns)) {\n\t\tpthread_mutex_unlock(&channel->mutex);\n\t\treturn false;\n\t}\n\n\tAVFrame *decoded = NULL;\n\tuint64_t actual_media_ns = 0;\n\tbool ok = sr_disk_player_decode_at(channel->player, target_media_ns, &decoded, &actual_media_ns);\n\tif (!ok) {\n\t\tsr_disk_player_refresh(channel->player);\n\t\tok = sr_disk_player_decode_at(channel->player, target_media_ns, &decoded, &actual_media_ns);\n\t}\n''',
)
replace_once(
    channel,
    '''\t\t*frame = decoded;\n\t\tif (media_timestamp_ns)\n\t\t\t*media_timestamp_ns = actual_ns;\n''',
    '''\t\t*frame = decoded;\n\t\tif (media_timestamp_ns) {\n\t\t\tuint64_t actual_global_ns = channel->playhead_ns;\n\t\t\tif (camera_media_to_global(actual_media_ns, channel->sync_offset_ns, &actual_global_ns))\n\t\t\t\t*media_timestamp_ns = actual_global_ns;\n\t\t\telse\n\t\t\t\t*media_timestamp_ns = channel->playhead_ns;\n\t\t}\n''',
)

# EventDB v2 removes the semantically misleading "coverage" overlap view while
# retaining the raw overlap query under an explicit name.
db_h = Path("src/sr-event-db.h")
replace_once(db_h, "#define SR_EVENT_DB_SCHEMA_VERSION 1\n", "#define SR_EVENT_DB_SCHEMA_VERSION 2\n")

db = Path("src/sr-event-db.c")
replace_once(
    db,
    '''\treturn true;\n}\n\nstatic bool migrate_schema(struct sr_event_db *db)\n{\n''',
    '''\treturn true;\n}\n\nstatic bool migrate_v2(struct sr_event_db *db)\n{\n\tstatic const char *migration_sql =\n\t\t"DROP VIEW IF EXISTS event_camera_coverage;"\n\t\t"CREATE VIEW IF NOT EXISTS event_segment_overlap AS "\n\t\t"SELECT e.id AS event_id, s.camera_id AS camera_id "\n\t\t"FROM events AS e JOIN segments AS s ON s.start_ns <= e.out_ns AND s.end_ns >= e.in_ns "\n\t\t"GROUP BY e.id, s.camera_id;";\n\n\tif (!begin_transaction(db))\n\t\treturn false;\n\tif (!exec_sql(db, migration_sql, "migrate schema v2") ||\n\t    !exec_sql(db, "PRAGMA user_version=2", "set schema v2")) {\n\t\trollback_transaction(db);\n\t\treturn false;\n\t}\n\tif (!commit_transaction(db)) {\n\t\trollback_transaction(db);\n\t\treturn false;\n\t}\n\treturn true;\n}\n\nstatic bool migrate_schema(struct sr_event_db *db)\n{\n''',
)
replace_once(
    db,
    '''\tif (version == 0) {\n\t\tif (!migrate_v1(db))\n\t\t\treturn false;\n\t\tversion = 1;\n\t}\n\n\tdb->schema_version = version;\n''',
    '''\tif (version == 0) {\n\t\tif (!migrate_v1(db))\n\t\t\treturn false;\n\t\tversion = 1;\n\t}\n\n\tif (version == 1) {\n\t\tif (!migrate_v2(db))\n\t\t\treturn false;\n\t\tversion = 2;\n\t}\n\n\tdb->schema_version = version;\n''',
)

# Locale strings.
en = Path("data/locale/en-US.ini")
replace_once(
    en,
    'DiskRecording.Description="Write the encoded camera stream continuously into crash-recoverable replay segments while keeping the existing RAM instant replay buffer active. This is an early disk-engine option and is disabled by default."\n',
    'DiskRecording.Description="Write the encoded camera stream continuously into crash-recoverable replay segments while keeping the existing RAM instant replay buffer active. This is an early disk-engine option and is disabled by default."\nSyncOffset="Camera sync offset"\nSyncOffset.Description="Positive values compensate a camera that arrives late relative to the global/master replay timeline by selecting later camera frames. Example: a camera delayed by 200 ms uses +200 ms."\n',
)

es = Path("data/locale/es-ES.ini")
replace_once(
    es,
    'GOP.Economy="1,00 s — Economy"\n',
    'GOP.Economy="1,00 s — Economy"\nSyncOffset="Compensación de sincronización de cámara"\nSyncOffset.Description="Los valores positivos compensan una cámara que llega tarde respecto a la línea de tiempo global/master del replay seleccionando cuadros posteriores. Ejemplo: una cámara retrasada 200 ms usa +200 ms."\n',
)

print("camera sync offsets and EventDB schema v2 applied")
