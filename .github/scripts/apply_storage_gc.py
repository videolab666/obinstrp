from pathlib import Path


def must_replace(path: Path, old: str, new: str, guard: str | None = None) -> None:
    text = path.read_text(encoding="utf-8")
    if guard and guard in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# ---------------------------------------------------------------------------
# Process-wide media-reference guard.
# Event range creation/update and physical segment unlink use the same guard,
# closing the tiny check-then-unlink race without holding the UI/controller
# mutex while scanning thousands of segment files.
# ---------------------------------------------------------------------------
Path("src/sr-media-guard.h").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Serializes mutations that can create a new media reference (Event create /
 * range update) against a storage worker's final overlap-check + unlink. The
 * guard is intentionally process-wide because EventDB and the per-camera disk
 * writers may use independent SQLite connections. */
void sr_media_guard_lock(void);
void sr_media_guard_unlock(void);

#ifdef __cplusplus
}
#endif
''', encoding="utf-8")

Path("src/sr-media-guard.c").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-media-guard.h"

#include <util/threading.h>

static pthread_mutex_t g_media_guard = PTHREAD_MUTEX_INITIALIZER;

void sr_media_guard_lock(void)
{
    pthread_mutex_lock(&g_media_guard);
}

void sr_media_guard_unlock(void)
{
    pthread_mutex_unlock(&g_media_guard);
}
''', encoding="utf-8")

# ---------------------------------------------------------------------------
# Storage cleanup + automatic conservative GC.
# ---------------------------------------------------------------------------
Path("src/sr-storage-cleanup.h").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_db;

struct sr_storage_cleanup_result {
    size_t camera_dirs_scanned;
    size_t segments_examined;
    size_t segments_deleted;
    size_t segments_pinned;
    size_t errors;
    uint64_t free_bytes_before;
    uint64_t free_bytes_after;
    bool target_reached;
};

/* Permanently removes finalized segment/index pairs that are wholly contained
 * inside [range_in_ns, range_out_ns] and are not referenced by any Event that
 * still exists in the supplied Event database. Active .part files, boundary
 * segments and any uncertain/corrupt entries are kept. */
bool sr_storage_delete_unreferenced_range(struct sr_event_db *events, uint64_t range_in_ns, uint64_t range_out_ns,
                                          struct sr_storage_cleanup_result *result);

/* Oldest-first rolling GC for the current replay session. Only finalized
 * segments with zero saved Event overlap are eligible. Saved Events therefore
 * pin their media regardless of their Protected flag; automatic GC can never
 * silently invalidate an Event. Multiple camera writers are serialized inside
 * this function. */
bool sr_storage_gc_reclaim_unreferenced(const char *session_dir, const char *volume_path, uint64_t target_free_bytes,
                                        struct sr_storage_cleanup_result *result);

#ifdef __cplusplus
}
#endif
''', encoding="utf-8")

Path("src/sr-storage-cleanup.c").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-storage-cleanup.h"

#include "sr-event-db.h"
#include "sr-media-guard.h"
#include "sr-segment-reader.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

struct sr_gc_candidate {
    char *segment_path;
    char *index_path;
    uint64_t start_ns;
    uint64_t end_ns;
};

static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *join_path(const char *dir, const char *tail)
{
    struct dstr path = {0};
    dstr_copy(&path, dir ? dir : "");
    dstr_replace(&path, "\\", "/");
    if (path.len && dstr_end(&path) != '/')
        dstr_cat_ch(&path, '/');
    dstr_cat(&path, tail ? tail : "");
    char *result = bstrdup(path.array);
    dstr_free(&path);
    return result;
}

static char *index_path_for_segment(const char *segment_path)
{
    if (!segment_path)
        return NULL;
    const size_t len = strlen(segment_path);
    static const char suffix[] = ".srseg";
    if (len < sizeof(suffix) - 1 || strcmp(segment_path + len - (sizeof(suffix) - 1), suffix) != 0)
        return NULL;

    struct dstr path = {0};
    dstr_ncopy(&path, segment_path, len - (sizeof(suffix) - 1));
    dstr_cat(&path, ".sridx");
    char *result = bstrdup(path.array);
    dstr_free(&path);
    return result;
}

static bool segment_range(const char *segment_path, const char *index_path, uint64_t *start_ns, uint64_t *end_ns)
{
    struct sr_segment_reader *reader = sr_segment_reader_open(segment_path, index_path);
    if (!reader)
        return false;

    struct sr_segment_stream_info info;
    bool ok = sr_segment_reader_get_info(reader, &info);
    uint64_t end = info.segment_start_ns;
    if (ok && info.indexed_packets) {
        struct sr_index_entry last;
        ok = sr_segment_reader_find(reader, UINT64_MAX, false, &last);
        if (ok)
            end = last.timestamp_ns;
    }

    if (ok) {
        if (start_ns)
            *start_ns = info.segment_start_ns;
        if (end_ns)
            *end_ns = end;
    }
    sr_segment_reader_close(reader);
    return ok;
}

static bool delete_pair_if_unreferenced(struct sr_event_db *events, const char *segment_path, const char *index_path,
                                        uint64_t start_ns, uint64_t end_ns,
                                        struct sr_storage_cleanup_result *result)
{
    bool pinned = true;
    sr_media_guard_lock();
    const bool query_ok = sr_event_db_has_event_overlap(events, start_ns, end_ns, &pinned);
    if (!query_ok) {
        sr_media_guard_unlock();
        result->errors++;
        return false;
    }
    if (pinned) {
        sr_media_guard_unlock();
        result->segments_pinned++;
        return true;
    }

    const int segment_rc = os_unlink(segment_path);
    int index_rc = 0;
    if (segment_rc == 0)
        index_rc = os_unlink(index_path);
    sr_media_guard_unlock();

    if (segment_rc == 0 && index_rc == 0) {
        result->segments_deleted++;
        blog(LOG_INFO, "Sports Replay: permanently deleted unreferenced replay segment '%s'", segment_path);
        return true;
    }

    result->errors++;
    blog(LOG_WARNING, "Sports Replay: could not completely delete replay segment pair '%s' / '%s'", segment_path,
         index_path);
    return false;
}

static void cleanup_camera_dir(struct sr_event_db *events, const char *camera_dir, uint64_t range_in_ns,
                               uint64_t range_out_ns, struct sr_storage_cleanup_result *result)
{
    char *pattern = join_path(camera_dir, "*.srseg");
    if (!pattern) {
        result->errors++;
        return;
    }

    os_glob_t *glob = NULL;
    if (os_glob(pattern, 0, &glob) != 0) {
        bfree(pattern);
        return;
    }
    bfree(pattern);

    for (size_t i = 0; i < glob->gl_pathc; i++) {
        if (glob->gl_pathv[i].directory)
            continue;

        const char *segment_path = glob->gl_pathv[i].path;
        char *index_path = index_path_for_segment(segment_path);
        if (!index_path || !os_file_exists(index_path)) {
            bfree(index_path);
            result->errors++;
            continue;
        }

        result->segments_examined++;
        uint64_t start_ns = 0;
        uint64_t end_ns = 0;
        if (!segment_range(segment_path, index_path, &start_ns, &end_ns)) {
            bfree(index_path);
            result->errors++;
            continue;
        }

        /* Never remove a boundary segment: it contains recording outside the
         * range the operator explicitly asked to delete. */
        if (start_ns < range_in_ns || end_ns > range_out_ns) {
            bfree(index_path);
            continue;
        }

        delete_pair_if_unreferenced(events, segment_path, index_path, start_ns, end_ns, result);
        bfree(index_path);
    }

    os_globfree(glob);
}

bool sr_storage_delete_unreferenced_range(struct sr_event_db *events, uint64_t range_in_ns, uint64_t range_out_ns,
                                          struct sr_storage_cleanup_result *result)
{
    if (!events || range_out_ns < range_in_ns)
        return false;

    struct sr_storage_cleanup_result local = {0};
    char *session_dir = sr_session_get_or_create_path();
    if (!session_dir)
        return false;

    char *pattern = join_path(session_dir, "cam-*");
    if (!pattern) {
        bfree(session_dir);
        return false;
    }

    os_glob_t *glob = NULL;
    if (os_glob(pattern, 0, &glob) == 0) {
        for (size_t i = 0; i < glob->gl_pathc; i++) {
            if (!glob->gl_pathv[i].directory)
                continue;
            local.camera_dirs_scanned++;
            cleanup_camera_dir(events, glob->gl_pathv[i].path, range_in_ns, range_out_ns, &local);
        }
        os_globfree(glob);
    }

    bfree(pattern);
    bfree(session_dir);
    if (result)
        *result = local;
    return true;
}

static int gc_candidate_compare(const void *a, const void *b)
{
    const struct sr_gc_candidate *ca = a;
    const struct sr_gc_candidate *cb = b;
    if (ca->end_ns < cb->end_ns)
        return -1;
    if (ca->end_ns > cb->end_ns)
        return 1;
    if (ca->start_ns < cb->start_ns)
        return -1;
    if (ca->start_ns > cb->start_ns)
        return 1;
    return strcmp(ca->segment_path, cb->segment_path);
}

static bool append_gc_candidate(struct sr_gc_candidate **items, size_t *count, size_t *capacity,
                                const char *segment_path, const char *index_path,
                                struct sr_storage_cleanup_result *result)
{
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    if (!segment_range(segment_path, index_path, &start_ns, &end_ns)) {
        result->errors++;
        return false;
    }

    if (*count == *capacity) {
        const size_t next_capacity = *capacity ? *capacity * 2 : 256;
        struct sr_gc_candidate *next = brealloc(*items, next_capacity * sizeof(**items));
        if (!next) {
            result->errors++;
            return false;
        }
        *items = next;
        *capacity = next_capacity;
    }

    struct sr_gc_candidate *dst = &(*items)[(*count)++];
    dst->segment_path = bstrdup(segment_path);
    dst->index_path = bstrdup(index_path);
    dst->start_ns = start_ns;
    dst->end_ns = end_ns;
    return dst->segment_path && dst->index_path;
}

static void free_gc_candidates(struct sr_gc_candidate *items, size_t count)
{
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        bfree(items[i].segment_path);
        bfree(items[i].index_path);
    }
    bfree(items);
}

static bool collect_gc_candidates(const char *session_dir, struct sr_gc_candidate **items, size_t *count,
                                  struct sr_storage_cleanup_result *result)
{
    *items = NULL;
    *count = 0;
    size_t capacity = 0;

    char *camera_pattern = join_path(session_dir, "cam-*");
    if (!camera_pattern)
        return false;

    os_glob_t *cameras = NULL;
    if (os_glob(camera_pattern, 0, &cameras) != 0) {
        bfree(camera_pattern);
        return true;
    }
    bfree(camera_pattern);

    for (size_t c = 0; c < cameras->gl_pathc; c++) {
        if (!cameras->gl_pathv[c].directory)
            continue;
        result->camera_dirs_scanned++;
        char *segment_pattern = join_path(cameras->gl_pathv[c].path, "*.srseg");
        if (!segment_pattern) {
            result->errors++;
            continue;
        }
        os_glob_t *segments = NULL;
        if (os_glob(segment_pattern, 0, &segments) == 0) {
            for (size_t i = 0; i < segments->gl_pathc; i++) {
                if (segments->gl_pathv[i].directory)
                    continue;
                char *index_path = index_path_for_segment(segments->gl_pathv[i].path);
                if (!index_path || !os_file_exists(index_path)) {
                    bfree(index_path);
                    result->errors++;
                    continue;
                }
                append_gc_candidate(items, count, &capacity, segments->gl_pathv[i].path, index_path, result);
                bfree(index_path);
            }
            os_globfree(segments);
        }
        bfree(segment_pattern);
    }
    os_globfree(cameras);

    if (*count > 1)
        qsort(*items, *count, sizeof(**items), gc_candidate_compare);
    return true;
}

bool sr_storage_gc_reclaim_unreferenced(const char *session_dir, const char *volume_path, uint64_t target_free_bytes,
                                        struct sr_storage_cleanup_result *result)
{
    if (!session_dir || !*session_dir || !volume_path || !*volume_path || !target_free_bytes)
        return false;

    struct sr_storage_cleanup_result local = {0};
    pthread_mutex_lock(&g_gc_mutex);

    local.free_bytes_before = os_get_free_disk_space(volume_path);
    local.free_bytes_after = local.free_bytes_before;
    if (local.free_bytes_before >= target_free_bytes) {
        local.target_reached = true;
        pthread_mutex_unlock(&g_gc_mutex);
        if (result)
            *result = local;
        return true;
    }

    struct sr_event_db *events = sr_event_db_open(session_dir);
    if (!events) {
        local.errors++;
        pthread_mutex_unlock(&g_gc_mutex);
        if (result)
            *result = local;
        return false;
    }

    struct sr_gc_candidate *items = NULL;
    size_t count = 0;
    const bool scan_ok = collect_gc_candidates(session_dir, &items, &count, &local);
    if (scan_ok) {
        for (size_t i = 0; i < count && local.free_bytes_after < target_free_bytes; i++) {
            local.segments_examined++;
            delete_pair_if_unreferenced(events, items[i].segment_path, items[i].index_path, items[i].start_ns,
                                        items[i].end_ns, &local);
            local.free_bytes_after = os_get_free_disk_space(volume_path);
        }
    } else {
        local.errors++;
    }

    local.target_reached = local.free_bytes_after >= target_free_bytes;
    free_gc_candidates(items, count);
    sr_event_db_close(events);
    pthread_mutex_unlock(&g_gc_mutex);

    if (result)
        *result = local;
    return scan_ok;
}
''', encoding="utf-8")

# ---------------------------------------------------------------------------
# Config v3: low-space policy + existing purge target become live settings.
# ---------------------------------------------------------------------------
config_h = Path("src/sr-config.h")
text = config_h.read_text(encoding="utf-8")
text = text.replace("#define SR_CONFIG_SCHEMA_VERSION 2", "#define SR_CONFIG_SCHEMA_VERSION 3")
if "enum sr_storage_low_space_action" not in text:
    text = text.replace("void sr_config_init(void);\n", r'''enum sr_storage_low_space_action {
    SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED = 0,
    SR_STORAGE_LOW_SPACE_STOP_RECORDING = 1,
    SR_STORAGE_LOW_SPACE_WARN_ONLY = 2,
};

void sr_config_init(void);
''', 1)
if "sr_config_get_low_space_action" not in text:
    text = text.replace("uint32_t sr_config_get_segment_duration_ms(void);\n", r'''enum sr_storage_low_space_action sr_config_get_low_space_action(void);
void sr_config_set_low_space_action(enum sr_storage_low_space_action action);

uint32_t sr_config_get_segment_duration_ms(void);
''', 1)
config_h.write_text(text, encoding="utf-8")

config_c = Path("src/sr-config.c")
text = config_c.read_text(encoding="utf-8")
if "g_low_space_action" not in text:
    text = text.replace("static uint64_t g_purge_target_bytes;\n", "static uint64_t g_purge_target_bytes;\nstatic enum sr_storage_low_space_action g_low_space_action;\n", 1)
    text = text.replace('obs_data_set_int(data, "purge_target_bytes", (long long)g_purge_target_bytes);\n',
                        'obs_data_set_int(data, "purge_target_bytes", (long long)g_purge_target_bytes);\n\tobs_data_set_int(data, "low_space_action", (long long)g_low_space_action);\n', 1)
    text = text.replace('const int64_t segment_ms = data ? obs_data_get_int(data, "segment_duration_ms") : 0;\n',
                        'const int64_t segment_ms = data ? obs_data_get_int(data, "segment_duration_ms") : 0;\n\tconst int64_t low_space_action = data ? obs_data_get_int(data, "low_space_action") : 0;\n', 1)
    text = text.replace("\tif (g_purge_target_bytes < g_min_free_bytes)\n\t\tg_purge_target_bytes = g_min_free_bytes;\n\n\tg_segment_duration_ms",
                        "\tif (g_purge_target_bytes < g_min_free_bytes)\n\t\tg_purge_target_bytes = g_min_free_bytes;\n\n\tg_low_space_action = low_space_action >= SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED &&\n\t\t\t\t     low_space_action <= SR_STORAGE_LOW_SPACE_WARN_ONLY\n\t\t\t\t     ? (enum sr_storage_low_space_action)low_space_action\n\t\t\t\t     : SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;\n\n\tg_segment_duration_ms", 1)
    marker = "uint32_t sr_config_get_segment_duration_ms(void)\n"
    api = r'''enum sr_storage_low_space_action sr_config_get_low_space_action(void)
{
    pthread_mutex_lock(&g_mutex);
    const enum sr_storage_low_space_action value = g_low_space_action;
    pthread_mutex_unlock(&g_mutex);
    return value;
}

void sr_config_set_low_space_action(enum sr_storage_low_space_action action)
{
    if (action < SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED || action > SR_STORAGE_LOW_SPACE_WARN_ONLY)
        action = SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;

    pthread_mutex_lock(&g_mutex);
    g_low_space_action = action;
    save_locked();
    pthread_mutex_unlock(&g_mutex);
}

'''
    if marker not in text:
        raise SystemExit("config low-space API marker missing")
    text = text.replace(marker, api + marker, 1)
config_c.write_text(text, encoding="utf-8")

# ---------------------------------------------------------------------------
# Event controller: guard media-reference creation/update; don't hold the UI
# mutex over the whole filesystem scan for Delete + Media.
# ---------------------------------------------------------------------------
controller = Path("src/sr-event-controller.c")
text = controller.read_text(encoding="utf-8")
if '#include "sr-media-guard.h"' not in text:
    text = text.replace('#include "sr-event-db.h"\n', '#include "sr-event-db.h"\n#include "sr-media-guard.h"\n', 1)

old = '''static bool create_in_current_list_locked(struct sr_event_controller *controller, const struct sr_event_write *event,
                                          uint64_t *event_id)
{
    return ensure_db_locked(controller) &&
           sr_event_db_create_event_in_list(controller->db, event, controller->current_list, -1, event_id);
}
'''
if old not in text:
    old = '''static bool create_in_current_list_locked(struct sr_event_controller *controller, const struct sr_event_write *event,
\t\t\t\t\t  uint64_t *event_id)
{
\treturn ensure_db_locked(controller) &&
\t       sr_event_db_create_event_in_list(controller->db, event, controller->current_list, -1, event_id);
}
'''
new = r'''static bool create_in_current_list_locked(struct sr_event_controller *controller, const struct sr_event_write *event,
                                          uint64_t *event_id)
{
    sr_media_guard_lock();
    const bool ok = ensure_db_locked(controller) &&
                    sr_event_db_create_event_in_list(controller->db, event, controller->current_list, -1, event_id);
    sr_media_guard_unlock();
    return ok;
}
'''
if "sr_media_guard_lock();\n\tconst bool ok = ensure_db_locked(controller)" not in text and "sr_media_guard_lock();\n    const bool ok = ensure_db_locked(controller)" not in text:
    if old not in text:
        raise SystemExit("controller create helper marker missing")
    text = text.replace(old, new, 1)

old = '''\tpthread_mutex_lock(&controller->mutex);
\tconst bool ok = ensure_db_locked(controller) && sr_event_db_update_event(controller->db, event_id, event);
\tpthread_mutex_unlock(&controller->mutex);
\treturn ok;
}'''
new = '''\tpthread_mutex_lock(&controller->mutex);
\tsr_media_guard_lock();
\tconst bool ok = ensure_db_locked(controller) && sr_event_db_update_event(controller->db, event_id, event);
\tsr_media_guard_unlock();
\tpthread_mutex_unlock(&controller->mutex);
\treturn ok;
}'''
if "sr_media_guard_lock();\n\tconst bool ok = ensure_db_locked(controller) && sr_event_db_update_event" not in text:
    if old not in text:
        raise SystemExit("controller update marker missing")
    text = text.replace(old, new, 1)

old = '''\tuint64_t duplicate_id = 0;
\tconst bool ok = sr_event_db_create_event_in_list(controller->db, &write, target_list, position, &duplicate_id);
\tsr_event_record_free(&record);
'''
new = '''\tuint64_t duplicate_id = 0;
\tsr_media_guard_lock();
\tconst bool ok = sr_event_db_create_event_in_list(controller->db, &write, target_list, position, &duplicate_id);
\tsr_media_guard_unlock();
\tsr_event_record_free(&record);
'''
if "sr_media_guard_lock();\n\tconst bool ok = sr_event_db_create_event_in_list(controller->db, &write" not in text:
    if old not in text:
        raise SystemExit("controller duplicate marker missing")
    text = text.replace(old, new, 1)

start = text.index("bool sr_event_controller_delete_event_with_media(")
end = text.index("void sr_event_controller_free_event", start)
replacement = r'''bool sr_event_controller_delete_event_with_media(struct sr_event_controller *controller, uint64_t event_id,
                                                 struct sr_storage_cleanup_result *result)
{
    if (!controller || !event_id)
        return false;

    struct sr_storage_cleanup_result local = {0};
    struct sr_event_db *db = NULL;
    uint64_t in_ns = 0;
    uint64_t out_ns = 0;

    pthread_mutex_lock(&controller->mutex);
    if (!ensure_db_locked(controller)) {
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    struct sr_event_record event = {0};
    if (!sr_event_db_get_event(controller->db, event_id, &event)) {
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    if (event.protected_event) {
        sr_event_record_free(&event);
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    in_ns = event.in_ns;
    out_ns = event.out_ns;
    sr_event_record_free(&event);

    const bool deleted = sr_event_db_delete_event(controller->db, event_id);
    db = controller->db;
    pthread_mutex_unlock(&controller->mutex);

    if (deleted && !sr_storage_delete_unreferenced_range(db, in_ns, out_ns, &local))
        local.errors++;
    if (result)
        *result = local;
    return deleted;
}

'''
text = text[:start] + replacement + text[end:]
controller.write_text(text, encoding="utf-8")

# ---------------------------------------------------------------------------
# Segment writer policy wiring.
# ---------------------------------------------------------------------------
writer_h = Path("src/sr-segment-writer.h")
text = writer_h.read_text(encoding="utf-8")
if "purge_target_bytes" not in text:
    text = text.replace("\tuint64_t min_free_bytes;\n", "\tuint64_t min_free_bytes;\n\tuint64_t purge_target_bytes;\n\tint low_space_action;\n", 1)
writer_h.write_text(text, encoding="utf-8")

writer_c = Path("src/sr-segment-writer.c")
text = writer_c.read_text(encoding="utf-8")
if '#include "sr-config.h"' not in text:
    text = text.replace('#include "sr-segment-format.h"\n', '#include "sr-segment-format.h"\n#include "sr-config.h"\n#include "sr-storage-cleanup.h"\n', 1)
if "char *session_dir;" not in text:
    text = text.replace("\tchar *camera_name;\n\tchar *camera_dir;\n", "\tchar *camera_name;\n\tchar *camera_dir;\n\tchar *session_dir;\n", 1)
    text = text.replace("\tuint64_t min_free_bytes;\n\tbool reserve_blocked;\n", "\tuint64_t min_free_bytes;\n\tuint64_t purge_target_bytes;\n\tint low_space_action;\n\tuint64_t last_gc_attempt_ns;\n\tbool reserve_blocked;\n", 1)

start = text.index("static bool storage_reserve_allows(")
end = text.index("static bool open_segment(", start)
reserve_fn = r'''static bool storage_reserve_allows(struct sr_segment_writer *w, uint64_t timestamp_ns)
{
    if (!w->min_free_bytes)
        return true;

    uint64_t free_bytes = os_get_free_disk_space(w->camera_dir);
    if (free_bytes >= w->min_free_bytes) {
        if (w->reserve_blocked) {
            blog(LOG_INFO, "Sports Replay: disk reserve restored for '%s'; continuous recording resumed",
                 w->camera_name);
            w->reserve_blocked = false;
            w->reserve_recheck_after_ns = 0;
            stats_set_reserve_blocked(w, false);
        }
        return true;
    }

    if (w->low_space_action == SR_STORAGE_LOW_SPACE_WARN_ONLY) {
        if (!w->last_gc_attempt_ns || timestamp_ns - w->last_gc_attempt_ns >= 30000000000ULL) {
            blog(LOG_WARNING,
                 "Sports Replay: disk free space %.1f GB is below the %.1f GB reserve for '%s'; continuing because low-space action is Warn only",
                 (double)free_bytes / (1024.0 * 1024.0 * 1024.0),
                 (double)w->min_free_bytes / (1024.0 * 1024.0 * 1024.0), w->camera_name);
            w->last_gc_attempt_ns = timestamp_ns;
        }
        return true;
    }

    if (w->low_space_action == SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED &&
        (!w->last_gc_attempt_ns || timestamp_ns - w->last_gc_attempt_ns >= 5000000000ULL)) {
        w->last_gc_attempt_ns = timestamp_ns;
        struct sr_storage_cleanup_result gc = {0};
        const uint64_t target = w->purge_target_bytes >= w->min_free_bytes ? w->purge_target_bytes : w->min_free_bytes;
        const bool gc_ok = sr_storage_gc_reclaim_unreferenced(w->session_dir, w->camera_dir, target, &gc);
        free_bytes = os_get_free_disk_space(w->camera_dir);
        if (gc.segments_deleted || gc.errors) {
            blog(gc.errors ? LOG_WARNING : LOG_INFO,
                 "Sports Replay: storage GC for '%s': deleted %zu segment(s), pinned %zu, errors %zu, free %.1f -> %.1f GB%s",
                 w->camera_name, gc.segments_deleted, gc.segments_pinned, gc.errors,
                 (double)gc.free_bytes_before / (1024.0 * 1024.0 * 1024.0),
                 (double)gc.free_bytes_after / (1024.0 * 1024.0 * 1024.0), gc.target_reached ? ", target reached" : "");
        }
        if (!gc_ok)
            blog(LOG_WARNING, "Sports Replay: automatic storage GC could not complete for '%s'", w->camera_name);
        if (free_bytes >= w->min_free_bytes) {
            w->reserve_blocked = false;
            w->reserve_recheck_after_ns = 0;
            stats_set_reserve_blocked(w, false);
            return true;
        }
    }

    if (w->reserve_blocked && timestamp_ns < w->reserve_recheck_after_ns)
        return false;

    if (!w->reserve_blocked) {
        blog(LOG_ERROR,
             "Sports Replay: continuous recording paused for '%s': disk free space %.1f GB is below the %.1f GB reserve",
             w->camera_name, (double)free_bytes / (1024.0 * 1024.0 * 1024.0),
             (double)w->min_free_bytes / (1024.0 * 1024.0 * 1024.0));
    }
    w->reserve_blocked = true;
    w->reserve_recheck_after_ns = timestamp_ns + 1000000000ULL;
    stats_set_reserve_blocked(w, true);
    return false;
}

'''
text = text[:start] + reserve_fn + text[end:]

if "w->session_dir = bstrdup(config->session_dir);" not in text:
    text = text.replace("\tw->camera_name = bstrdup(config->camera_name);\n", "\tw->camera_name = bstrdup(config->camera_name);\n\tw->session_dir = bstrdup(config->session_dir);\n", 1)
    text = text.replace("\tw->min_free_bytes = config->min_free_bytes;\n", "\tw->min_free_bytes = config->min_free_bytes;\n\tw->purge_target_bytes = config->purge_target_bytes;\n\tw->low_space_action = config->low_space_action;\n\tif (w->low_space_action < SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED ||\n\t    w->low_space_action > SR_STORAGE_LOW_SPACE_WARN_ONLY)\n\t\tw->low_space_action = SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;\n", 1)
    text = text.replace("\tbfree(w->camera_dir);\n", "\tbfree(w->camera_dir);\n\tbfree(w->session_dir);\n", 1)
writer_c.write_text(text, encoding="utf-8")

# Capture filter passes the live storage policy into each writer instance.
capture = Path("src/capture-filter.c")
text = capture.read_text(encoding="utf-8")
if ".purge_target_bytes" not in text:
    text = text.replace("\t\t.min_free_bytes = sr_config_get_min_free_bytes(),\n\t\t.max_queue_packets = 600,\n",
                        "\t\t.min_free_bytes = sr_config_get_min_free_bytes(),\n\t\t.purge_target_bytes = sr_config_get_purge_target_bytes(),\n\t\t.low_space_action = (int)sr_config_get_low_space_action(),\n\t\t.max_queue_packets = 600,\n", 1)
capture.write_text(text, encoding="utf-8")

# CMake source list.
cmake = Path("CMakeLists.txt")
text = cmake.read_text(encoding="utf-8")
if "src/sr-media-guard.c" not in text:
    text = text.replace("    src/sr-event-controller.c\n", "    src/sr-event-controller.c\n    src/sr-media-guard.c\n", 1)
cmake.write_text(text, encoding="utf-8")

# ---------------------------------------------------------------------------
# Settings UI: purge target + low-space action.
# ---------------------------------------------------------------------------
dock = Path("src/sr-dock.cpp")
text = dock.read_text(encoding="utf-8")
if "#include <QComboBox>" not in text:
    text = text.replace("#include <QDialogButtonBox>\n", "#include <QDialogButtonBox>\n#include <QComboBox>\n", 1)
if "Dock.PurgeTarget" not in text:
    anchor = '''\t\tminFree->setValue((double)sr_config_get_min_free_bytes() / gib);
\t\tlay->addWidget(new QLabel(T("Dock.MinFree"), &dlg));
\t\tlay->addWidget(minFree);

'''
    extra = '''\t\tauto *purgeTarget = new QDoubleSpinBox(&dlg);
\t\tpurgeTarget->setRange(1.0, 10000.0);
\t\tpurgeTarget->setDecimals(1);
\t\tpurgeTarget->setSingleStep(10.0);
\t\tpurgeTarget->setSuffix(QStringLiteral(" GB"));
\t\tpurgeTarget->setValue((double)sr_config_get_purge_target_bytes() / gib);
\t\tpurgeTarget->setMinimum(minFree->value());
\t\tlay->addWidget(new QLabel(T("Dock.PurgeTarget"), &dlg));
\t\tlay->addWidget(purgeTarget);
\t\tconnect(minFree, &QDoubleSpinBox::valueChanged, purgeTarget, [purgeTarget](double value) {
\t\t\tpurgeTarget->setMinimum(value);
\t\t\tif (purgeTarget->value() < value)
\t\t\t\tpurgeTarget->setValue(value);
\t\t});

\t\tauto *lowSpaceAction = new QComboBox(&dlg);
\t\tlowSpaceAction->addItem(T("Dock.LowSpace.DeleteUnreferenced"), SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED);
\t\tlowSpaceAction->addItem(T("Dock.LowSpace.Stop"), SR_STORAGE_LOW_SPACE_STOP_RECORDING);
\t\tlowSpaceAction->addItem(T("Dock.LowSpace.Warn"), SR_STORAGE_LOW_SPACE_WARN_ONLY);
\t\tconst int actionIndex = lowSpaceAction->findData((int)sr_config_get_low_space_action());
\t\tif (actionIndex >= 0)
\t\t\tlowSpaceAction->setCurrentIndex(actionIndex);
\t\tlay->addWidget(new QLabel(T("Dock.LowSpaceAction"), &dlg));
\t\tlay->addWidget(lowSpaceAction);

'''
    if anchor not in text:
        raise SystemExit("dock min-free marker missing")
    text = text.replace(anchor, anchor + extra, 1)
    text = text.replace("\t\t\tsr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));\n\t\t\tsr_config_set_segment_duration_ms",
                        "\t\t\tsr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));\n\t\t\tsr_config_set_purge_target_bytes((uint64_t)(purgeTarget->value() * gib));\n\t\t\tsr_config_set_low_space_action(\n\t\t\t\t(enum sr_storage_low_space_action)lowSpaceAction->currentData().toInt());\n\t\t\tsr_config_set_segment_duration_ms", 1)
dock.write_text(text, encoding="utf-8")

# Locale strings.
locale = Path("data/locale/en-US.ini")
text = locale.read_text(encoding="utf-8")
if "Dock.PurgeTarget=" not in text:
    anchor = 'Dock.MinFree="Minimum free disk space"\n'
    extra = (
        'Dock.PurgeTarget="GC purge target (hysteresis)"\n'
        'Dock.LowSpaceAction="When disk reserve is reached"\n'
        'Dock.LowSpace.DeleteUnreferenced="Delete oldest unreferenced replay media"\n'
        'Dock.LowSpace.Stop="Stop continuous recording"\n'
        'Dock.LowSpace.Warn="Warn only (keep recording)"\n'
    )
    if anchor not in text:
        raise SystemExit("locale Dock.MinFree marker missing")
    text = text.replace(anchor, anchor + extra, 1)
locale.write_text(text, encoding="utf-8")

# Spanish locale: keep English fallback-quality strings rather than missing keys.
locale_es = Path("data/locale/es-ES.ini")
text = locale_es.read_text(encoding="utf-8")
if "Dock.PurgeTarget=" not in text:
    if 'Dock.MinFree=' in text:
        line = next(line for line in text.splitlines(True) if line.startswith('Dock.MinFree='))
        extra = (
            'Dock.PurgeTarget="Objetivo de espacio libre tras limpieza"\n'
            'Dock.LowSpaceAction="Al alcanzar la reserva de disco"\n'
            'Dock.LowSpace.DeleteUnreferenced="Eliminar el material de replay no referenciado más antiguo"\n'
            'Dock.LowSpace.Stop="Detener la grabación continua"\n'
            'Dock.LowSpace.Warn="Solo advertir (continuar grabando)"\n'
        )
        text = text.replace(line, line + extra, 1)
    else:
        text += ('\nDock.PurgeTarget="Objetivo de espacio libre tras limpieza"\n'
                 'Dock.LowSpaceAction="Al alcanzar la reserva de disco"\n'
                 'Dock.LowSpace.DeleteUnreferenced="Eliminar el material de replay no referenciado más antiguo"\n'
                 'Dock.LowSpace.Stop="Detener la grabación continua"\n'
                 'Dock.LowSpace.Warn="Solo advertir (continuar grabando)"\n')
locale_es.write_text(text, encoding="utf-8")

# Status documentation for runtime validation.
Path("docs/STORAGE_GC_STATUS.md").write_text(r'''# Storage GC checkpoint

This checkpoint adds conservative automatic disk reclamation to the continuous replay engine.

## Policy

- `Minimum free disk space` is the hard reserve.
- `GC purge target` is the hysteresis target; it must be >= the reserve.
- Default low-space action is **Delete oldest unreferenced replay media**.
- Automatic GC scans only the **current session** and only finalized `.srseg` + `.sridx` pairs.
- Any saved Event overlap pins a segment, regardless of the Event's Protected flag. Therefore GC cannot silently invalidate a saved Event.
- Active `.part` files are never GC candidates.
- Corrupt/uncertain entries are kept, never guessed safe.
- If GC cannot recover the reserve, continuous recording pauses. Playback and existing media remain available.
- `Warn only` intentionally allows the recorder to continue below the reserve and is not recommended for unattended operation.

## Race safety

Event creation/range update and the final `overlap query -> unlink` step share a process-wide media-reference guard. This prevents a hotkey/API/UI Event from appearing between GC's final safety check and physical file deletion while avoiding a long controller/UI lock during the filesystem scan.

## Runtime validation

1. Record 4 x 1080p50/60 until free space is just above the configured reserve.
2. Save Events across old and recent timeline areas; protect some of them.
3. Lower the reserve above current free space to force GC on the next segment rotation.
4. Verify oldest unreferenced finalized segments disappear first.
5. Verify every segment overlapping any saved Event is retained.
6. Verify `.part` files are untouched.
7. Verify recording resumes once free space is back above the hard reserve.
8. Fill the current session with only Event-pinned media and verify the writer pauses instead of deleting it.
9. Test Stop and Warn-only policies separately.
10. Repeat while rapidly creating Events during GC to exercise the media-reference guard.

Cross-session retention/purge remains a later storage-manager milestone; this checkpoint deliberately limits destructive automatic GC to the active session.
''', encoding="utf-8")
