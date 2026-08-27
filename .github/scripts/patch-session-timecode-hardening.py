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


def replace_between(text, start_marker, end_marker, replacement, label):
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:start] + replacement + text[end:]


def replace_function(text, signature, replacement, label):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"{label}: signature not found")
    brace = text.find("{", start + len(signature))
    if brace < 0:
        raise RuntimeError(f"{label}: opening brace not found")
    depth = 0
    i = brace
    in_string = False
    in_char = False
    escape = False
    line_comment = False
    block_comment = False
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if line_comment:
            if ch == "\n":
                line_comment = False
            i += 1
            continue
        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            i += 1
            continue
        if in_char:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
            i += 1
            continue
        if ch == "/" and nxt == "/":
            line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            block_comment = True
            i += 2
            continue
        if ch == '"':
            in_string = True
            i += 1
            continue
        if ch == "'":
            in_char = True
            i += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                return text[:start] + replacement + text[end:]
        i += 1
    raise RuntimeError(f"{label}: function end not found")


# ---------------------------------------------------------------------------
# Session clock core: physical media is the source of truth; first run starts 0.
# ---------------------------------------------------------------------------
rel = "src/sr-session.c"
text = read(rel)
text = replace_once(
    text,
    '#include "sr-camera-identity.h"\n#include "sr-config.h"\n#include "sr-segment-format.h"',
    '#include "sr-audio-format.h"\n#include "sr-camera-identity.h"\n#include "sr-config.h"\n#include "sr-segment-format.h"',
    "sr-session audio include",
)

scanner = r'''#define SR_INDEX_MAX_SEGMENT_SPAN_NS (10ULL * 60ULL * 1000000000ULL)
#define SR_AUDIO_VIDEO_SLOP_NS (10ULL * 1000000000ULL)

struct sr_media_bounds {
	bool have;
	uint64_t start_ns;
	uint64_t end_ns;
};

static void media_bounds_add(struct sr_media_bounds *bounds, uint64_t start_ns, uint64_t end_ns)
{
	if (!bounds || end_ns < start_ns)
		return;
	if (!bounds->have) {
		bounds->have = true;
		bounds->start_ns = start_ns;
		bounds->end_ns = end_ns;
		return;
	}
	if (start_ns < bounds->start_ns)
		bounds->start_ns = start_ns;
	if (end_ns > bounds->end_ns)
		bounds->end_ns = end_ns;
}

static bool valid_segment_timestamp(uint64_t segment_start_ns, uint64_t timestamp_ns)
{
	return timestamp_ns >= segment_start_ns &&
	       timestamp_ns - segment_start_ns <= SR_INDEX_MAX_SEGMENT_SPAN_NS;
}

static bool scan_video_index_file(const char *path, struct sr_media_bounds *bounds)
{
	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	struct sr_index_file_header header;
	bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
		  memcmp(header.magic, SR_INDEX_MAGIC, sizeof(header.magic)) == 0 &&
		  header.version == SR_SEGMENT_FORMAT_VERSION && os_fseeki64(file, 0, SEEK_END) == 0;
	const int64_t file_size = ok ? os_ftelli64(file) : -1;
	if (!ok || file_size < (int64_t)(sizeof(header) + sizeof(struct sr_index_entry))) {
		fclose(file);
		return false;
	}

	const uint64_t payload = (uint64_t)file_size - sizeof(header);
	const uint64_t entries = payload / sizeof(struct sr_index_entry);
	if (!entries) {
		fclose(file);
		return false;
	}

	struct sr_index_entry first;
	if (os_fseeki64(file, (int64_t)sizeof(header), SEEK_SET) != 0 ||
	    fread(&first, 1, sizeof(first), file) != sizeof(first) ||
	    !valid_segment_timestamp(header.segment_start_ns, first.timestamp_ns)) {
		fclose(file);
		return false;
	}

	struct sr_index_entry last = first;
	bool have_last = false;
	for (uint64_t back = 0; back < entries; back++) {
		const uint64_t index = entries - 1 - back;
		const uint64_t offset = sizeof(header) + index * sizeof(struct sr_index_entry);
		struct sr_index_entry candidate;
		if (offset > INT64_MAX || os_fseeki64(file, (int64_t)offset, SEEK_SET) != 0 ||
		    fread(&candidate, 1, sizeof(candidate), file) != sizeof(candidate))
			continue;
		if (candidate.timestamp_ns < first.timestamp_ns ||
		    !valid_segment_timestamp(header.segment_start_ns, candidate.timestamp_ns))
			continue;
		last = candidate;
		have_last = true;
		break;
	}
	fclose(file);
	if (!have_last)
		return false;
	media_bounds_add(bounds, first.timestamp_ns, last.timestamp_ns);
	return true;
}

static bool scan_audio_index_file(const char *path, struct sr_media_bounds *bounds)
{
	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	struct sr_audio_index_header header;
	bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
		  memcmp(header.magic, SR_AUDIO_INDEX_MAGIC, sizeof(header.magic)) == 0 &&
		  header.version == SR_AUDIO_FORMAT_VERSION && os_fseeki64(file, 0, SEEK_END) == 0;
	const int64_t file_size = ok ? os_ftelli64(file) : -1;
	if (!ok || file_size < (int64_t)(sizeof(header) + sizeof(struct sr_audio_index_entry))) {
		fclose(file);
		return false;
	}

	const uint64_t payload = (uint64_t)file_size - sizeof(header);
	const uint64_t entries = payload / sizeof(struct sr_audio_index_entry);
	if (!entries) {
		fclose(file);
		return false;
	}

	struct sr_audio_index_entry first;
	if (os_fseeki64(file, (int64_t)sizeof(header), SEEK_SET) != 0 ||
	    fread(&first, 1, sizeof(first), file) != sizeof(first) ||
	    !valid_segment_timestamp(header.segment_start_ns, first.timestamp_ns)) {
		fclose(file);
		return false;
	}

	struct sr_audio_index_entry last = first;
	bool have_last = false;
	for (uint64_t back = 0; back < entries; back++) {
		const uint64_t index = entries - 1 - back;
		const uint64_t offset = sizeof(header) + index * sizeof(struct sr_audio_index_entry);
		struct sr_audio_index_entry candidate;
		if (offset > INT64_MAX || os_fseeki64(file, (int64_t)offset, SEEK_SET) != 0 ||
		    fread(&candidate, 1, sizeof(candidate), file) != sizeof(candidate))
			continue;
		if (candidate.timestamp_ns < first.timestamp_ns ||
		    !valid_segment_timestamp(header.segment_start_ns, candidate.timestamp_ns))
			continue;
		last = candidate;
		have_last = true;
		break;
	}
	fclose(file);
	if (!have_last)
		return false;
	media_bounds_add(bounds, first.timestamp_ns, last.timestamp_ns);
	return true;
}

static void scan_index_pattern(const char *pattern, bool audio, struct sr_media_bounds *bounds)
{
	os_glob_t *glob = NULL;
	if (!pattern || os_glob(pattern, 0, &glob) != 0)
		return;
	for (size_t i = 0; i < glob->gl_pathc; i++) {
		if (glob->gl_pathv[i].directory)
			continue;
		if (audio)
			scan_audio_index_file(glob->gl_pathv[i].path, bounds);
		else
			scan_video_index_file(glob->gl_pathv[i].path, bounds);
	}
	os_globfree(glob);
}

static void scan_session_pattern(const char *session_dir, const char *tail, bool audio,
				 struct sr_media_bounds *bounds)
{
	char *pattern = join_path(session_dir, tail);
	if (!pattern)
		return;
	scan_index_pattern(pattern, audio, bounds);
	bfree(pattern);
}

bool sr_session_get_media_bounds(const char *session_dir, uint64_t *start_ns, uint64_t *end_ns)
{
	if (start_ns)
		*start_ns = 0;
	if (end_ns)
		*end_ns = 0;
	if (!session_dir || !*session_dir)
		return false;

	struct sr_media_bounds video = {0};
	struct sr_media_bounds audio = {0};
	scan_session_pattern(session_dir, "cam-*/*.sridx", false, &video);
	scan_session_pattern(session_dir, "cam-*/*.sridx.part", false, &video);
	scan_session_pattern(session_dir, "cam-*/*.sraidx", true, &audio);
	scan_session_pattern(session_dir, "cam-*/*.sraidx.part", true, &audio);
	scan_session_pattern(session_dir, "audio-master/*.sraidx", true, &audio);
	scan_session_pattern(session_dir, "audio-master/*.sraidx.part", true, &audio);

	struct sr_media_bounds result = {0};
	if (video.have) {
		result = video;
		if (audio.have) {
			const uint64_t video_end_slop =
				video.end_ns > UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS ? UINT64_MAX
									 : video.end_ns + SR_AUDIO_VIDEO_SLOP_NS;
			const uint64_t audio_end_slop =
				audio.end_ns > UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS ? UINT64_MAX
									 : audio.end_ns + SR_AUDIO_VIDEO_SLOP_NS;
			if (audio.start_ns <= video_end_slop && audio_end_slop >= video.start_ns) {
				if (audio.start_ns < result.start_ns &&
				    result.start_ns - audio.start_ns <= SR_AUDIO_VIDEO_SLOP_NS)
					result.start_ns = audio.start_ns;
				if (audio.end_ns > result.end_ns && audio.end_ns - result.end_ns <= SR_AUDIO_VIDEO_SLOP_NS)
					result.end_ns = audio.end_ns;
				else if (audio.end_ns > result.end_ns +
							 (result.end_ns <= UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS
								  ? SR_AUDIO_VIDEO_SLOP_NS
								  : 0))
					blog(LOG_WARNING,
					     "Pitel Instant Replay: ignoring audio timestamp tail outside video timeline in '%s'",
					     session_dir);
			}
		}
	} else if (audio.have) {
		result = audio;
	}

	if (!result.have)
		return false;
	if (start_ns)
		*start_ns = result.start_ns;
	if (end_ns)
		*end_ns = result.end_ns;
	return true;
}

static uint64_t last_media_timestamp(const char *session_dir)
{
	uint64_t start_ns = 0;
	uint64_t end_ns = 0;
	return sr_session_get_media_bounds(session_dir, &start_ns, &end_ns) ? end_ns : 0;
}

'''
text = replace_between(
    text,
    "static uint64_t scan_index_pattern",
    "static void recover_stale_recording_runs",
    scanner,
    "sr-session index scanner",
)

finish_run = r'''static void finish_recording_run_locked(uint64_t obs_now_ns)
{
	if (!g_recording_path || !g_recording_run_id)
		return;

	uint64_t media_start_ns = 0;
	uint64_t media_end_ns = 0;
	const bool have_media = sr_session_get_media_bounds(g_recording_path, &media_start_ns, &media_end_ns);
	uint64_t timeline_end = g_recording_timeline_start_ns;
	if (have_media && media_end_ns >= g_recording_timeline_start_ns)
		timeline_end = media_end_ns;

	uint64_t projected_end = g_recording_timeline_start_ns;
	if (obs_now_ns >= g_recording_obs_start_ns &&
	    obs_now_ns - g_recording_obs_start_ns <= UINT64_MAX - projected_end)
		projected_end += obs_now_ns - g_recording_obs_start_ns;
	if (projected_end > timeline_end + 2000000000ULL)
		blog(LOG_WARNING,
		     "Pitel Instant Replay: REC clock advanced to %.3f s but committed media ends at %.3f s; run end follows media",
		     (double)projected_end / 1e9, (double)timeline_end / 1e9);

	sqlite3 *sql = open_session_sqlite(g_recording_path);
	if (!sql)
		return;
	sqlite3_stmt *stmt = NULL;
	const char *query =
		"UPDATE recording_runs SET ended_unix=unixepoch(),timeline_end_ns=?,obs_end_ns=? WHERE id=?";
	if (sqlite3_prepare_v2(sql, query, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timeline_end);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)obs_now_ns);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)g_recording_run_id);
		sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	sqlite3_close(sql);
}'''
text = replace_function(text, "static void finish_recording_run_locked(uint64_t obs_now_ns)", finish_run,
                        "finish recording run")

prepare = r'''bool sr_session_prepare_recording(uint64_t obs_now_ns)
{
	pthread_mutex_lock(&g_session_mutex);
	if (g_recording_path) {
		pthread_mutex_unlock(&g_session_mutex);
		return true;
	}
	if (g_record_target_path && g_opened_path && !same_path(g_record_target_path, g_opened_path)) {
		blog(LOG_WARNING,
		     "Pitel Instant Replay: START REC refused because Opened Session and Recording Target differ; use Resume Recording explicitly");
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}
	if (!g_record_target_path && !create_session_locked(NULL, true, true, NULL)) {
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}

	g_recording_path = bstrdup(g_record_target_path);
	if (!g_recording_path) {
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}

	uint64_t media_start_ns = 0;
	uint64_t previous_end = 0;
	const bool have_media = sr_session_get_media_bounds(g_recording_path, &media_start_ns, &previous_end);
	recover_stale_recording_runs(g_recording_path, have_media ? previous_end : 0);
	g_recording_discontinuity = have_media;
	g_recording_obs_start_ns = obs_now_ns;
	if (have_media) {
		const uint64_t interval = frame_interval_ns();
		if (previous_end > UINT64_MAX - interval) {
			bfree(g_recording_path);
			g_recording_path = NULL;
			pthread_mutex_unlock(&g_session_mutex);
			return false;
		}
		g_recording_timeline_start_ns = previous_end + interval;
	} else {
		/* Session time is independent of OBS uptime. A new session always starts
		 * at zero; only deltas inside a Run are derived from the native OBS clock. */
		g_recording_timeline_start_ns = 0;
	}
	g_recording_run_id = 0;
	if (!begin_recording_run_locked(obs_now_ns, g_recording_timeline_start_ns, g_recording_discontinuity)) {
		bfree(g_recording_path);
		g_recording_path = NULL;
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}
	g_recording_generation++;
	blog(LOG_INFO, "Pitel Instant Replay: recording run %llu started at session %.3f s%s",
	     (unsigned long long)g_recording_run_id, (double)g_recording_timeline_start_ns / 1e9,
	     g_recording_discontinuity ? " (resume/discontinuity)" : "");
	pthread_mutex_unlock(&g_session_mutex);
	return true;
}'''
text = replace_function(text, "bool sr_session_prepare_recording(uint64_t obs_now_ns)", prepare,
                        "prepare recording")

write(rel, text)

rel = "src/sr-session.h"
text = read(rel)
text = replace_once(
    text,
    "bool sr_session_recording_starts_with_discontinuity(void);\n",
    "bool sr_session_recording_starts_with_discontinuity(void);\n"
    "bool sr_session_get_media_bounds(const char *session_dir, uint64_t *start_ns, uint64_t *end_ns);\n",
    "session media bounds declaration",
)
write(rel, text)

# ---------------------------------------------------------------------------
# STOP is a barrier: do not clear the session clock until all producers and
# master-audio queues have actually closed.
# ---------------------------------------------------------------------------
rel = "src/sr-capture-session.c"
text = read(rel)
text = replace_once(text, '#include "sr-capture.h"\n#include "sr-session.h"',
                    '#include "sr-capture.h"\n#include "sr-master-audio.h"\n#include "sr-session.h"',
                    "capture session master audio include")
text = replace_once(text, "#include <obs-module.h>\n", "#include <obs-module.h>\n#include <util/platform.h>\n",
                    "capture session platform include")
replacement = r'''static bool wait_for_recording_producers(uint32_t timeout_ms)
{
	const uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	for (;;) {
		struct sr_capture_recording_summary summary = {0};
		if (sr_capture_get_recording_summary_impl(&summary) && summary.active_count == 0)
			break;
		if (os_gettime_ns() >= deadline)
			return false;
		os_sleep_ms(5);
	}

	const uint64_t now = os_gettime_ns();
	const uint32_t remaining_ms = now >= deadline ? 0 : (uint32_t)((deadline - now) / 1000000ULL);
	return sr_master_audio_wait_idle(remaining_ms);
}

bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)
{
	if (enabled) {
		if (!sr_session_prepare_recording(obs_get_video_frame_time()))
			return false;
		const bool ok = sr_capture_set_all_disk_recording_impl(true, camera_count);
		if (!ok || (camera_count && *camera_count == 0)) {
			size_t ignored = 0;
			sr_capture_set_all_disk_recording_impl(false, &ignored);
			if (wait_for_recording_producers(3000))
				sr_session_finish_recording(obs_get_video_frame_time());
			else
				blog(LOG_ERROR,
				     "Pitel Instant Replay: recorder rollback did not quiesce; session remains locked to prevent cross-session timestamp corruption");
		}
		return ok;
	}

	const bool ok = sr_capture_set_all_disk_recording_impl(false, camera_count);
	if (!ok)
		return false;
	if (!wait_for_recording_producers(3000)) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: STOP timed out waiting for video/audio writers; session remains active and cannot be switched safely");
		return false;
	}
	sr_session_finish_recording(obs_get_video_frame_time());
	return true;
}'''
text = replace_function(text, "bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)", replacement,
                        "capture session stop barrier")
write(rel, text)

# ---------------------------------------------------------------------------
# Master audio cannot be rebound to another session until its worker drained.
# ---------------------------------------------------------------------------
rel = "src/sr-master-audio.h"
text = read(rel)
text = replace_once(text, "void sr_master_audio_release(void);\n",
                    "void sr_master_audio_release(void);\nbool sr_master_audio_wait_idle(uint32_t timeout_ms);\n",
                    "master audio wait declaration")
write(rel, text)

rel = "src/sr-master-audio.c"
text = read(rel)
text = replace_once(text, "\tbool active;\n\tunsigned active_refs;\n",
                    "\tbool active;\n\tbool worker_idle;\n\tunsigned active_refs;\n\tuint64_t recording_generation;\n",
                    "master audio state ownership")
text = replace_once(text, "\tstate->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;\n\tstate->sample_rate = MASTER_AUDIO_SAMPLE_RATE;",
                    "\tstate->max_queue_chunks = MASTER_AUDIO_MAX_QUEUE_CHUNKS;\n\tstate->sample_rate = MASTER_AUDIO_SAMPLE_RATE;\n\tstate->worker_idle = true;",
                    "master audio initial idle")
text = replace_once(text, "\tstate->next_segment_discontinuity = false;\n\tstate->reserve_blocked = false;",
                    "\tstate->next_segment_discontinuity = false;\n\tstate->reserve_blocked = false;",
                    "finish stream anchor")
# Add worker-idle publication at the end of finish_stream, immediately before function close.
text = replace_once(
    text,
    "\tstats_set_reserve_blocked(state, false);\n\tstate->encoder_failed_session = false;\n}\n\nstatic bool encode_chunk",
    "\tstats_set_reserve_blocked(state, false);\n\tstate->encoder_failed_session = false;\n"
    "\tpthread_mutex_lock(&state->mutex);\n"
    "\tstate->worker_idle = !state->active && state->head == NULL;\n"
    "\tpthread_cond_broadcast(&state->cond);\n"
    "\tpthread_mutex_unlock(&state->mutex);\n"
    "}\n\nstatic bool encode_chunk",
    "finish stream idle publication",
)
# Make the idle state observable without spinning when no encoder was ever opened.
old_pop = r'''static struct sr_master_audio_chunk *pop_chunk(struct sr_master_audio_state *state, bool *finalize_idle)
{
	*finalize_idle = false;
	pthread_mutex_lock(&state->mutex);
	while (!state->head && !state->stopping) {
		if (!state->active && state->encoder) {
			*finalize_idle = true;
			pthread_mutex_unlock(&state->mutex);
			return NULL;
		}
		pthread_cond_wait(&state->cond, &state->mutex);
	}

	if (!state->head) {
		pthread_mutex_unlock(&state->mutex);
		return NULL;
	}

	struct sr_master_audio_chunk *chunk = state->head;
	state->head = chunk->next;
	if (!state->head)
		state->tail = NULL;
	state->queue_depth--;
	state->stats.queue_depth = state->queue_depth;
	pthread_mutex_unlock(&state->mutex);
	return chunk;
}'''
new_pop = r'''static struct sr_master_audio_chunk *pop_chunk(struct sr_master_audio_state *state, bool *finalize_idle)
{
	*finalize_idle = false;
	pthread_mutex_lock(&state->mutex);
	while (!state->head && !state->stopping) {
		if (!state->active && state->encoder) {
			*finalize_idle = true;
			pthread_mutex_unlock(&state->mutex);
			return NULL;
		}
		if (!state->active) {
			state->worker_idle = true;
			pthread_cond_broadcast(&state->cond);
		}
		pthread_cond_wait(&state->cond, &state->mutex);
	}

	if (!state->head) {
		pthread_mutex_unlock(&state->mutex);
		return NULL;
	}

	state->worker_idle = false;
	struct sr_master_audio_chunk *chunk = state->head;
	state->head = chunk->next;
	if (!state->head)
		state->tail = NULL;
	state->queue_depth--;
	state->stats.queue_depth = state->queue_depth;
	pthread_mutex_unlock(&state->mutex);
	return chunk;
}'''
if old_pop not in text:
    raise RuntimeError("master audio pop_chunk exact block not found")
text = text.replace(old_pop, new_pop, 1)
text = replace_once(text, "\tchunk->epoch = state->enqueue_epoch;\n",
                    "\tchunk->epoch = state->enqueue_epoch;\n\tstate->worker_idle = false;\n",
                    "enqueue worker busy")

acquire = r'''bool sr_master_audio_acquire(void)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return false;
	char *audio_dir = join_path(session_dir, "audio-master");
	if (!audio_dir || os_mkdirs(audio_dir) == MKDIR_ERROR) {
		bfree(audio_dir);
		bfree(session_dir);
		return false;
	}
	const uint64_t generation = sr_session_recording_generation();

	pthread_mutex_lock(&state->mutex);
	if (state->active_refs &&
	    (state->recording_generation != generation || !state->session_dir || strcmp(state->session_dir, session_dir) != 0)) {
		pthread_mutex_unlock(&state->mutex);
		blog(LOG_ERROR,
		     "Pitel Instant Replay: refused to bind master audio to a new Session while the previous Session still owns references");
		bfree(audio_dir);
		bfree(session_dir);
		return false;
	}
	if (!state->active_refs && !state->worker_idle) {
		pthread_mutex_unlock(&state->mutex);
		blog(LOG_ERROR,
		     "Pitel Instant Replay: refused to rebind master audio before the previous Session finished draining");
		bfree(audio_dir);
		bfree(session_dir);
		return false;
	}
	if (!state->active_refs) {
		bfree(state->session_dir);
		bfree(state->audio_dir);
		state->session_dir = session_dir;
		state->audio_dir = audio_dir;
		state->recording_generation = generation;
		state->target_segment_ns = (uint64_t)sr_config_get_segment_duration_ms() * 1000000ULL;
		state->min_free_bytes = sr_config_get_low_space_action() == SR_STORAGE_LOW_SPACE_WARN_ONLY
						? 0
						: sr_config_get_min_free_bytes();
		state->reserve_blocked = false;
		state->reserve_recheck_after_ns = 0;
		state->next_sequence = find_next_sequence(state->audio_dir);
		state->next_segment_discontinuity = sr_session_recording_starts_with_discontinuity();
		state->enqueue_epoch++;
		state->active = true;
		state->worker_idle = false;
		session_dir = NULL;
		audio_dir = NULL;
	}
	state->active_refs++;
	pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->mutex);
	bfree(audio_dir);
	bfree(session_dir);
	return true;
}'''
text = replace_function(text, "bool sr_master_audio_acquire(void)", acquire, "master audio acquire ownership")

release = r'''void sr_master_audio_release(void)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return;

	pthread_mutex_lock(&state->mutex);
	if (state->active_refs)
		state->active_refs--;
	if (!state->active_refs && state->active) {
		state->active = false;
		state->worker_idle = false;
		state->enqueue_epoch++;
		pthread_cond_broadcast(&state->cond);
	}
	pthread_mutex_unlock(&state->mutex);
}'''
text = replace_function(text, "void sr_master_audio_release(void)", release, "master audio release")

wait_idle = r'''
bool sr_master_audio_wait_idle(uint32_t timeout_ms)
{
	struct sr_master_audio_state *state = g_audio;
	if (!state)
		return true;
	const uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	for (;;) {
		pthread_mutex_lock(&state->mutex);
		const bool idle = !state->active_refs && !state->active && !state->head && !state->queue_depth &&
				  state->worker_idle;
		pthread_mutex_unlock(&state->mutex);
		if (idle)
			return true;
		if (os_gettime_ns() >= deadline)
			return false;
		os_sleep_ms(2);
	}
}
'''
anchor = "\nvoid sr_master_audio_get_stats(struct sr_master_audio_stats *stats)"
if anchor not in text:
    raise RuntimeError("master audio wait insertion anchor missing")
text = text.replace(anchor, wait_idle + anchor, 1)
write(rel, text)

# Audio producer proxy never forwards raw OBS clock outside an active Recording Run.
rel = "src/sr-audio-session.c"
text = read(rel)
text = replace_once(text, "\tif (!data)\n\t\treturn;\n\n\tpthread_mutex_lock(&g_raw_audio_mutex);",
                    "\tif (!data || !sr_session_recording_is_active())\n\t\treturn;\n\n\tpthread_mutex_lock(&g_raw_audio_mutex);",
                    "raw master audio active guard")
text = replace_once(text, "\tif (!audio)\n\t\treturn false;\n",
                    "\tif (!audio || !sr_session_recording_is_active())\n\t\treturn false;\n",
                    "camera audio active guard")
write(rel, text)

# ---------------------------------------------------------------------------
# Video discontinuity must be explicit on resumed Runs.
# ---------------------------------------------------------------------------
for rel in ("src/capture-filter.c", "src/sr-program-recorder.c"):
    text = read(rel)
    marker = "\t\t.max_queue_packets = 600,\n"
    if marker not in text:
        raise RuntimeError(f"{rel}: segment writer queue marker missing")
    text = text.replace(marker, marker + "\t\t.start_discontinuity = sr_session_recording_starts_with_discontinuity(),\n", 1)
    write(rel, text)

# ---------------------------------------------------------------------------
# Event timeline: never mix native OBS time with session time; reset on OPEN.
# ---------------------------------------------------------------------------
rel = "src/sr-event-dock.cpp"
text = read(rel)
text = replace_once(text, "\tuint64_t editTimelineStartNs = 0;\n\tuint64_t editTimelineEndNs = 0;\n\tbool editTimelineHaveBounds = false;",
                    "\tuint64_t editTimelineStartNs = 0;\n\tuint64_t editTimelineEndNs = 0;\n\tbool editTimelineHaveBounds = false;\n\tQString editTimelineSessionPath;",
                    "event dock session-bound cache")
new_bounds = r'''void updateEditTimelineBounds()
	{
		char *openedRaw = sr_session_get_opened_path();
		const QString openedPath = QString::fromUtf8(openedRaw ? openedRaw : "");
		bfree(openedRaw);

		if (openedPath != editTimelineSessionPath) {
			editTimelineSessionPath = openedPath;
			editTimelineStartNs = 0;
			editTimelineEndNs = 0;
			editTimelineHaveBounds = false;
			timelineEventId = 0;
			editPreviewEventId = 0;
			editPreviewCamera.clear();
		}

		/* Physical indexes define an archived Session's bounds. This also
		 * handles legacy sessions whose first timestamp used OBS uptime rather
		 * than zero: UI time remains relative to the actual media start. */
		if (!openedPath.isEmpty() && !editTimelineHaveBounds) {
			uint64_t mediaStart = 0;
			uint64_t mediaEnd = 0;
			const QByteArray sessionUtf8 = openedPath.toUtf8();
			if (sr_session_get_media_bounds(sessionUtf8.constData(), &mediaStart, &mediaEnd) && mediaEnd >= mediaStart) {
				editTimelineStartNs = mediaStart;
				editTimelineEndNs = mediaEnd;
				editTimelineHaveBounds = mediaEnd > mediaStart;
			}
		}

		char *recordingRaw = sr_session_get_recording_path();
		const QString recordingPath = QString::fromUtf8(recordingRaw ? recordingRaw : "");
		bfree(recordingRaw);
		const bool openedIsRecording = !openedPath.isEmpty() && openedPath == recordingPath &&
					       sr_session_recording_is_active();
		if (openedIsRecording) {
			sr_capture_recording_summary recording = {};
			if (sr_capture_get_recording_summary(&recording) && recording.requested_count) {
				const uint64_t recordingStart = sr_session_recording_start_ns();
				uint64_t recordingEnd = recordingStart;
				if (recording.recording_duration_ns <= UINT64_MAX - recordingStart)
					recordingEnd += recording.recording_duration_ns;
				if (!editTimelineHaveBounds || recordingStart < editTimelineStartNs)
					editTimelineStartNs = recordingStart;
				if (!editTimelineHaveBounds || recordingEnd > editTimelineEndNs)
					editTimelineEndNs = recordingEnd;
				editTimelineHaveBounds = editTimelineEndNs > editTimelineStartNs;
			}
		}

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (controller && eventId && sr_event_controller_get_event(controller, eventId, &event)) {
			/* Events are not allowed to inflate a real media range. Legacy or
			 * damaged out-of-range Events stay visible in the list but the editor
			 * remains clamped to physical media. */
			if (!editTimelineHaveBounds) {
				editTimelineStartNs = event.in_ns;
				editTimelineEndNs = event.out_ns;
				editTimelineHaveBounds = event.out_ns > event.in_ns;
			}
			sr_event_controller_free_event(&event);
		}
	}'''
text = replace_function(text, "void updateEditTimelineBounds()", new_bounds, "event dock bounds")

# Clamp edited IN/OUT to physical timeline even if an old event had corrupt values.
sig = "void editSelectedEventRange(uint64_t inNs, uint64_t outNs)"
start = text.find(sig)
if start < 0:
    raise RuntimeError("editSelectedEventRange missing")
brace = text.find("{", start)
insert = "\n\t\tinNs = qBound(editTimelineStartNs, inNs, editTimelineEndNs);\n\t\toutNs = qBound(editTimelineStartNs, outNs, editTimelineEndNs);\n\t\tif (outNs <= inNs) {\n\t\t\tsyncTimeline();\n\t\t\treturn;\n\t\t}\n"
needle = "\t\tif (!controller || replayPlayoutActive() || !editTimelineHaveBounds || outNs <= inNs) {\n\t\t\tsyncTimeline();\n\t\t\treturn;\n\t\t}\n"
text = replace_once(text, needle, needle + insert, "event edit clamp")
write(rel, text)

# ---------------------------------------------------------------------------
# Opening a browse-only Session clears a stale target. Resume is the explicit
# action that selects a Session for recording.
# ---------------------------------------------------------------------------
rel = "src/sr-session-panel.cpp"
text = read(rel)
open_selected = r'''void openSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty())
			return;
		if (!openPath(path)) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));
			refresh();
			return;
		}
		if (!sr_session_recording_is_active())
			sr_session_clear_record_target();
		refresh();
	}'''
text = replace_function(text, "void openSelected()", open_selected, "session panel clear stale target")
write(rel, text)

print("Session timecode hardening patch applied successfully")
