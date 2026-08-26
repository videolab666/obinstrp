from pathlib import Path
from textwrap import dedent

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

# Merge PROGRAM into the same global REC/health/performance model as camera recorders.
p = 'src/capture-filter.c'
s = load(p)
s = replace_exact(s, '#include "sr-master-audio.h"\n',
                  '#include "sr-master-audio.h"\n#include "sr-program-recorder.h"\n',
                  label='capture Program include')
s = replace_exact(s,
                  '\tobs_enum_sources(capture_control_source, &ctx);\n\tif (camera_count)\n\t\t*camera_count = ctx.camera_count;\n\treturn true;\n}\n\nbool sr_capture_get_recording_summary',
                  '\tobs_enum_sources(capture_control_source, &ctx);\n'
                  '\tif (sr_program_recorder_selected()) {\n'
                  '\t\tctx.camera_count++;\n'
                  '\t\tsr_program_recorder_set_recording(enabled);\n'
                  '\t}\n'
                  '\tif (camera_count)\n\t\t*camera_count = ctx.camera_count;\n\treturn true;\n}\n\nbool sr_capture_get_recording_summary',
                  label='global REC Program')
s = replace_exact(s,
                  '\tstruct capture_control_context ctx = {.summary = summary};\n\tobs_enum_sources(capture_control_source, &ctx);\n\treturn true;\n',
                  '\tstruct capture_control_context ctx = {.summary = summary};\n\tobs_enum_sources(capture_control_source, &ctx);\n'
                  '\tsr_program_recorder_add_recording_summary(summary);\n\treturn true;\n',
                  label='Program summary')
s = replace_exact(s,
                  '\tstruct capture_control_context ctx = {.performance = snapshot};\n\tobs_enum_sources(capture_control_source, &ctx);\n\treturn true;\n',
                  '\tstruct capture_control_context ctx = {.performance = snapshot};\n\tobs_enum_sources(capture_control_source, &ctx);\n'
                  '\tstruct sr_capture_performance_entry program = {0};\n'
                  '\tif (sr_program_recorder_get_performance_entry(&program)) {\n'
                  '\t\tconst size_t next_count = snapshot->count + 1;\n'
                  '\t\tstruct sr_capture_performance_entry *entries = brealloc(snapshot->entries, next_count * sizeof(*entries));\n'
                  '\t\tif (entries) {\n'
                  '\t\t\tsnapshot->entries = entries;\n'
                  '\t\t\tsnapshot->entries[snapshot->count] = program;\n'
                  '\t\t\tsnapshot->count = next_count;\n'
                  '\t\t}\n'
                  '\t}\n\treturn true;\n',
                  label='Program performance')
save(p, s)

# PROGRAM appears as a replay angle whenever it is selected in Replay Setup.
p = 'src/sr-camera-list.c'
s = load(p)
s = replace_exact(s, '#include "sr-capture.h"\n',
                  '#include "sr-capture.h"\n#include "sr-program-recorder.h"\n',
                  label='camera list Program include')
marker = '\tif (builder.count > 1)\n\t\tqsort(builder.items, builder.count, sizeof(*builder.items), compare_entries);\n'
addition = dedent(r'''
	if (sr_program_recorder_selected() && !contains_key(&builder, SR_PROGRAM_CAMERA_KEY)) {
		if (builder.count == builder.capacity) {
			const size_t next_capacity = builder.capacity ? builder.capacity * 2 : 8;
			struct camera_entry *next = brealloc(builder.items, next_capacity * sizeof(*next));
			if (!next) {
				free_builder(&builder);
				return false;
			}
			builder.items = next;
			builder.capacity = next_capacity;
		}
		struct camera_entry *entry = &builder.items[builder.count];
		memset(entry, 0, sizeof(*entry));
		entry->name = bstrdup(SR_PROGRAM_CAMERA_NAME);
		if (!entry->name) {
			free_builder(&builder);
			return false;
		}
		memcpy(entry->key, SR_PROGRAM_CAMERA_KEY, sizeof(SR_PROGRAM_CAMERA_KEY));
		builder.count++;
	}

''')
s = insert_before(s, marker, addition, label='append Program angle')
save(p, s)

# Replay Setup knows whether PROGRAM is supported/selected and can persist the choice.
p = 'src/sr-replay-setup.h'
s = load(p)
s = replace_exact(s,
                  '\tbool event_transition_ready;\n\tchar scene_a[SR_REPLAY_SETUP_NAME_MAX];',
                  '\tbool event_transition_ready;\n\tbool program_output_supported;\n\tbool program_output_enabled;\n'
                  '\tchar scene_a[SR_REPLAY_SETUP_NAME_MAX];',
                  label='setup Program state')
s = insert_before(s, '/* Create/repair two scene-backed Event Outputs for A/B playback.',
                  '/* Select/deselect final OBS Program/PGM as a persistent replay pseudo-angle. */\n'
                  'bool sr_replay_setup_set_program_output(bool enabled);\n\n',
                  label='setup Program API')
save(p, s)

p = 'src/sr-replay-setup.c'
s = load(p)
s = replace_exact(s, '#include "sr-event-output.h"\n',
                  '#include "sr-event-output.h"\n#include "sr-program-recorder.h"\n',
                  label='setup Program include')
s = replace_exact(s,
                  '\tobs_enum_sources(collect_setup_source, &ctx);\n\n\tchar *scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);',
                  '\tobs_enum_sources(collect_setup_source, &ctx);\n'
                  '\tsnapshot->program_output_supported = sr_program_recorder_supported();\n'
                  '\tsnapshot->program_output_enabled = sr_program_recorder_selected();\n\n'
                  '\tchar *scene_a = sr_replay_setup_find_output_scene_name(SR_REPLAY_BUS_A);',
                  label='setup snapshot Program')
setter = dedent(r'''
bool sr_replay_setup_set_program_output(bool enabled)
{
	if (enabled && !sr_program_recorder_supported())
		return false;
	sr_program_recorder_set_selected(enabled);
	obs_frontend_save();
	return sr_program_recorder_selected() == enabled;
}

''')
s = insert_before(s, 'static obs_source_t *get_or_create_scene_source', setter, label='setup Program setter')
save(p, s)

print('Program integration patch OK')
