/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-segment-writer.h"
#include "sr-segment-format.h"

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
	bool overflow_pending;

	char *camera_name;
	char *camera_dir;
	uint32_t camera_hash;

	enum AVCodecID codec_id;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint8_t *extradata;
	int extradata_size;
	uint64_t target_segment_ns;

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

	struct sr_segment_writer_stats stats;
};

static uint32_t fnv1a_32(const char *s)
{
	uint32_t h = 2166136261u;
	if (!s)
		return h;
	while (*s) {
		h ^= (uint8_t)*s++;
		h *= 16777619u;
	}
	return h;
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
	return max_sequence + 1u;
}

static bool write_exact(FILE *f, const void *data, size_t bytes)
{
	return bytes == 0 || fwrite(data, 1, bytes, f) == bytes;
}

static bool open_segment(struct sr_segment_writer *w, uint64_t start_ns, bool discontinuity)
{
	const uint32_t sequence = w->next_sequence++;
	clear_current_paths(w);
	w->segment_part_path = make_path(w->camera_dir, sequence, ".srseg.part");
	w->segment_final_path = make_path(w->camera_dir, sequence, ".srseg");
	w->index_part_path = make_path(w->camera_dir, sequence, ".sridx.part");
	w->index_final_path = make_path(w->camera_dir, sequence, ".sridx");

	w->segment_file = os_fopen(w->segment_part_path, "wb");
	w->index_file = os_fopen(w->index_part_path, "wb");
	if (!w->segment_file || !w->index_file) {
		obs_log(LOG_ERROR, "Sports Replay: could not open segment files for camera '%s'", w->camera_name);
		if (w->segment_file)
			fclose(w->segment_file);
		if (w->index_file)
			fclose(w->index_file);
		w->segment_file = NULL;
		w->index_file = NULL;
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

	bool ok = write_exact(w->segment_file, &sh, sizeof(sh)) &&
		      write_exact(w->segment_file, w->extradata, sh.extradata_size) &&
		      write_exact(w->index_file, &ih, sizeof(ih));
	if (!ok) {
		obs_log(LOG_ERROR, "Sports Replay: failed to write segment header for camera '%s'", w->camera_name);
		w->current_segment_failed = true;
		stats_set_failed(w);
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

	if (finalize && !w->current_segment_failed) {
		const int seg_rc = os_rename(w->segment_part_path, w->segment_final_path);
		const int idx_rc = os_rename(w->index_part_path, w->index_final_path);
		if (seg_rc == 0 && idx_rc == 0) {
			pthread_mutex_lock(&w->mutex);
			w->stats.segments_finalized++;
			pthread_mutex_unlock(&w->mutex);
		} else {
			obs_log(LOG_ERROR, "Sports Replay: could not finalize segment for camera '%s'", w->camera_name);
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

	/* Make the active .part reasonably current for the future live reader
	 * without flushing every frame. With the current All-I encoder this is
	 * approximately twice per second; with short GOP it follows keyframes. */
	if (node->keyframe && node->timestamp_ns - w->last_flush_ns >= 500000000ULL) {
		fflush(w->segment_file);
		fflush(w->index_file);
		w->last_flush_ns = node->timestamp_ns;
	}
	return true;
}

static struct sr_writer_packet *pop_packet(struct sr_segment_writer *w, bool *overflow)
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
	*overflow = w->overflow_pending;
	w->overflow_pending = false;
	pthread_mutex_unlock(&w->mutex);
	return node;
}

static void *writer_thread(void *param)
{
	struct sr_segment_writer *w = param;

	for (;;) {
		bool overflow = false;
		struct sr_writer_packet *node = pop_packet(w, &overflow);
		if (!node)
			break;

		if (overflow) {
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
			if (!open_segment(w, node->timestamp_ns, false)) {
				free_packet_node(node);
				continue;
			}
		}

		if (node->keyframe && w->segment_start_ns &&
		    node->timestamp_ns - w->segment_start_ns >= w->target_segment_ns) {
			close_segment(w, true);
			if (!open_segment(w, node->timestamp_ns, false)) {
				free_packet_node(node);
				continue;
			}
		}

		if (!write_video_packet(w, node)) {
			obs_log(LOG_ERROR, "Sports Replay: disk write failed for camera '%s'; waiting for next keyframe",
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
	    !config->width || !config->height || !config->fps_num || !config->fps_den)
		return NULL;

	struct sr_segment_writer *w = bzalloc(sizeof(*w));
	pthread_mutex_init(&w->mutex, NULL);
	pthread_cond_init(&w->cond, NULL);

	w->camera_name = bstrdup(config->camera_name);
	w->camera_hash = fnv1a_32(config->camera_name);
	w->codec_id = config->codec_id;
	w->width = config->width;
	w->height = config->height;
	w->fps_num = config->fps_num;
	w->fps_den = config->fps_den;
	w->extradata_size = config->extradata_size > 0 ? config->extradata_size : 0;
	if (w->extradata_size)
		w->extradata = bmemdup(config->extradata, (size_t)w->extradata_size);
	w->target_segment_ns = (uint64_t)(config->target_segment_ms ? config->target_segment_ms : 4000) * 1000000ULL;
	w->max_queue_packets = config->max_queue_packets ? config->max_queue_packets : 600;
	w->need_keyframe = true;

	char camera_folder[32];
	snprintf(camera_folder, sizeof(camera_folder), "cam-%08x", w->camera_hash);
	struct dstr dir = {0};
	dstr_copy(&dir, config->session_dir);
	dstr_replace(&dir, "\\", "/");
	if (dir.len && dstr_end(&dir) != '/')
		dstr_cat_ch(&dir, '/');
	dstr_cat(&dir, camera_folder);
	w->camera_dir = bstrdup(dir.array);
	dstr_free(&dir);

	if (os_mkdirs(w->camera_dir) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Sports Replay: could not create camera recording directory '%s'", w->camera_dir);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	w->next_sequence = find_next_sequence(w->camera_dir);

	if (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {
		obs_log(LOG_ERROR, "Sports Replay: could not start disk writer thread for camera '%s'", w->camera_name);
		sr_segment_writer_destroy(w);
		return NULL;
	}
	w->thread_started = true;

	obs_log(LOG_INFO,
		"Sports Replay: continuous recorder started for '%s' (%ux%u, %.3f fps, segment %.2f s, queue %zu)",
		w->camera_name, w->width, w->height, (double)w->fps_num / (double)w->fps_den,
		(double)w->target_segment_ns / 1e9, w->max_queue_packets);
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
	bfree(w->camera_name);
	bfree(w->camera_dir);
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->mutex);
	bfree(w);
}

bool sr_segment_writer_push_video(struct sr_segment_writer *w, const AVPacket *pkt, uint64_t timestamp_ns)
{
	if (!w || !pkt || pkt->size <= 0)
		return false;

	AVPacket *clone = av_packet_clone(pkt);
	if (!clone)
		return false;

	struct sr_writer_packet *node = bzalloc(sizeof(*node));
	node->pkt = clone;
	node->timestamp_ns = timestamp_ns;
	node->keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

	pthread_mutex_lock(&w->mutex);
	if (w->stopping || w->queue_depth >= w->max_queue_packets) {
		if (!w->stopping) {
			w->stats.packets_dropped++;
			w->overflow_pending = true;
		}
		pthread_mutex_unlock(&w->mutex);
		free_packet_node(node);
		return false;
	}

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
