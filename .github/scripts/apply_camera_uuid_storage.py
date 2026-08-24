from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


Path("src/sr-camera-identity.h").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CAMERA_STABLE_KEY_MAX 64

/* OBS source UUIDs are the persistent camera identity. Display names remain
 * operator-facing labels only and may be renamed without moving new replay
 * media to another directory. */
bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size);
bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size);

uint32_t sr_camera_key_hash(const char *key);

/* Returned paths use bmalloc/bstrdup semantics and must be released with
 * bfree(). The legacy directory helper preserves read compatibility with
 * recordings created before UUID-backed storage was introduced. */
char *sr_camera_directory_for_key(const char *session_dir, const char *key);
char *sr_camera_legacy_directory(const char *session_dir, const char *camera_name);

#ifdef __cplusplus
}
#endif
''', encoding="utf-8")

Path("src/sr-camera-identity.c").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-identity.h"

#include <util/bmem.h>
#include <util/dstr.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool valid_key_char(unsigned char c)
{
    return isalnum(c) || c == '-' || c == '_';
}

static bool copy_key(const char *value, char *key, size_t key_size)
{
    if (!value || !*value || !key || key_size < 2)
        return false;

    const size_t length = strlen(value);
    if (length >= key_size)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (!valid_key_char((unsigned char)value[i]))
            return false;
    }

    memcpy(key, value, length + 1);
    return true;
}

bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size)
{
    if (!source)
        return false;
    return copy_key(obs_source_get_uuid(source), key, key_size);
}

bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size)
{
    if (!camera_name || !*camera_name)
        return false;

    obs_source_t *source = obs_get_source_by_name(camera_name);
    if (!source)
        return false;
    const bool ok = sr_camera_key_from_source(source, key, key_size);
    obs_source_release(source);
    return ok;
}

uint32_t sr_camera_key_hash(const char *key)
{
    uint32_t hash = 2166136261u;
    if (!key)
        return hash;
    while (*key) {
        hash ^= (uint8_t)*key++;
        hash *= 16777619u;
    }
    return hash;
}

static char *append_camera_folder(const char *session_dir, const char *folder)
{
    if (!session_dir || !*session_dir || !folder || !*folder)
        return NULL;

    struct dstr path = {0};
    dstr_copy(&path, session_dir);
    dstr_replace(&path, "\\", "/");
    if (path.len && dstr_end(&path) != '/')
        dstr_cat_ch(&path, '/');
    dstr_cat(&path, folder);

    char *result = bstrdup(path.array);
    dstr_free(&path);
    return result;
}

char *sr_camera_directory_for_key(const char *session_dir, const char *key)
{
    char checked[SR_CAMERA_STABLE_KEY_MAX] = {0};
    if (!copy_key(key, checked, sizeof(checked)))
        return NULL;

    struct dstr folder = {0};
    dstr_copy(&folder, "cam-");
    dstr_cat(&folder, checked);
    char *result = append_camera_folder(session_dir, folder.array);
    dstr_free(&folder);
    return result;
}

char *sr_camera_legacy_directory(const char *session_dir, const char *camera_name)
{
    if (!camera_name || !*camera_name)
        return NULL;
    char folder[32];
    snprintf(folder, sizeof(folder), "cam-%08x", sr_camera_key_hash(camera_name));
    return append_camera_folder(session_dir, folder);
}
''', encoding="utf-8")

cmake = Path("CMakeLists.txt")
replace_once(cmake, "    src/sr-camera-list.c\n", "    src/sr-camera-list.c\n    src/sr-camera-identity.c\n")

capture = Path("src/capture-filter.c")
replace_once(capture, '#include "sr-buffer.h"\n', '#include "sr-buffer.h"\n#include "sr-camera-identity.h"\n')
replace_once(
    capture,
    '''static const char *capture_camera_name(struct sr_capture *c)\n{\n\tobs_source_t *parent = obs_filter_get_parent(c->self);\n\treturn parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);\n}\n''',
    '''static obs_source_t *capture_camera_source(struct sr_capture *c)\n{\n\treturn c ? obs_filter_get_parent(c->self) : NULL;\n}\n\nstatic const char *capture_camera_name(struct sr_capture *c)\n{\n\tobs_source_t *parent = capture_camera_source(c);\n\treturn parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);\n}\n''',
)
replace_once(
    capture,
    '''\tconst uint8_t *extradata = NULL;\n\tint extradata_size = 0;\n\tsr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);\n\n\tstruct sr_segment_writer_config cfg = {\n\t\t.session_dir = session_dir,\n\t\t.camera_name = capture_camera_name(c),\n''',
    '''\tconst uint8_t *extradata = NULL;\n\tint extradata_size = 0;\n\tsr_encoder_get_extradata(c->encoder, &extradata, &extradata_size);\n\n\tobs_source_t *camera_source = capture_camera_source(c);\n\tchar camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};\n\tif (!camera_source || !sr_camera_key_from_source(camera_source, camera_key, sizeof(camera_key))) {\n\t\tobs_log(LOG_ERROR, "'%s': could not resolve persistent OBS UUID for replay camera '%s'",\n\t\t\tobs_source_get_name(c->self), capture_camera_name(c));\n\t\tbfree(session_dir);\n\t\tc->writer_failed = true;\n\t\treturn false;\n\t}\n\n\tstruct sr_segment_writer_config cfg = {\n\t\t.session_dir = session_dir,\n\t\t.camera_name = capture_camera_name(c),\n\t\t.camera_key = camera_key,\n''',
)

writer_h = Path("src/sr-segment-writer.h")
replace_once(writer_h, "\tconst char *camera_name;\n", "\tconst char *camera_name;\n\tconst char *camera_key; /* persistent OBS source UUID */\n")

writer = Path("src/sr-segment-writer.c")
replace_once(writer, '#include "sr-segment-writer.h"\n', '#include "sr-segment-writer.h"\n#include "sr-camera-identity.h"\n')
replace_once(writer, "\tchar *camera_name;\n\tchar *camera_dir;\n", "\tchar *camera_name;\n\tchar *camera_key;\n\tchar *camera_dir;\n")
old_fnv = '''static uint32_t fnv1a_32(const char *s)\n{\n\tuint32_t h = 2166136261u;\n\tif (!s)\n\t\treturn h;\n\twhile (*s) {\n\t\th ^= (uint8_t)*s++;\n\t\th *= 16777619u;\n\t}\n\treturn h;\n}\n\n'''
text = writer.read_text(encoding="utf-8")
if old_fnv in text:
    writer.write_text(text.replace(old_fnv, "", 1), encoding="utf-8")
replace_once(
    writer,
    '''\tif (!config || !config->session_dir || !*config->session_dir || !config->camera_name || !*config->camera_name ||\n\t    !config->width || !config->height || !config->fps_num || !config->fps_den)\n''',
    '''\tif (!config || !config->session_dir || !*config->session_dir || !config->camera_name || !*config->camera_name ||\n\t    !config->camera_key || !*config->camera_key || !config->width || !config->height || !config->fps_num ||\n\t    !config->fps_den)\n''',
)
replace_once(
    writer,
    '''\tw->camera_name = bstrdup(config->camera_name);\n\tw->camera_hash = fnv1a_32(config->camera_name);\n''',
    '''\tw->camera_name = bstrdup(config->camera_name);\n\tw->camera_key = bstrdup(config->camera_key);\n\tw->camera_hash = sr_camera_key_hash(config->camera_key);\n''',
)
old_dir = '''\tchar camera_folder[32];\n\tsnprintf(camera_folder, sizeof(camera_folder), "cam-%08x", w->camera_hash);\n\tstruct dstr dir = {0};\n\tdstr_copy(&dir, config->session_dir);\n\tdstr_replace(&dir, "\\\\", "/");\n\tif (dir.len && dstr_end(&dir) != '/')\n\t\tdstr_cat_ch(&dir, '/');\n\tdstr_cat(&dir, camera_folder);\n\tw->camera_dir = bstrdup(dir.array);\n\tdstr_free(&dir);\n\n'''
new_dir = '''\tw->camera_dir = sr_camera_directory_for_key(config->session_dir, config->camera_key);\n\tif (!w->camera_dir) {\n\t\tblog(LOG_ERROR, "Sports Replay: invalid persistent camera key for '%s'", w->camera_name);\n\t\tsr_segment_writer_destroy(w);\n\t\treturn NULL;\n\t}\n\n'''
replace_once(writer, old_dir, new_dir)
replace_once(
    writer,
    '''\tw->next_sequence = find_next_sequence(w->camera_dir);\n\n\tif (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {\n''',
    '''\tw->next_sequence = find_next_sequence(w->camera_dir);\n\tchar *legacy_dir = sr_camera_legacy_directory(config->session_dir, config->camera_name);\n\tif (legacy_dir && strcmp(legacy_dir, w->camera_dir) != 0) {\n\t\tconst uint32_t legacy_next = find_next_sequence(legacy_dir);\n\t\tif (legacy_next > w->next_sequence)\n\t\t\tw->next_sequence = legacy_next;\n\t}\n\tbfree(legacy_dir);\n\n\tif (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {\n''',
)
replace_once(writer, "\tbfree(w->camera_name);\n\tbfree(w->camera_dir);\n", "\tbfree(w->camera_name);\n\tbfree(w->camera_key);\n\tbfree(w->camera_dir);\n")

Path("src/sr-segment-catalog.c").write_text(r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-segment-catalog.h"
#include "sr-camera-identity.h"
#include "sr-segment-reader.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>

static bool ends_with(const char *value, const char *suffix)
{
    if (!value || !suffix)
        return false;
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len && memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

static char *replace_suffix(const char *path, const char *old_suffix, const char *new_suffix)
{
    if (!ends_with(path, old_suffix))
        return NULL;

    const size_t path_len = strlen(path);
    const size_t old_len = strlen(old_suffix);

    struct dstr out = {0};
    dstr_ncopy(&out, path, path_len - old_len);
    dstr_cat(&out, new_suffix);
    char *result = bstrdup(out.array);
    dstr_free(&out);
    return result;
}

static int descriptor_compare(const void *a, const void *b)
{
    const struct sr_segment_descriptor *sa = a;
    const struct sr_segment_descriptor *sb = b;
    if (sa->start_ns < sb->start_ns)
        return -1;
    if (sa->start_ns > sb->start_ns)
        return 1;
    if (sa->sequence < sb->sequence)
        return -1;
    if (sa->sequence > sb->sequence)
        return 1;
    if (sa->active != sb->active)
        return sa->active ? 1 : -1;
    return 0;
}

static bool append_descriptor(struct sr_segment_descriptor **items, size_t *count, size_t *capacity,
                              const char *segment_path, const char *index_path, bool active)
{
    struct sr_segment_reader *reader = sr_segment_reader_open(segment_path, index_path);
    if (!reader)
        return false;

    struct sr_segment_stream_info info;
    if (!sr_segment_reader_get_info(reader, &info)) {
        sr_segment_reader_close(reader);
        return false;
    }

    uint64_t end_ns = info.segment_start_ns;
    if (info.indexed_packets) {
        struct sr_index_entry last;
        if (sr_segment_reader_find(reader, UINT64_MAX, false, &last))
            end_ns = last.timestamp_ns;
    }

    if (*count == *capacity) {
        const size_t next_capacity = *capacity ? *capacity * 2 : 32;
        struct sr_segment_descriptor *next = brealloc(*items, next_capacity * sizeof(**items));
        if (!next) {
            sr_segment_reader_close(reader);
            return false;
        }
        *items = next;
        *capacity = next_capacity;
    }

    struct sr_segment_descriptor *dst = &(*items)[(*count)++];
    memset(dst, 0, sizeof(*dst));
    dst->sequence = info.sequence;
    dst->start_ns = info.segment_start_ns;
    dst->end_ns = end_ns;
    dst->fps_num = info.fps_num;
    dst->fps_den = info.fps_den;
    dst->flags = info.segment_flags;
    dst->active = active;
    dst->segment_path = bstrdup(segment_path);
    dst->index_path = bstrdup(index_path);

    sr_segment_reader_close(reader);
    return true;
}

static void scan_directory(const char *camera_dir, struct sr_segment_descriptor **items, size_t *count,
                           size_t *capacity)
{
    if (!camera_dir || !*camera_dir)
        return;

    struct dstr pattern = {0};
    dstr_copy(&pattern, camera_dir);
    dstr_replace(&pattern, "\\", "/");
    if (pattern.len && dstr_end(&pattern) != '/')
        dstr_cat_ch(&pattern, '/');
    dstr_cat(&pattern, "*.srseg*");

    os_glob_t *glob = NULL;
    if (os_glob(pattern.array, 0, &glob) == 0) {
        for (size_t i = 0; i < glob->gl_pathc; i++) {
            if (glob->gl_pathv[i].directory)
                continue;

            const char *segment_path = glob->gl_pathv[i].path;
            const bool active = ends_with(segment_path, ".srseg.part");
            const bool finalized = ends_with(segment_path, ".srseg") && !active;
            if (!active && !finalized)
                continue;

            char *index_path = active ? replace_suffix(segment_path, ".srseg.part", ".sridx.part")
                                      : replace_suffix(segment_path, ".srseg", ".sridx");
            if (!index_path)
                continue;

            if (os_file_exists(index_path))
                append_descriptor(items, count, capacity, segment_path, index_path, active);
            bfree(index_path);
        }
        os_globfree(glob);
    }
    dstr_free(&pattern);
}

bool sr_segment_catalog_scan(const char *session_dir, const char *camera_name, struct sr_segment_descriptor **segments,
                             size_t *count)
{
    if (!segments || !count)
        return false;
    *segments = NULL;
    *count = 0;

    if (!session_dir || !*session_dir || !camera_name || !*camera_name)
        return false;

    struct sr_segment_descriptor *items = NULL;
    size_t item_count = 0;
    size_t capacity = 0;

    char key[SR_CAMERA_STABLE_KEY_MAX] = {0};
    char *stable_dir = NULL;
    if (sr_camera_key_from_name(camera_name, key, sizeof(key)))
        stable_dir = sr_camera_directory_for_key(session_dir, key);
    char *legacy_dir = sr_camera_legacy_directory(session_dir, camera_name);

    /* New recordings are keyed by the persistent OBS source UUID. Also scan
     * the old display-name hash directory so a session started with an older
     * plugin remains replayable after upgrading. */
    scan_directory(stable_dir, &items, &item_count, &capacity);
    if (!stable_dir || !legacy_dir || strcmp(stable_dir, legacy_dir) != 0)
        scan_directory(legacy_dir, &items, &item_count, &capacity);

    bfree(stable_dir);
    bfree(legacy_dir);

    if (item_count > 1)
        qsort(items, item_count, sizeof(*items), descriptor_compare);

    *segments = items;
    *count = item_count;
    return true;
}

void sr_segment_catalog_free(struct sr_segment_descriptor *segments, size_t count)
{
    if (!segments)
        return;
    for (size_t i = 0; i < count; i++) {
        bfree(segments[i].segment_path);
        bfree(segments[i].index_path);
    }
    bfree(segments);
}

const struct sr_segment_descriptor *sr_segment_catalog_find(const struct sr_segment_descriptor *segments, size_t count,
                                                            uint64_t timestamp_ns)
{
    if (!segments || !count)
        return NULL;

    /* Sorted by start time. Binary-search the newest segment whose start is
     * at/before the requested timestamp, then walk backward across rare
     * overlapping ranges until a containing segment is found. */
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (segments[mid].start_ns <= timestamp_ns)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;

    for (size_t i = lo; i > 0; i--) {
        const struct sr_segment_descriptor *candidate = &segments[i - 1];
        if (candidate->start_ns <= timestamp_ns && candidate->end_ns >= timestamp_ns)
            return candidate;
    }
    return NULL;
}
''', encoding="utf-8")

print("UUID camera storage integration applied")
