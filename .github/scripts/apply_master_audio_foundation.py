from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


capture = Path("src/capture-filter.c")
replace_once(
    capture,
    '#include "sr-config.h"\n#include "sr-session.h"\n',
    '#include "sr-config.h"\n#include "sr-master-audio.h"\n#include "sr-session.h"\n',
)
replace_once(
    capture,
    '\tbool disk_recording;\n\tbool writer_failed;\n',
    '\tbool disk_recording;\n\tbool writer_failed;\n\tbool master_audio_acquired;\n',
)
replace_once(
    capture,
    '''static void destroy_writer(struct sr_capture *c)\n{\n\tif (!c->writer)\n\t\treturn;\n\tsr_segment_writer_destroy(c->writer);\n\tc->writer = NULL;\n}\n''',
    '''static void destroy_writer(struct sr_capture *c)\n{\n\tif (c->writer) {\n\t\tsr_segment_writer_destroy(c->writer);\n\t\tc->writer = NULL;\n\t}\n\tif (c->master_audio_acquired) {\n\t\tsr_master_audio_release();\n\t\tc->master_audio_acquired = false;\n\t}\n}\n''',
)
replace_once(
    capture,
    '''\tc->writer = sr_segment_writer_create(&cfg);\n\tbfree(session_dir);\n\tif (!c->writer) {\n\t\tc->writer_failed = true;\n\t\tobs_log(LOG_ERROR, "'%s': could not start continuous replay recorder", obs_source_get_name(c->self));\n\t\treturn false;\n\t}\n\treturn true;\n}\n''',
    '''\tc->writer = sr_segment_writer_create(&cfg);\n\tbfree(session_dir);\n\tif (!c->writer) {\n\t\tc->writer_failed = true;\n\t\tobs_log(LOG_ERROR, "'%s': could not start continuous replay recorder", obs_source_get_name(c->self));\n\t\treturn false;\n\t}\n\n\tif (!c->master_audio_acquired) {\n\t\tif (sr_master_audio_acquire())\n\t\t\tc->master_audio_acquired = true;\n\t\telse\n\t\t\tobs_log(LOG_WARNING, "'%s': continuous video is recording, but master replay audio could not start",\n\t\t\t\tobs_source_get_name(c->self));\n\t}\n\treturn true;\n}\n''',
)

plugin = Path("src/plugin-main.c")
replace_once(
    plugin,
    '#include "sr-event-dock.h"\n#include "sr-replay-channel.h"\n',
    '#include "sr-event-dock.h"\n#include "sr-master-audio.h"\n#include "sr-replay-channel.h"\n',
)
replace_once(
    plugin,
    '''\tsr_config_init();\n\tsr_session_init();\n\tif (!sr_storage_cleanup_init()) {\n\t\tsr_session_free();\n\t\tsr_config_free();\n\t\tobs_log(LOG_ERROR, "Sports Replay: could not initialize storage synchronization");\n\t\treturn false;\n\t}\n''',
    '''\tsr_config_init();\n\tsr_session_init();\n\tif (!sr_master_audio_init()) {\n\t\tsr_session_free();\n\t\tsr_config_free();\n\t\tobs_log(LOG_ERROR, "Sports Replay: could not initialize master replay audio capture");\n\t\treturn false;\n\t}\n\tif (!sr_storage_cleanup_init()) {\n\t\tsr_master_audio_free();\n\t\tsr_session_free();\n\t\tsr_config_free();\n\t\tobs_log(LOG_ERROR, "Sports Replay: could not initialize storage synchronization");\n\t\treturn false;\n\t}\n''',
)
replace_once(
    plugin,
    '''\t\tsr_event_controller_destroy(event_controller);\n\t\tevent_controller = NULL;\n\t\tsr_storage_cleanup_free();\n\t\tsr_session_free();\n''',
    '''\t\tsr_event_controller_destroy(event_controller);\n\t\tevent_controller = NULL;\n\t\tsr_storage_cleanup_free();\n\t\tsr_master_audio_free();\n\t\tsr_session_free();\n''',
)
replace_once(
    plugin,
    '''\tsr_event_controller_destroy(event_controller);\n\tevent_controller = NULL;\n\tsr_storage_cleanup_free();\n\tsr_session_free();\n''',
    '''\tsr_event_controller_destroy(event_controller);\n\tevent_controller = NULL;\n\tsr_storage_cleanup_free();\n\tsr_master_audio_free();\n\tsr_session_free();\n''',
)

cmake = Path("CMakeLists.txt")
replace_once(
    cmake,
    '    src/sr-config.c\n    src/sr-session.c\n',
    '    src/sr-config.c\n    src/sr-master-audio.c\n    src/sr-session.c\n',
)
