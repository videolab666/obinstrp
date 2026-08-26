from pathlib import Path

ROOT = Path('.')

def load(path):
    return (ROOT / path).read_text(encoding='utf-8')

def save(path, text):
    (ROOT / path).write_text(text, encoding='utf-8')

def replace_exact(text, old, new, *, count=1, label='replacement'):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'{label}: expected {count} occurrence(s), found {actual}')
    return text.replace(old, new, count)

def insert_before(text, marker, addition, *, label='insert'):
    actual = text.count(marker)
    if actual != 1:
        raise RuntimeError(f'{label}: expected one marker, found {actual}')
    return text.replace(marker, addition + marker, 1)

# Build source list.
p = 'CMakeLists.txt'
s = load(p)
s = replace_exact(s, '    src/sr-gpu-encoder.cpp\n    src/sr-frame-cache.c',
                  '    src/sr-gpu-encoder.cpp\n    src/sr-program-recorder.c\n    src/sr-frame-cache.c',
                  label='CMake Program source')
save(p, s)

# Persistent Program selection.
p = 'src/sr-config.h'
s = load(p)
s = replace_exact(s, '#define SR_CONFIG_SCHEMA_VERSION 6', '#define SR_CONFIG_SCHEMA_VERSION 7', label='config schema')
s = insert_before(s, '#ifdef __cplusplus\n}\n#endif\n',
                  '/* Persistent Replay Setup selection for final OBS Program/PGM recording. */\n'
                  'bool sr_config_get_program_output_enabled(void);\n'
                  'void sr_config_set_program_output_enabled(bool enabled);\n\n',
                  label='config Program API')
save(p, s)

p = 'src/sr-config.c'
s = load(p)
s = replace_exact(s, 'static enum sr_replay_speed_policy g_replay_speed_policy;\n',
                  'static enum sr_replay_speed_policy g_replay_speed_policy;\nstatic bool g_program_output_enabled;\n',
                  label='config Program state')
s = replace_exact(s, '\tobs_data_set_int(data, "replay_speed_policy", (long long)g_replay_speed_policy);\n',
                  '\tobs_data_set_int(data, "replay_speed_policy", (long long)g_replay_speed_policy);\n'
                  '\tobs_data_set_bool(data, "program_output_enabled", g_program_output_enabled);\n',
                  label='config Program save')
s = replace_exact(s,
                  '\tg_replay_speed_policy = replay_speed_policy == SR_REPLAY_SPEED_EVENT ? SR_REPLAY_SPEED_EVENT\n'
                  '\t\t\t\t\t\t\t\t\t     : SR_REPLAY_SPEED_GLOBAL;\n',
                  '\tg_replay_speed_policy = replay_speed_policy == SR_REPLAY_SPEED_EVENT ? SR_REPLAY_SPEED_EVENT\n'
                  '\t\t\t\t\t\t\t\t\t     : SR_REPLAY_SPEED_GLOBAL;\n'
                  '\tg_program_output_enabled = data ? obs_data_get_bool(data, "program_output_enabled") : false;\n',
                  label='config Program load')
s += '''\n\nbool sr_config_get_program_output_enabled(void)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tconst bool value = g_program_output_enabled;\n\tpthread_mutex_unlock(&g_mutex);\n\treturn value;\n}\n\nvoid sr_config_set_program_output_enabled(bool enabled)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tg_program_output_enabled = enabled;\n\tsave_locked();\n\tpthread_mutex_unlock(&g_mutex);\n}\n'''
save(p, s)

# Program is a stable pseudo-camera understood by catalog/coverage/Event DB.
p = 'src/sr-camera-identity.h'
s = load(p)
s = replace_exact(s, '#define SR_CAMERA_STABLE_KEY_MAX 64\n',
                  '#define SR_CAMERA_STABLE_KEY_MAX 64\n#define SR_PROGRAM_CAMERA_NAME "PROGRAM"\n#define SR_PROGRAM_CAMERA_KEY "program-output-v1"\n',
                  label='Program identity constants')
s = insert_before(s, 'uint32_t sr_camera_key_hash(const char *key);\n',
                  'bool sr_camera_is_program_name(const char *camera_name);\n\n',
                  label='Program identity helper declaration')
save(p, s)

p = 'src/sr-camera-identity.c'
s = load(p)
s = replace_exact(s, 'bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size)\n',
                  'bool sr_camera_is_program_name(const char *camera_name)\n{\n\treturn camera_name && strcmp(camera_name, SR_PROGRAM_CAMERA_NAME) == 0;\n}\n\n'
                  'bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size)\n',
                  label='Program identity helper')
s = replace_exact(s,
                  '\tif (!camera_name || !*camera_name)\n\t\treturn false;\n\n\tobs_source_t *source = obs_get_source_by_name(camera_name);',
                  '\tif (!camera_name || !*camera_name)\n\t\treturn false;\n'
                  '\tif (sr_camera_is_program_name(camera_name))\n\t\treturn copy_key(SR_PROGRAM_CAMERA_KEY, key, key_size);\n\n'
                  '\tobs_source_t *source = obs_get_source_by_name(camera_name);',
                  count=1, label='Program name to key')
s = replace_exact(s,
                  '\tchar checked[SR_CAMERA_STABLE_KEY_MAX] = {0};\n\tif (!copy_key(key, checked, sizeof(checked)))\n\t\treturn NULL;\n\n\tobs_source_t *source = obs_get_source_by_uuid(checked);',
                  '\tchar checked[SR_CAMERA_STABLE_KEY_MAX] = {0};\n\tif (!copy_key(key, checked, sizeof(checked)))\n\t\treturn NULL;\n'
                  '\tif (strcmp(checked, SR_PROGRAM_CAMERA_KEY) == 0)\n\t\treturn bstrdup(SR_PROGRAM_CAMERA_NAME);\n\n'
                  '\tobs_source_t *source = obs_get_source_by_uuid(checked);',
                  label='Program key to name')
s = replace_exact(s,
                  '\tif (!camera_name || !*camera_name || !offset_ns)\n\t\treturn false;\n\t*offset_ns = 0;\n\n\tobs_source_t *source = obs_get_source_by_name(camera_name);',
                  '\tif (!camera_name || !*camera_name || !offset_ns)\n\t\treturn false;\n\t*offset_ns = 0;\n'
                  '\tif (sr_camera_is_program_name(camera_name))\n\t\treturn true;\n\n'
                  '\tobs_source_t *source = obs_get_source_by_name(camera_name);',
                  label='Program sync offset')
save(p, s)

# Encode an existing OBS GPU texture instead of re-rendering a source.
p = 'src/sr-codec.h'
s = load(p)
s = replace_exact(s,
                  'bool sr_gpu_encoder_render_encode(struct sr_gpu_encoder *enc, obs_source_t *target, AVPacket **packet);\n',
                  'bool sr_gpu_encoder_render_encode(struct sr_gpu_encoder *enc, obs_source_t *target, AVPacket **packet);\n'
                  '/* Program/PGM path: encode an existing OBS GPU texture without source re-render. */\n'
                  'bool sr_gpu_encoder_texture_encode(struct sr_gpu_encoder *enc, gs_texture_t *texture, AVPacket **packet);\n',
                  label='GPU texture API')
save(p, s)

p = 'src/sr-gpu-encoder.cpp'
s = load(p)
new_fn = r'''extern "C" bool sr_gpu_encoder_texture_encode(sr_gpu_encoder *enc, gs_texture_t *texture, AVPacket **packet)
{
	if (packet)
		*packet = nullptr;
#ifdef _WIN32
	if (!enc || !enc->ctx || !packet || !texture)
		return false;

	ID3D11Texture2D *source_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(texture));
	if (!source_texture)
		return false;
	ID3D11Device *texture_device = nullptr;
	source_texture->GetDevice(&texture_device);
	const bool same_device = texture_device == enc->device;
	if (texture_device)
		texture_device->Release();
	if (!same_device)
		return false;

	AVFrame *frame = av_frame_alloc();
	if (!frame)
		return false;
	if (av_hwframe_get_buffer(enc->hw_frames, frame, 0) < 0) {
		av_frame_free(&frame);
		return false;
	}
	frame->pts = enc->next_pts++;
	frame->color_range = AVCOL_RANGE_MPEG;
	frame->colorspace = AVCOL_SPC_BT709;
	frame->color_primaries = AVCOL_PRI_BT709;
	frame->color_trc = AVCOL_TRC_BT709;
	if (!convert_bgra_to_hw_nv12(enc, source_texture, frame)) {
		av_frame_free(&frame);
		if (!enc->render_failure_logged) {
			blog(LOG_WARNING, "Pitel Instant Replay: D3D11 Program texture-to-NV12 conversion failed; disabling Program encoder");
			enc->render_failure_logged = true;
		}
		return false;
	}
	const int send_ret = avcodec_send_frame(enc->ctx, frame);
	av_frame_free(&frame);
	if (send_ret < 0)
		return false;
	AVPacket *pkt = av_packet_alloc();
	if (!pkt)
		return false;
	const int receive_ret = avcodec_receive_packet(enc->ctx, pkt);
	if (receive_ret == AVERROR(EAGAIN) || receive_ret == AVERROR_EOF) {
		av_packet_free(&pkt);
		return true;
	}
	if (receive_ret < 0) {
		av_packet_free(&pkt);
		return false;
	}
	*packet = pkt;
	return true;
#else
	(void)enc;
	(void)texture;
	return false;
#endif
}

'''
s = insert_before(s, 'extern "C" enum AVCodecID sr_gpu_encoder_codec_id', new_fn, label='GPU texture implementation')
save(p, s)

print('Program core patch OK')
# retrigger diagnostics
