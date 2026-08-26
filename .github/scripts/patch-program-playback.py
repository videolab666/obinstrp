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

# PROGRAM remains a manual/fallback angle but is excluded from automatic camera tours.
p = 'src/sr-replay-playlist.c'
s = load(p)
s = replace_exact(s, '#include "sr-camera-list.h"\n',
                  '#include "sr-camera-list.h"\n#include "sr-camera-identity.h"\n',
                  label='playlist identity include')
s = replace_exact(s,
                  '\tfor (size_t i = 0; i < cameras.count; i++) {\n\t\tstruct sr_replay_coverage_info coverage = {0};\n'
                  '\t\tif (!sr_replay_coverage_query(cameras.names[i], event.in_ns, event.out_ns, &coverage))',
                  '\tfor (size_t i = 0; i < cameras.count; i++) {\n'
                  '\t\tif (sr_camera_is_program_name(cameras.names[i]))\n\t\t\tcontinue;\n'
                  '\t\tstruct sr_replay_coverage_info coverage = {0};\n'
                  '\t\tif (!sr_replay_coverage_query(cameras.names[i], event.in_ns, event.out_ns, &coverage))',
                  count=1, label='exclude Program from angle count')
s = replace_exact(s,
                  '\tfor (size_t i = 0; i < cameras.count && actual < wanted_count; i++) {\n'
                  '\t\tstruct sr_replay_coverage_info coverage = {0};',
                  '\tfor (size_t i = 0; i < cameras.count && actual < wanted_count; i++) {\n'
                  '\t\tif (sr_camera_is_program_name(cameras.names[i]))\n\t\t\tcontinue;\n'
                  '\t\tstruct sr_replay_coverage_info coverage = {0};',
                  label='exclude Program from angle materialization')
save(p, s)

# Selected-camera audio for PROGRAM is its native final mix: master audio.
p = 'src/sr-event-output.c'
s = load(p)
s = replace_exact(s,
                  '\tconst bool camera_audio =\n\t\toutput->audio_mode == SR_EVENT_OUTPUT_AUDIO_CAMERA ||\n'
                  '\t\t(output->audio_mode == SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS && state->audio_mode == SR_REPLAY_AUDIO_CAMERA);\n',
                  '\tconst bool requested_camera_audio =\n\t\toutput->audio_mode == SR_EVENT_OUTPUT_AUDIO_CAMERA ||\n'
                  '\t\t(output->audio_mode == SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS && state->audio_mode == SR_REPLAY_AUDIO_CAMERA);\n'
                  '\t/* PROGRAM has no separate ISO audio track: its native audio is the final OBS mix. */\n'
                  '\tconst bool camera_audio = requested_camera_audio && !sr_camera_is_program_name(state->camera_name);\n',
                  label='Program audio maps to master')
save(p, s)

# Program recorder lifecycle follows the plugin/master-audio/session lifecycle.
p = 'src/plugin-main.c'
s = load(p)
s = replace_exact(s, '#include "sr-master-audio.h"\n',
                  '#include "sr-master-audio.h"\n#include "sr-program-recorder.h"\n',
                  label='plugin Program include')
s = replace_exact(s, '\tif (!sr_storage_cleanup_init()) {\n',
                  '\tif (!sr_program_recorder_init()) {\n'
                  '\t\tsr_master_audio_free();\n\t\tsr_session_free();\n\t\tsr_config_free();\n'
                  '\t\tobs_log(LOG_ERROR, "Pitel Instant Replay: could not initialize PROGRAM replay recorder");\n'
                  '\t\treturn false;\n\t}\n'
                  '\tif (!sr_storage_cleanup_init()) {\n',
                  label='Program init')
s = replace_exact(s,
                  '\tif (!sr_storage_cleanup_init()) {\n\t\tsr_master_audio_free();',
                  '\tif (!sr_storage_cleanup_init()) {\n\t\tsr_program_recorder_free();\n\t\tsr_master_audio_free();',
                  label='Program cleanup storage init failure')
s = replace_exact(s,
                  '\t\tsr_storage_cleanup_free();\n\t\tsr_master_audio_free();',
                  '\t\tsr_storage_cleanup_free();\n\t\tsr_program_recorder_free();\n\t\tsr_master_audio_free();',
                  count=1, label='Program cleanup controller failure')
s = replace_exact(s,
                  '\tsr_storage_cleanup_free();\n\tsr_master_audio_free();\n\tsr_session_free();',
                  '\tsr_storage_cleanup_free();\n\tsr_program_recorder_free();\n\tsr_master_audio_free();\n\tsr_session_free();',
                  count=1, label='Program unload')
save(p, s)

print('Program playback/lifecycle patch OK')
