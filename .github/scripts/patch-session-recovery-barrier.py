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


rel = 'src/sr-storage-manager.h'
text = read(rel)
text = once(text, 'bool sr_storage_manager_start(void);\nvoid sr_storage_manager_stop(void);\n',
            'bool sr_storage_manager_start(void);\nvoid sr_storage_manager_stop(void);\n'
            'bool sr_storage_manager_wait_initial_recovery(uint32_t timeout_ms);\n',
            'storage recovery wait declaration')
write(rel, text)

rel = 'src/sr-storage-manager.c'
text = read(rel)
text = once(text, 'static bool g_manager_thread_started;\nstatic bool g_manager_stopping;\n',
            'static bool g_manager_thread_started;\nstatic bool g_manager_stopping;\n'
            'static bool g_initial_recovery_complete;\n',
            'storage recovery state')
text = once(text, '\trun_recovery_once();\n\n\twhile (!manager_should_stop()) {',
            '\trun_recovery_once();\n'
            '\tpthread_mutex_lock(&g_manager_mutex);\n'
            '\tg_initial_recovery_complete = true;\n'
            '\tpthread_mutex_unlock(&g_manager_mutex);\n\n'
            '\twhile (!manager_should_stop()) {',
            'publish recovery completion')
text = once(text, '\tg_manager_stopping = false;\n\tmemset(&g_manager_status, 0, sizeof(g_manager_status));',
            '\tg_manager_stopping = false;\n\tg_initial_recovery_complete = false;\n'
            '\tmemset(&g_manager_status, 0, sizeof(g_manager_status));',
            'reset recovery completion')
anchor = '\nvoid sr_storage_manager_get_status(struct sr_storage_manager_status *status)\n'
wait_fn = '''\nbool sr_storage_manager_wait_initial_recovery(uint32_t timeout_ms)\n{\n\tif (!g_manager_mutex_initialized || !g_manager_thread_started)\n\t\treturn false;\n\tconst uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;\n\tfor (;;) {\n\t\tpthread_mutex_lock(&g_manager_mutex);\n\t\tconst bool complete = g_initial_recovery_complete;\n\t\tconst bool stopping = g_manager_stopping;\n\t\tpthread_mutex_unlock(&g_manager_mutex);\n\t\tif (complete)\n\t\t\treturn true;\n\t\tif (stopping || os_gettime_ns() >= deadline)\n\t\t\treturn false;\n\t\tos_sleep_ms(5);\n\t}\n}\n'''
if anchor not in text:
    raise RuntimeError('storage manager wait anchor missing')
text = text.replace(anchor, wait_fn + anchor, 1)
write(rel, text)

rel = 'src/sr-capture-session.c'
text = read(rel)
text = once(text, '#include "sr-master-audio.h"\n#include "sr-session.h"',
            '#include "sr-master-audio.h"\n#include "sr-session.h"\n#include "sr-storage-manager.h"',
            'capture storage manager include')
text = once(text,
            '\tif (enabled) {\n\t\tif (!sr_session_prepare_recording(obs_get_video_frame_time()))',
            '\tif (enabled) {\n'
            '\t\tif (!sr_storage_manager_wait_initial_recovery(10000)) {\n'
            '\t\t\tblog(LOG_WARNING,\n'
            '\t\t\t     "Pitel Instant Replay: START REC deferred because crash recovery is still validating previous sessions");\n'
            '\t\t\treturn false;\n'
            '\t\t}\n'
            '\t\tif (!sr_session_prepare_recording(obs_get_video_frame_time()))',
            'capture recovery barrier')
write(rel, text)

print('Session recovery barrier applied successfully')
