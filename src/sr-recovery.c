/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-recovery.h"

#include "sr-audio-format.h"
#include "sr-segment-format.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR_RECOVERY_MAX_EXTRADATA (1024u * 1024u)
#define SR_RECOVERY_MAX_VIDEO_PACKET (256u * 1024u * 1024u)
#define SR_RECOVERY_MAX_AUDIO_PACKET (16u * 1024u * 1024u)
#define SR_RECOVERY_COPY_BUFFER (64u * 1024u)

static bool read_exact(FILE *file, void *data, size_t bytes)
{
	return bytes == 0 || (data && fread(data, 1, bytes, file) == bytes);
}

static bool write_exact(FILE *file, const void *data, size_t bytes)
{
	return bytes == 0 || (data && fwrite(data, 1, bytes, file) == bytes);
}

static bool copy_exact(FILE *input, FILE *output, uint64_t bytes)
{
	uint8_t buffer[SR_RECOVERY_COPY_BUFFER];
	while (bytes) {
		const size_t chunk = bytes > sizeof(buffer) ? sizeof(buffer) : (size_t)bytes;
		if (!read_exact(input, buffer, chunk) || !write_exact(output, buffer, chunk))
			return false;
		bytes -= chunk;
	}
	return true;
}

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

static char *replace_suffix(const char *path, const char *old_suffix, const char *new_suffix)
{
	if (!path || !old_suffix || !new_suffix)
		return NULL;
	const size_t path_len = strlen(path);
	const size_t old_len = strlen(old_suffix);
	if (path_len < old_len || strcmp(path + path_len - old_len, old_suffix) != 0)
		return NULL;

	struct dstr result = {0};
	dstr_ncopy(&result, path, path_len - old_len);
	dstr_cat(&result, new_suffix);
	char *copy = bstrdup(result.array);
	dstr_free(&result);
	return copy;
}

static bool stopped(sr_recovery_stop_cb should_stop, void *data)
{
	return should_stop && should_stop(data);
}

static bool file_size(FILE *file, uint64_t *size)
{
	if (!file || !size || os_fseeki64(file, 0, SEEK_END) != 0)
		return false;
	const int64_t value = os_ftelli64(file);
	if (value < 0 || os_fseeki64(file, 0, SEEK_SET) != 0)
		return false;
	*size = (uint64_t)value;
	return true;
}

static void cleanup_temp(const char *media_temp, const char *index_temp)
{
	if (media_temp)
		os_unlink(media_temp);
	if (index_temp)
		os_unlink(index_temp);
}

static bool publish_recovered_pair(const char *media_source, bool source_is_part, const char *source_index_part,
				   const char *media_temp, const char *index_temp, const char *media_final,
				   const char *index_final)
{
	/* Publish the index first. Readers discover finalized media, so the pair
	 * only becomes visible after the media rename succeeds. If OBS dies in
	 * between, the next scan safely rebuilds the index again. */
	os_unlink(index_final);
	if (os_rename(index_temp, index_final) != 0)
		return false;

	if (source_is_part) {
		if (os_file_exists(media_final) || os_rename(media_temp, media_final) != 0) {
			os_unlink(index_final);
			return false;
		}
		os_unlink(media_source);
	} else {
		os_unlink(media_temp);
	}

	if (source_index_part)
		os_unlink(source_index_part);
	return true;
}

static bool recover_video_file(const char *segment_source, bool source_is_part, struct sr_recovery_result *result)
{
	char *segment_final = source_is_part ? replace_suffix(segment_source, ".srseg.part", ".srseg")
					     : bstrdup(segment_source);
	char *index_part = replace_suffix(segment_source, source_is_part ? ".srseg.part" : ".srseg", ".sridx.part");
	char *index_final = replace_suffix(segment_source, source_is_part ? ".srseg.part" : ".srseg", ".sridx");
	char *segment_temp = segment_final ? replace_suffix(segment_final, ".srseg", ".srseg.recovering") : NULL;
	char *index_temp = index_final ? replace_suffix(index_final, ".sridx", ".sridx.recovering") : NULL;
	FILE *input = NULL;
	FILE *output = NULL;
	FILE *index = NULL;
	bool recovered = false;
	uint64_t source_size = 0;
	uint64_t good_end = 0;
	size_t packet_count = 0;

	if (!segment_final || !index_part || !index_final || !segment_temp || !index_temp)
		goto cleanup;
	if (!source_is_part && os_file_exists(index_final)) {
		result->segments_skipped++;
		goto cleanup;
	}

	cleanup_temp(segment_temp, index_temp);
	input = os_fopen(segment_source, "rb");
	output = os_fopen(segment_temp, "wb");
	index = os_fopen(index_temp, "wb");
	if (!input || !output || !index || !file_size(input, &source_size))
		goto cleanup;

	struct sr_segment_file_header header;
	if (!read_exact(input, &header, sizeof(header)) || memcmp(header.magic, SR_SEGMENT_MAGIC, 8) != 0 ||
	    header.version != SR_SEGMENT_FORMAT_VERSION || header.extradata_size > SR_RECOVERY_MAX_EXTRADATA ||
	    !header.width || !header.height || !header.fps_num || !header.fps_den ||
	    source_size < sizeof(header) + header.extradata_size)
		goto cleanup;
	if (!write_exact(output, &header, sizeof(header)) || !copy_exact(input, output, header.extradata_size))
		goto cleanup;

	struct sr_index_file_header index_header = {0};
	memcpy(index_header.magic, SR_INDEX_MAGIC, sizeof(index_header.magic));
	index_header.version = SR_SEGMENT_FORMAT_VERSION;
	index_header.camera_hash = header.camera_hash;
	index_header.sequence = header.sequence;
	index_header.segment_start_ns = header.segment_start_ns;
	if (!write_exact(index, &index_header, sizeof(index_header)))
		goto cleanup;

	good_end = sizeof(header) + header.extradata_size;
	uint64_t previous_timestamp = 0;
	bool have_timestamp = false;
	while (good_end + sizeof(struct sr_segment_packet_header) <= source_size) {
		struct sr_segment_packet_header packet;
		if (!read_exact(input, &packet, sizeof(packet)))
			break;
		if (packet.magic != SR_PACKET_RECORD_MAGIC || packet.type != SR_SEGMENT_PACKET_VIDEO ||
		    packet.payload_size > SR_RECOVERY_MAX_VIDEO_PACKET ||
		    good_end + sizeof(packet) + packet.payload_size > source_size ||
		    (have_timestamp && packet.timestamp_ns < previous_timestamp) ||
		    (!packet_count && !(packet.flags & SR_PACKET_FLAG_KEYFRAME)))
			break;

		const uint64_t record_offset = good_end;
		if (!write_exact(output, &packet, sizeof(packet)) || !copy_exact(input, output, packet.payload_size))
			goto cleanup;

		struct sr_index_entry entry = {0};
		entry.timestamp_ns = packet.timestamp_ns;
		entry.file_offset = record_offset;
		entry.packet_size = packet.payload_size;
		entry.frame_number = (uint32_t)packet_count;
		entry.keyframe = (packet.flags & SR_PACKET_FLAG_KEYFRAME) ? 1 : 0;
		if (!write_exact(index, &entry, sizeof(entry)))
			goto cleanup;

		good_end += sizeof(packet) + packet.payload_size;
		previous_timestamp = packet.timestamp_ns;
		have_timestamp = true;
		packet_count++;
		if (packet_count == UINT32_MAX)
			break;
	}

	if (!packet_count || fflush(output) != 0 || fflush(index) != 0)
		goto cleanup;
	fclose(input);
	input = NULL;
	fclose(output);
	output = NULL;
	fclose(index);
	index = NULL;

	if (!publish_recovered_pair(segment_source, source_is_part, index_part, segment_temp, index_temp, segment_final,
				    index_final))
		goto cleanup;

	result->video_segments_recovered++;
	result->bytes_discarded += source_size - good_end;
	blog(LOG_INFO,
	     "Pitel Instant Replay: recovered video segment '%s' (%zu packet(s), discarded %llu tail byte(s))",
	     segment_final, packet_count, (unsigned long long)(source_size - good_end));
	recovered = true;

cleanup:
	if (input)
		fclose(input);
	if (output)
		fclose(output);
	if (index)
		fclose(index);
	if (!recovered)
		cleanup_temp(segment_temp, index_temp);
	bfree(segment_final);
	bfree(index_part);
	bfree(index_final);
	bfree(segment_temp);
	bfree(index_temp);
	return recovered;
}

static bool recover_audio_file(const char *audio_source, bool source_is_part, struct sr_recovery_result *result)
{
	char *audio_final = source_is_part ? replace_suffix(audio_source, ".sraud.part", ".sraud")
					   : bstrdup(audio_source);
	char *index_part = replace_suffix(audio_source, source_is_part ? ".sraud.part" : ".sraud", ".sraidx.part");
	char *index_final = replace_suffix(audio_source, source_is_part ? ".sraud.part" : ".sraud", ".sraidx");
	char *audio_temp = audio_final ? replace_suffix(audio_final, ".sraud", ".sraud.recovering") : NULL;
	char *index_temp = index_final ? replace_suffix(index_final, ".sraidx", ".sraidx.recovering") : NULL;
	FILE *input = NULL;
	FILE *output = NULL;
	FILE *index = NULL;
	bool recovered = false;
	uint64_t source_size = 0;
	uint64_t good_end = 0;
	size_t packet_count = 0;

	if (!audio_final || !index_part || !index_final || !audio_temp || !index_temp)
		goto cleanup;
	if (!source_is_part && os_file_exists(index_final)) {
		result->segments_skipped++;
		goto cleanup;
	}

	cleanup_temp(audio_temp, index_temp);
	input = os_fopen(audio_source, "rb");
	output = os_fopen(audio_temp, "wb");
	index = os_fopen(index_temp, "wb");
	if (!input || !output || !index || !file_size(input, &source_size))
		goto cleanup;

	struct sr_audio_file_header header;
	if (!read_exact(input, &header, sizeof(header)) || memcmp(header.magic, SR_AUDIO_MAGIC, 8) != 0 ||
	    header.version != SR_AUDIO_FORMAT_VERSION || header.extradata_size > SR_RECOVERY_MAX_EXTRADATA ||
	    !header.sample_rate || !header.channels || source_size < sizeof(header) + header.extradata_size)
		goto cleanup;
	if (!write_exact(output, &header, sizeof(header)) || !copy_exact(input, output, header.extradata_size))
		goto cleanup;

	struct sr_audio_index_header index_header = {0};
	memcpy(index_header.magic, SR_AUDIO_INDEX_MAGIC, sizeof(index_header.magic));
	index_header.version = SR_AUDIO_FORMAT_VERSION;
	index_header.sequence = header.sequence;
	index_header.segment_start_ns = header.segment_start_ns;
	if (!write_exact(index, &index_header, sizeof(index_header)))
		goto cleanup;

	good_end = sizeof(header) + header.extradata_size;
	uint64_t previous_timestamp = 0;
	bool have_timestamp = false;
	while (good_end + sizeof(struct sr_audio_packet_header) <= source_size) {
		struct sr_audio_packet_header packet;
		if (!read_exact(input, &packet, sizeof(packet)))
			break;
		if (packet.magic != SR_AUDIO_PACKET_MAGIC || packet.payload_size > SR_RECOVERY_MAX_AUDIO_PACKET ||
		    good_end + sizeof(packet) + packet.payload_size > source_size ||
		    (have_timestamp && packet.timestamp_ns < previous_timestamp))
			break;

		const uint64_t record_offset = good_end;
		if (!write_exact(output, &packet, sizeof(packet)) || !copy_exact(input, output, packet.payload_size))
			goto cleanup;

		struct sr_audio_index_entry entry = {0};
		entry.timestamp_ns = packet.timestamp_ns;
		entry.file_offset = record_offset;
		entry.packet_size = packet.payload_size;
		entry.samples =
			packet.duration > 0 && (uint64_t)packet.duration <= UINT32_MAX ? (uint32_t)packet.duration : 0;
		if (!write_exact(index, &entry, sizeof(entry)))
			goto cleanup;

		good_end += sizeof(packet) + packet.payload_size;
		previous_timestamp = packet.timestamp_ns;
		have_timestamp = true;
		packet_count++;
	}

	if (!packet_count || fflush(output) != 0 || fflush(index) != 0)
		goto cleanup;
	fclose(input);
	input = NULL;
	fclose(output);
	output = NULL;
	fclose(index);
	index = NULL;

	if (!publish_recovered_pair(audio_source, source_is_part, index_part, audio_temp, index_temp, audio_final,
				    index_final))
		goto cleanup;

	result->audio_segments_recovered++;
	result->bytes_discarded += source_size - good_end;
	blog(LOG_INFO,
	     "Pitel Instant Replay: recovered master audio segment '%s' (%zu packet(s), discarded %llu tail byte(s))",
	     audio_final, packet_count, (unsigned long long)(source_size - good_end));
	recovered = true;

cleanup:
	if (input)
		fclose(input);
	if (output)
		fclose(output);
	if (index)
		fclose(index);
	if (!recovered)
		cleanup_temp(audio_temp, index_temp);
	bfree(audio_final);
	bfree(index_part);
	bfree(index_final);
	bfree(audio_temp);
	bfree(index_temp);
	return recovered;
}

static void recover_glob(const char *dir, const char *pattern_tail, bool audio, sr_recovery_stop_cb should_stop,
			 void *stop_data, struct sr_recovery_result *result)
{
	char *pattern = join_path(dir, pattern_tail);
	os_glob_t *glob = NULL;
	if (!pattern || os_glob(pattern, 0, &glob) != 0) {
		bfree(pattern);
		return;
	}

	for (size_t i = 0; i < glob->gl_pathc && !stopped(should_stop, stop_data); i++) {
		if (glob->gl_pathv[i].directory)
			continue;
		const char *path = glob->gl_pathv[i].path;
		const bool ok = audio ? recover_audio_file(path, true, result) : recover_video_file(path, true, result);
		if (!ok) {
			result->errors++;
			blog(LOG_WARNING, "Pitel Instant Replay: could not recover interrupted %s segment '%s'",
			     audio ? "master audio" : "video", path);
		}
	}
	os_globfree(glob);
	bfree(pattern);
}

static void recover_orphan_indexes(const char *dir, const char *pattern_tail, bool audio,
				   sr_recovery_stop_cb should_stop, void *stop_data, struct sr_recovery_result *result)
{
	char *pattern = join_path(dir, pattern_tail);
	os_glob_t *glob = NULL;
	if (!pattern || os_glob(pattern, 0, &glob) != 0) {
		bfree(pattern);
		return;
	}

	for (size_t i = 0; i < glob->gl_pathc && !stopped(should_stop, stop_data); i++) {
		if (glob->gl_pathv[i].directory)
			continue;
		const char *index_part = glob->gl_pathv[i].path;
		char *media_part = replace_suffix(index_part, audio ? ".sraidx.part" : ".sridx.part",
						  audio ? ".sraud.part" : ".srseg.part");
		char *media_final =
			replace_suffix(index_part, audio ? ".sraidx.part" : ".sridx.part", audio ? ".sraud" : ".srseg");
		if (media_part && !os_file_exists(media_part) && media_final && os_file_exists(media_final)) {
			const bool ok = audio ? recover_audio_file(media_final, false, result)
					      : recover_video_file(media_final, false, result);
			if (!ok) {
				result->errors++;
				blog(LOG_WARNING, "Pitel Instant Replay: could not rebuild orphaned %s index '%s'",
				     audio ? "master audio" : "video", index_part);
			}
		}
		bfree(media_part);
		bfree(media_final);
	}
	os_globfree(glob);
	bfree(pattern);
}

static void recover_session(const char *session_dir, sr_recovery_stop_cb should_stop, void *stop_data,
			    struct sr_recovery_result *result)
{
	char *camera_pattern = join_path(session_dir, "cam-*");
	os_glob_t *cameras = NULL;
	if (camera_pattern && os_glob(camera_pattern, 0, &cameras) == 0) {
		for (size_t i = 0; i < cameras->gl_pathc && !stopped(should_stop, stop_data); i++) {
			if (!cameras->gl_pathv[i].directory)
				continue;
			const char *dir = cameras->gl_pathv[i].path;
			recover_glob(dir, "*.srseg.part", false, should_stop, stop_data, result);
			recover_orphan_indexes(dir, "*.sridx.part", false, should_stop, stop_data, result);
			recover_glob(dir, "*.sraud.part", true, should_stop, stop_data, result);
			recover_orphan_indexes(dir, "*.sraidx.part", true, should_stop, stop_data, result);
		}
		os_globfree(cameras);
	}
	bfree(camera_pattern);

	if (stopped(should_stop, stop_data))
		return;
	char *audio_dir = join_path(session_dir, "audio-master");
	if (audio_dir && os_file_exists(audio_dir)) {
		recover_glob(audio_dir, "*.sraud.part", true, should_stop, stop_data, result);
		recover_orphan_indexes(audio_dir, "*.sraidx.part", true, should_stop, stop_data, result);
	}
	bfree(audio_dir);
}

bool sr_recovery_scan_root(const char *session_root, sr_recovery_stop_cb should_stop, void *stop_data,
			   struct sr_recovery_result *result)
{
	struct sr_recovery_result local = {0};
	if (!result)
		result = &local;
	else
		memset(result, 0, sizeof(*result));
	if (!session_root || !*session_root)
		return false;

	char *pattern = join_path(session_root, "*");
	os_glob_t *sessions = NULL;
	if (!pattern || os_glob(pattern, 0, &sessions) != 0) {
		bfree(pattern);
		return true;
	}

	for (size_t i = 0; i < sessions->gl_pathc && !stopped(should_stop, stop_data); i++) {
		if (!sessions->gl_pathv[i].directory)
			continue;
		if (sr_session_path_is_active(sessions->gl_pathv[i].path))
			continue;
		recover_session(sessions->gl_pathv[i].path, should_stop, stop_data, result);
	}
	os_globfree(sessions);
	bfree(pattern);
	return result->errors == 0;
}
