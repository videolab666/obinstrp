from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def write(rel, text):
    (ROOT / rel).write_text(text, encoding='utf-8', newline='\n')


def once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one match, found {count}')
    return text.replace(old, new, 1)


# Mapping outside an active Run must never leak native OBS uptime into persisted
# media. Late/stale producers are generation-guarded and receive zero here.
rel = 'src/sr-session.c'
text = read(rel)
text = once(text,
            'uint64_t result = obs_timestamp_ns;\n\tif (g_recording_path) {',
            'uint64_t result = 0;\n\tif (g_recording_path) {',
            'inactive mapping must not return raw clock')
write(rel, text)

# Master Audio has the same generation ownership at callback time as camera
# audio and video writers. This is defense in depth on top of STOP drain.
rel = 'src/sr-master-audio.c'
text = read(rel)
text = once(text,
            '\tif (!state || !data || !data->frames || !data->data[0] || !data->data[1])\n\t\treturn;\n\n'
            '\tenqueue_audio(state, data->data[0], data->data[1], data->frames, data->timestamp);',
            '\tif (!state || !data || !data->frames || !data->data[0] || !data->data[1])\n\t\treturn;\n'
            '\tif (!sr_session_recording_is_active() ||\n'
            '\t    state->recording_generation != sr_session_recording_generation())\n'
            '\t\treturn;\n\n'
            '\tenqueue_audio(state, data->data[0], data->data[1], data->frames, data->timestamp);',
            'master audio callback generation guard')
write(rel, text)

# The public control function accepts a nullable camera_count. Always keep a
# local count so an empty START can be rolled back instead of leaving a phantom
# Recording Run active.
rel = 'src/sr-capture-session.c'
text = read(rel)
old = '''bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)\n{\n\tif (enabled) {\n'''
new = '''bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count)\n{\n\tsize_t local_count = 0;\n\tsize_t *count = camera_count ? camera_count : &local_count;\n\tif (enabled) {\n'''
text = once(text, old, new, 'capture local camera count')
text = once(text,
            '\t\tconst bool ok = sr_capture_set_all_disk_recording_impl(true, camera_count);\n'
            '\t\tif (!ok || (camera_count && *camera_count == 0)) {',
            '\t\tconst bool ok = sr_capture_set_all_disk_recording_impl(true, count);\n'
            '\t\tif (!ok || *count == 0) {',
            'capture start count')
text = once(text,
            '\tconst bool ok = sr_capture_set_all_disk_recording_impl(false, camera_count);',
            '\tconst bool ok = sr_capture_set_all_disk_recording_impl(false, count);',
            'capture stop count')
write(rel, text)

# If Resume selects a target but opening its Event DB fails, clear the target.
# Otherwise a later START could appear mysteriously blocked by a hidden stale
# target selected by a failed UI operation.
rel = 'src/sr-session-panel.cpp'
text = read(rel)
text = once(text,
            '\t\tif (!openPath(path))\n\t\t\tQMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));\n\t\trefresh();',
            '\t\tif (!openPath(path)) {\n'
            '\t\t\tsr_session_clear_record_target();\n'
            '\t\t\tQMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));\n'
            '\t\t}\n\t\trefresh();',
            'resume open failure clears target')
write(rel, text)

print('Final Session timebase guards applied successfully')
