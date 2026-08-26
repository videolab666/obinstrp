from pathlib import Path

p = Path('src/sr-gpu-encoder.cpp')
s = p.read_text(encoding='utf-8')

anchor = '''extern "C" bool sr_gpu_encoder_texture_encode(sr_gpu_encoder *enc, gs_texture_t *texture, AVPacket **packet)\n{\n'''
if s.count(anchor) != 1:
    raise SystemExit(f'texture encode anchor count={s.count(anchor)}')

helper = r'''#ifdef _WIN32
static bool normalize_program_texture(sr_gpu_encoder *enc, gs_texture_t *texture, ID3D11Texture2D **output)
{
	*output = nullptr;
	if (!enc || !enc->render || !texture)
		return false;

	/* obs_get_main_texture() is the final composited Program image, but its
	 * native D3D11 format/bind flags are an OBS implementation detail and are
	 * not guaranteed to be accepted directly by ID3D11VideoProcessor. Normalize
	 * it into the same known BGRA render target used by the proven ISO-camera
	 * GPU path. This is a GPU-only shader blit; there is no GPU->CPU readback. */
	gs_texrender_reset(enc->render);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool rendered = false;
	if (gs_texrender_begin_with_color_space(enc->render, enc->width, enc->height, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, static_cast<float>(enc->width), 0.0f, static_cast<float>(enc->height), -100.0f,
			 100.0f);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = effect ? gs_effect_get_param_by_name(effect, "image") : nullptr;
		if (effect && image) {
			gs_effect_set_texture_srgb(image, texture);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(texture, 0, enc->width, enc->height);
			rendered = true;
		}
		gs_texrender_end(enc->render);
	}
	gs_blend_state_pop();

	if (!rendered)
		return false;

	gs_texture_t *normalized = gs_texrender_get_texture(enc->render);
	if (!normalized)
		return false;

	ID3D11Texture2D *d3d_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(normalized));
	if (!d3d_texture)
		return false;

	ID3D11Device *texture_device = nullptr;
	d3d_texture->GetDevice(&texture_device);
	const bool same_device = texture_device == enc->device;
	if (texture_device)
		texture_device->Release();
	if (!same_device)
		return false;

	*output = d3d_texture;
	return true;
}
#endif

'''
s = s.replace(anchor, helper + anchor, 1)

old = '''\tID3D11Texture2D *source_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(texture));\n\tif (!source_texture)\n\t\treturn false;\n\tID3D11Device *texture_device = nullptr;\n\tsource_texture->GetDevice(&texture_device);\n\tconst bool same_device = texture_device == enc->device;\n\tif (texture_device)\n\t\ttexture_device->Release();\n\tif (!same_device)\n\t\treturn false;\n'''
new = '''\tID3D11Texture2D *source_texture = nullptr;\n\tif (!normalize_program_texture(enc, texture, &source_texture)) {\n\t\tif (!enc->render_failure_logged) {\n\t\t\tblog(LOG_WARNING,\n\t\t\t     "Pitel Instant Replay: PROGRAM main texture could not be normalized to the BGRA replay target");\n\t\t\tenc->render_failure_logged = true;\n\t\t}\n\t\treturn false;\n\t}\n'''
# Only the texture_encode function has this exact direct-main-texture block.
if s.count(old) != 1:
    raise SystemExit(f'direct Program texture block count={s.count(old)}')
s = s.replace(old, new, 1)

p.write_text(s, encoding='utf-8')
print('Program main texture normalization patch OK')
