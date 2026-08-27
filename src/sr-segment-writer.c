/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-segment-writer.h"
#include "sr-camera-identity.h"
#include "sr-segment-format.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sr_writer_packet {
	struct sr_writer_packet *next;
	AVPacket *pkt;
	uint64_t timestamp_ns;
	uint64_t epoch;
	bool keyframe;
};

struct sr_segment_writer {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t thread;
	bool thread_started;
	bool stopping;

	struct sr_writer_packet *head;
	struct sr_writer_packet *tail;
	size_t queue_depth;
	size_t max_queue_packets;
	uint64_t enqueue_epoch;
	uint64_t recording_generation;
	uint64_t write_epoch;
	bool have_write_epoch;

	char *camera_name;
	char *camera_key;
	char *camera_dir;
	uint32_t camera_hash;
	bool camera_claimed;

	enum AVCodecID codec_id;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint8_t *extradata;
	int extradata_size;
	uint64_t target_segment_ns;
	uint64_t min_free_bytes;
	bool reserve_blocked;
	uint64_t reserve_recheck_after_ns;

	uint32_t next_sequence;
	FILE *segment_file;
	FILE *index_file;
	char *segment_part_path;
	char *segment_final_path;
	char *index_part_path;
	char *index_final_path;
	uint64_t segment_start_ns;
	uint64_t last_flush_ns;
	uint32_t current_frame_number;
	bool current_segment_failed;
	bool need_keyframe;
	bool discontinuity_for_next_packet;
	bool initial_discontinuity;

	struct sr_segment_writer_stats stats;
};

struct sr_camera_writer_claim {
	char *key;
	struct sr_camera_writer_claim *next;
};

static pthread_mutex_t g_camera_claim_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct sr_camera_writer_claim *g_camera_claims;

static bool claim_camera_writer(const char *key)
{
	if (!key || !*key)
		return false;
	pthread_mutex_lock(&g_camera_claim_mutex);
	for (struct sr_camera_writer_claim *claim = g_camera_claims; claim; claim = claim->next) {
		if (strcmp(claim->key, key) == 0) {
			pthread_mutex_unlock(&g_camera_claim_mutex);
			return false;
		}
	}
	struct sr_camera_writer_claim *claim = bzalloc(sizeof(*claim));
	claim->key = bstrdup(key);
	if (!claim->key) {
		bfree(claim);
		pthread_mutex_unlock(&g_camera_claim_mutex);
		return false;
	}
	claim->next = g_camera_claims;
	g_camera_claims = claim;
	pthread_mutex_unlock(&g_camera_claim_mutex);
	return true;
}

static void release_camera_writer(const char *key)
{
	if (!key || !*key)
		return;
	pthread_mutex_lock(&g_camera_claim_mutex);
	struct sr_camera_writer_claim **link = &g_camera_claims;
	while (*link) {
		struct sr_camera_writer_claim *claim = *link;
		if (strcmp(claim->key, key) == 0) {
			*link = claim->next;
			bfree(claim->key);
			bfree(claim);
			break;
		}
		link = &claim->next;
	}
	pthread_mutex_unlock(&g_camera_claim_mutex);
}

static void free_packet_node(struct sr_writer_packet *node)
{
	if (!node)
		return;
	av_packet_free(&node->pkt);
	bfree(node);
}

static void stats_add_drop(struct sr_segment_writer *w)
{
	pthread_mutex_lock(&w->mutex);
	w->stats.packets_dropped++;
	pthread_mutex_unlock(&w->mutex);
}

static void stats_add_write(struct sr_segment_writer *w, uint64_t bytes)
{
	pthread_mutex_lock(&w->mutex);
	w->stats.packets_written++;
	w->stats.bytes_written += bytes;
	pthread_mutex_unlock(&w->mutex);
}

static void stats_set_failed(struct sr_segment_writer *w)
{
	pthread_mutex_lock(&w->mutex);
	w->stats.write_failed = true;
	pthread_mutex_unlock(&w->mutex);
}

static void stats_set_reserve_blocked(struct sr_segment_writer *w, bool blocked)
{
	pthread_mutex_lock(&w->mutex);
	w->stats.reserve_blocked = blocked;
	pthread_mutex_unlock(&w->mutex);
}

static void clear_current_paths(struct sr_segment_writer *w)
{
	bfree(w->segment_part_path);
	bfree(w->segment_final_path);
	bfree(w->index_part_path);
	bfree(w->index_final_path);
	w->segment_part_path = NULL;
	w->segment_final_path = NULL;
	w->index_part_path = NULL;
	w->index_final_path = NULL;
}

static char *make_path(const char *dir, uint32_t sequence, const char *suffix)
{
	char file[64];
	snprintf(file, sizeof(file), "%08u%s", sequence, suffix);
	struct dstr path = {0};
	dstr_copy(&path, dir);
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, file);
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

static uint32_t find_next_sequence(const char *camera_dir)
{
	struct dstr pattern = {0};
	dstr_copy(&pattern, camera_dir);
	dstr_replace(&pattern, "\\", "/");
	if (pattern.len && dstr_end(&pattern) != '/')
		dstr_cat_ch(&pattern, '/');
	dstr_cat(&pattern, "*.srseg*");
	uint32_t max_sequence = 0;
	os_glob_t *glob = NULL;
	if (os_glob(pattern.array, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			const char *path = glob->gl_pathv[i].path;
			const char *base = strrchr(path, '/');
#ifdef _WIN32
			const char *base2 = strrchr(path, '\\');
			if (!base || (base2 && base2 > base))
				base = base2;
#endif
			base = base ? base + 1 : path;
			char *end = NULL;
			unsigned long value = strtoul(base, &end, 10);
			if (end != base && value > max_sequence && value <= UINT32_MAX)
				max_sequence = (uint32_t)value;
		}
		os_globfree(glob);
	}
	dstr_free(&pattern);
	return max_sequence == UINT32_MAX ? UINT32_MAX : max_sequence + 1u;
}

static bool write_exact(FILE *f, const void *data, size_t bytes)
{
	return bytes == 0 || (data && fwrite(data, 1, bytes, f) == bytes);
}

static void close_open_files(struct sr_segment_writer *w)
{
	if (w->segment_file) {
		fflush(w->segment_file);
		fclose(w->segment_file);
		w->segment_file = NULL;
	}
	if (w->index_file) {
		fflush(w->index_file);
		fclose(w->index_file);
		w->index_file = NULL;
	}
}

static bool storage_reserve_allows(struct sr_segment_writer *w, uint64_t timestamp_ns)
{
	if (!w->min_free_bytes)
		return true;
	if (w->reserve_blocked && timestamp_ns < w->reserve_recheck_after_ns)
		return false;
	const uint64_t free_bytes = os_get_free_disk_space(w->camera_dir);
	if (free_bytes < w->min_free_bytes) {
		if (!w->reserve_blocked) {
			blog(LOG_ERROR,
			     "Pitel Instant Replay: continuous recording paused for '%s': disk free space %.1f GB is below the %.1f GB reserve",
			     w->camera_name, (double)free_bytes / (1024.0 * 1024.0 * 1024.0),
			     (double)w->min_free_bytes / (1024.0 * 1024.0 * 1024.0));
		}
		w->reserve_blocked = true;
		w->reserve_recheck_after_ns = timestamp_ns + 1000000000ULL;
		stats_set_reserve_blocked(w, true);
		return false;
	}
	if (w->reserve_blocked) {
		blog(LOG_INFO, "Pitel Instant Replay: disk reserve restored for '%s'; continuous recording resumed",
		     w->camera_name);
		w->reserve_blocked = false;
		w->reserve_recheck_after_ns = 0;
		stats_set_reserve_blocked(w, false);
	}
	return true;
}

static bool open_segment(struct sr_segment_writer *w, uint64_t start_ns, bool discontinuity)
{
	if (!storage_reserve_allows(w, start_ns))
		return false;
	if (w->next_sequence == UINT32_MAX) {
		blog(LOG_ERROR, "Pitel Instant Replay: segment sequence exhausted for camera '%s'", w->camera_name);
		stats_set_failed(w);
		return false;
	}
	const uint32_t sequence = w->next_sequence++;
	clear_current_paths(w);
	w->segment_part_path = make_path(w->camera_dir, sequence, ".srseg.part");
	w->segment_final_path = make_path(w->camera_dir, sequence, ".srseg");
	w->index_part_path = make_path(w->camera_dir, sequence, ".sridx.part");
	w->index_final_path = make_path(w->camera_dir, sequence, ".sridx");
	w->segment_file = os_fopen(w->segment_part_path, "wb");
	w->index_file = os_fopen(w->index_part_path, "wb");
	if (!w->segment_file || !w->index_file) {
		blog(LOG_ERROR, "Pitel Instant Replay: could not open segment files for camera '%s'", w->camera_name);
		close_open_files(w);
		w->current_segment_failed = true;
		stats_set_failed(w);
		return false;
	}
	struct sr_segment_file_header sh = {0};
	memcpy(sh.magic, SR_SEGMENT_MAGIC, sizeof(sh.magic));
	sh.version = SR_SEGMENT_FORMAT_VERSION;
	sh.camera_hash = w->camera_hash;
	sh.sequence = sequence;
	sh.codec_id = (uint32_t)w->codec_id;
	sh.width = w->width;
	sh.height = w->height;
	sh.fps_num = w->fps_num;
	sh.fps_den = w->fps_den;
	sh.segment_start_ns = start_ns;
	sh.extradata_size = w->extradata_size > 0 ? (uint32_t)w->extradata_size : 0;
	sh.flags = discontinuity ? SR_SEGMENT_FLAG_DISCONTINUITY : 0;
	struct sr_index_file_header ih = {0};
	memcpy(ih.magic, SR_INDEX_MAGIC, sizeof(ih.magic));
	ih.version = SR_SEGMENT_FORMAT_VERSION;
	ih.camera_hash = w->camera_hash;
	ih.sequence = sequence;
	ih.segment_start_ns = start_ns;
	const bool ok = write_exact(w->segment_file, &sh, sizeof(sh)) &&
			write_exact(w->segment_file, w->extradata, sh.extradata_size) && fflush(w->segment_file) == 0 &&
			write_exact(w->index_file, &ih, sizeof(ih)) && fflush(w->index_file) == 0;
	if (!ok) {
		blog(LOG_ERROR, "Pitel Instant Replay: failed to write segment header for camera '%s'", w->camera_name);
		w->current_segment_failed = true;
		stats_set_failed(w);
		close_open_files(w);
		return false;
	}
	w->segment_start_ns = start_ns;
	w->last_flush_ns = start_ns;
	w->current_frame_number = 0;
	w->current_segment_failed = false;
	return true;
}

static void close_segment(struct sr_segment_writer *w, bool finalize)
{
	if (!w->segment_file && !w->index_file)
		return;
	close_open_files(w);
	if (finalize && !w->current_segment_failed) {
		const int seg_rc = os_rename(w->segment_part_path, w->segment_final_path);
		const int idx_rc = os_rename(w->index_part_path, w->index_final_path);
		if (seg_rc == 0 && idx_rc == 0) {
			pthread_mutex_lock(&w->mutex);
			w->stats.segments_finalized++;
			pthread_mutex_unlock(&w->mutex);
		} else {
			blog(LOG_ERROR, "Pitel Instant Replay: could not finalize segment for camera '%s'",
			     w->camera_name);
			stats_set_failed(w);
		}
	}
	clear_current_paths(w);
	w->segment_start_ns = 0;
	w->last_flush_ns = 0;
	w->current_frame_number = 0;
	w->current_segment_failed = false;
}

static bool write_video_packet(struct sr_segment_writer *w, struct sr_writer_packet *node)
{
	if (!w->segment_file || !w->index_file)
		return false;
	const int64_t offset = os_ftelli64(w->segment_file);
	if (offset < 0)
		return false;
	struct sr_segment_packet_header ph = {0};
	ph.magic = SR_PACKET_RECORD_MAGIC;
	ph.payload_size = (uint32_t)node->pkt->size;
	ph.type = SR_SEGMENT_PACKET_VIDEO;
	if (node->keyframe)
		ph.flags |= SR_PACKET_FLAG_KEYFRAME;
	if (w->discontinuity_for_next_packet) {
		ph.flags |= SR_PACKET_FLAG_DISCONTINUITY;
		w->discontinuity_for_next_packet = false;
	}
	ph.timestamp_ns = node->timestamp_ns;
	ph.pts = node->pkt->pts;
	ph.dts = node->pkt->dts;
	ph.duration = node->pkt->duration;
	struct sr_index_entry ie = {0};
	ie.timestamp_ns = node->timestamp_ns;
	ie.file_offset = (uint64_t)offset;
	ie.packet_size = ph.payload_size;
	ie.frame_number = w->current_frame_number++;
	ie.keyframe = node->keyframe ? 1 : 0;
	if (!write_exact(w->segment_file, &ph, sizeof(ph)) ||
	    !write_exact(w->segment_file, node->pkt->data, (size_t)node->pkt->size) ||
	    !write_exact(w->index_file, &ie, sizeof(ie))) {
		w->current_segment_failed = true;
		stats_set_failed(w);
		return false;
	}
	stats_add_write(w, sizeof(ph) + (uint64_t)node->pkt->size + sizeof(ie));
	if (node->timestamp_ns - w->last_flush_ns >= 500000000ULL) {
		if (fflush(w->segment_file) != 0 || fflush(w->index_file) != 0) {
			w->current_segment_failed = true;
			stats_set_failed(w);
			return false;
		}
		w->last_flush_ns = node->timestamp_ns;
	}
	return true;
}

static struct sr_writer_packet *pop_packet(struct sr_segment_writer *w)
{
	pthread_mutex_lock(&w->mutex);
	while (!w->head && !w->stopping)
		pthread_cond_wait(&w->cond, &w->mutex);
	if (!w->head && w->stopping) {
		pthread_mutex_unlock(&w->mutex);
		return NULL;
	}
	struct sr_writer_packet *node = w->head;
	w->head = node->next;
	if (!w->head)
		w->tail = NULL;
	w->queue_depth--;
	w->stats.queue_depth = w->queue_depth;
	pthread_mutex_unlock(&w->mutex);
	return node;
}

static void *writer_thread(void *param)
{
	struct sr_segment_writer *w = param;
	os_set_thread_name("pitel-replay-writer");
	for (;;) {
		struct sr_writer_packet *node = pop_packet(w);
		if (!node)
			break;
		if (!w->have_write_epoch) {
			w->write_epoch = node->epoch;
			w->have_write_epoch = true;
		} else if (node->epoch != w->write_epoch) {
			w->write_epoch = node->epoch;
			w->need_keyframe = true;
			w->discontinuity_for_next_packet = true;
		}
		if (w->need_keyframe) {
			if (!node->keyframe) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			close_segment(w, true);
			if (!open_segment(w, node->timestamp_ns, true)) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			w->need_keyframe = false;
		}
		if (!w->segment_file) {
			if (!node->keyframe) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			const bool initial_disc = w->initial_discontinuity;
			if (!open_segment(w, node->timestamp_ns, initial_disc)) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			if (initial_disc)
				w->discontinuity_for_next_packet = true;
			w->initial_discontinuity = false;
		}
		if (node->timestamp_ns < w->segment_start_ns) {
			close_segment(w, true);
			w->need_keyframe = true;
			w->discontinuity_for_next_packet = true;
			if (!node->keyframe) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			if (!open_segment(w, node->timestamp_ns, true)) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
			w->need_keyframe = false;
		}
		if (node->keyframe && w->segment_start_ns &&
		    node->timestamp_ns - w->segment_start_ns >= w->target_segment_ns) {
			close_segment(w, true);
			if (!open_segment(w, node->timestamp_ns, false)) {
				stats_add_drop(w);
				free_packet_node(node);
				continue;
			}
		}
		if (!write_video_packet(w, node)) {
			blog(LOG_ERROR,
			     "Pitel Instant Replay: disk write failed for camera '%s'; waiting for next keyframe",
			     w->camera_name);
			close_segment(w, false);
			w->need_keyframe = true;
			w->discontinuity_for_next_packet = true;
		}
		free_packet_node(node);
	}
	close_segment(w, true);
	return NULL;
}

struct sr_segment_writer *sr_segment_writer_create(const struct sr_segment_writer_config *config)
{
	if (!config || !config->session_dir || !*config->session_dir || !config->camera_name || !*config->camera_name ||
	    !config->camera_key || !*config->camera_key || !config->width || !config->height || !config->fps_num ||
	    !config->fps_den)
		return NULL;
	struct sr_segment_writer *w = bzalloc(sizeof(*w));
	pthread_mutex_init(&w->mutex, NULL);
	pthread_cond_init(&w->cond, NULL);
	w->camera_name = bstrdup(config->camera_name);
	w->camera_key = bstrdup(config->camera_key);
	w->camera_hash = sr_camera_key_hash(config->camera_key);
	if (!w->camera_name || !w->camera_key || !claim_camera_writer(config->camera_key)) {
		blog(LOG_ERROR,
		     "Pitel Instant Replay: refusing a second continuous disk writer for camera '%s' (UUID %s)",
		     config->camera_name, config->camera_key);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	w->camera_claimed = true;
	w->codec_id = config->codec_id;
	w->width = config->width;
	w->height = config->height;
	w->fps_num = config->fps_num;
	w->fps_den = config->fps_den;
	w->extradata_size = config->extradata && config->extradata_size > 0 ? config->extradata_size : 0;
	if (w->extradata_size)
		w->extradata = bmemdup(config->extradata, (size_t)w->extradata_size);
	w->target_segment_ns = (uint64_t)(config->target_segment_ms ? config->target_segment_ms : 4000) * 1000000ULL;
	w->min_free_bytes = config->min_free_bytes;
	w->max_queue_packets = config->max_queue_packets ? config->max_queue_packets : 600;
	w->need_keyframe = false;
	w->initial_discontinuity = config->start_discontinuity;
	w->recording_generation = sr_session_recording_generation();

	/* Persist the camera identity as soon as the writer is created. Archive
	 * replay no longer depends on the source still existing in today's OBS
	 * scene collection. */
	if (!sr_session_register_camera(config->session_dir, config->camera_key, config->camera_name,
					config->sync_offset_ns))
		blog(LOG_WARNING, "Pitel Instant Replay: could not register camera '%s' in session metadata",
		     config->camera_name);

	w->camera_dir = sr_camera_directory_for_key(config->session_dir, config->camera_key);
	if (!w->camera_dir) {
		blog(LOG_ERROR, "Pitel Instant Replay: invalid persistent camera key for '%s'", w->camera_name);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	if (os_mkdirs(w->camera_dir) == MKDIR_ERROR) {
		blog(LOG_ERROR, "Pitel Instant Replay: could not create camera recording directory '%s'",
		     w->camera_dir);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	w->next_sequence = find_next_sequence(w->camera_dir);
	char *legacy_dir = sr_camera_legacy_directory(config->session_dir, config->camera_name);
	if (legacy_dir && strcmp(legacy_dir, w->camera_dir) != 0) {
		const uint32_t legacy_next = find_next_sequence(legacy_dir);
		if (legacy_next > w->next_sequence)
			w->next_sequence = legacy_next;
	}
	bfree(legacy_dir);
	if (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {
		blog(LOG_ERROR, "Pitel Instant Replay: could not start disk writer thread for camera '%s'",
		     w->camera_name);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	w->thread_started = true;
	blog(LOG_INFO,
	     "Pitel Instant Replay: continuous recorder started for '%s' (%ux%u, %.3f fps, segment %.2f s, queue %zu, reserve %.1f GB%s)",
	     w->camera_name, w->width, w->height, (double)w->fps_num / (double)w->fps_den,
	     (double)w->target_segment_ns / 1e9, w->max_queue_packets,
	     (double)w->min_free_bytes / (1024.0 * 1024.0 * 1024.0), w->initial_discontinuity ? ", resumed run" : "");
	return w;
}

void sr_segment_writer_destroy(struct sr_segment_writer *w)
{
	if (!w)
		return;
	pthread_mutex_lock(&w->mutex);
	w->stopping = true;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	if (w->thread_started)
		pthread_join(w->thread, NULL);
	while (w->head) {
		struct sr_writer_packet *node = w->head;
		w->head = node->next;
		free_packet_node(node);
	}
	close_segment(w, false);
	clear_current_paths(w);
	bfree(w->extradata);
	if (w->camera_claimed)
		release_camera_writer(w->camera_key);
	bfree(w->camera_name);
	bfree(w->camera_key);
	bfree(w->camera_dir);
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->mutex);
	bfree(w);
}

bool sr_segment_writer_push_video(struct sr_segment_writer *w, const AVPacket *pkt, uint64_t timestamp_ns)
{
	if (!w || !pkt || pkt->size <= 0)
		return false;
	if (!sr_session_recording_is_active() || sr_session_recording_generation() != w->recording_generation) {
		pthread_mutex_lock(&w->mutex);
		w->stats.packets_dropped++;
		pthread_mutex_unlock(&w->mutex);
		return false;
	}
	AVPacket *clone = av_packet_clone(pkt);
	if (!clone)
		return false;
	struct sr_writer_packet *node = bzalloc(sizeof(*node));
	node->pkt = clone;
	/* Capture/PROGRAM callbacks stay on the native OBS clock. Mapping belongs
	 * at the disk boundary so every producer (NVENC, QSV and CPU fallback)
	 * gets identical resume semantics. */
	node->timestamp_ns = sr_session_map_recording_timestamp(timestamp_ns);
	node->keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
	pthread_mutex_lock(&w->mutex);
	if (w->stopping || w->queue_depth >= w->max_queue_packets) {
		if (!w->stopping) {
			w->stats.packets_dropped++;
			w->enqueue_epoch++;
		}
		pthread_mutex_unlock(&w->mutex);
		free_packet_node(node);
		return false;
	}
	node->epoch = w->enqueue_epoch;
	if (w->tail)
		w->tail->next = node;
	else
		w->head = node;
	w->tail = node;
	w->queue_depth++;
	w->stats.queue_depth = w->queue_depth;
	if (w->queue_depth > w->stats.queue_high_watermark)
		w->stats.queue_high_watermark = w->queue_depth;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->mutex);
	return true;
}

void sr_segment_writer_get_stats(struct sr_segment_writer *w, struct sr_segment_writer_stats *stats)
{
	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!w)
		return;
	pthread_mutex_lock(&w->mutex);
	*stats = w->stats;
	stats->queue_depth = w->queue_depth;
	pthread_mutex_unlock(&w->mutex);
}
