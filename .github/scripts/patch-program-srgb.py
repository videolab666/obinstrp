from pathlib import Path

path = Path('src/sr-gpu-encoder.cpp')
text = path.read_text(encoding='utf-8')

old = '''\tgs_texrender_reset(enc->render);\n\tgs_blend_state_push();\n\tgs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);\n\n\tbool rendered = false;\n'''
new = '''\tgs_texrender_reset(enc->render);\n\t/* obs_get_main_texture() is sampled as sRGB below. Match OBS's own main\n\t * texture presentation path by enabling sRGB framebuffer encoding while\n\t * writing the normalized BGRA target, otherwise linearized values are\n\t * stored as UNORM and the recorded PROGRAM image becomes too dark. */\n\tconst bool previous_srgb = gs_framebuffer_srgb_enabled();\n\tgs_enable_framebuffer_srgb(true);\n\tgs_blend_state_push();\n\tgs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);\n\n\tbool rendered = false;\n'''

# There are two texrender users. Patch only normalize_program_texture(), not the
# ISO source render path.
marker = 'static bool normalize_program_texture'
pos = text.find(marker)
if pos < 0:
    raise RuntimeError('normalize_program_texture not found')
prefix = text[:pos]
body = text[pos:]
if body.count(old) != 1:
    raise RuntimeError(f'expected one Program render-state block, found {body.count(old)}')
body = body.replace(old, new, 1)

old2 = '''\tgs_blend_state_pop();\n\n\tif (!rendered)\n'''
new2 = '''\tgs_blend_state_pop();\n\tgs_enable_framebuffer_srgb(previous_srgb);\n\n\tif (!rendered)\n'''
if body.count(old2) != 1:
    raise RuntimeError(f'expected one Program render-state restore block, found {body.count(old2)}')
body = body.replace(old2, new2, 1)

path.write_text(prefix + body, encoding='utf-8')
print('Program sRGB normalization patch applied')
