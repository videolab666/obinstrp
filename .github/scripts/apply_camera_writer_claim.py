from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


path = Path("src/sr-segment-writer.c")
replace_once(
    path,
    '''\tchar *camera_name;\n\tchar *camera_key;\n\tchar *camera_dir;\n\tuint32_t camera_hash;\n''',
    '''\tchar *camera_name;\n\tchar *camera_key;\n\tchar *camera_dir;\n\tuint32_t camera_hash;\n\tbool camera_claimed;\n''',
)
replace_once(
    path,
    '''\tstruct sr_segment_writer_stats stats;\n};\n\nstatic void free_packet_node(struct sr_writer_packet *node)\n''',
    '''\tstruct sr_segment_writer_stats stats;\n};\n\nstruct sr_camera_writer_claim {\n\tchar *key;\n\tstruct sr_camera_writer_claim *next;\n};\n\nstatic pthread_mutex_t g_camera_claim_mutex = PTHREAD_MUTEX_INITIALIZER;\nstatic struct sr_camera_writer_claim *g_camera_claims;\n\nstatic bool claim_camera_writer(const char *key)\n{\n\tif (!key || !*key)\n\t\treturn false;\n\n\tpthread_mutex_lock(&g_camera_claim_mutex);\n\tfor (struct sr_camera_writer_claim *claim = g_camera_claims; claim; claim = claim->next) {\n\t\tif (strcmp(claim->key, key) == 0) {\n\t\t\tpthread_mutex_unlock(&g_camera_claim_mutex);\n\t\t\treturn false;\n\t\t}\n\t}\n\n\tstruct sr_camera_writer_claim *claim = bzalloc(sizeof(*claim));\n\tclaim->key = bstrdup(key);\n\tif (!claim->key) {\n\t\tbfree(claim);\n\t\tpthread_mutex_unlock(&g_camera_claim_mutex);\n\t\treturn false;\n\t}\n\tclaim->next = g_camera_claims;\n\tg_camera_claims = claim;\n\tpthread_mutex_unlock(&g_camera_claim_mutex);\n\treturn true;\n}\n\nstatic void release_camera_writer(const char *key)\n{\n\tif (!key || !*key)\n\t\treturn;\n\n\tpthread_mutex_lock(&g_camera_claim_mutex);\n\tstruct sr_camera_writer_claim **link = &g_camera_claims;\n\twhile (*link) {\n\t\tstruct sr_camera_writer_claim *claim = *link;\n\t\tif (strcmp(claim->key, key) == 0) {\n\t\t\t*link = claim->next;\n\t\t\tbfree(claim->key);\n\t\t\tbfree(claim);\n\t\t\tbreak;\n\t\t}\n\t\tlink = &claim->next;\n\t}\n\tpthread_mutex_unlock(&g_camera_claim_mutex);\n}\n\nstatic void free_packet_node(struct sr_writer_packet *node)\n''',
)
replace_once(
    path,
    '''\tw->camera_name = bstrdup(config->camera_name);\n\tw->camera_key = bstrdup(config->camera_key);\n\tw->camera_hash = sr_camera_key_hash(config->camera_key);\n''',
    '''\tw->camera_name = bstrdup(config->camera_name);\n\tw->camera_key = bstrdup(config->camera_key);\n\tw->camera_hash = sr_camera_key_hash(config->camera_key);\n\tif (!w->camera_name || !w->camera_key || !claim_camera_writer(config->camera_key)) {\n\t\tblog(LOG_ERROR,\n\t\t     "Sports Replay: refusing a second continuous disk writer for camera '%s' (UUID %s)",\n\t\t     config->camera_name, config->camera_key);\n\t\tsr_segment_writer_destroy(w);\n\t\treturn NULL;\n\t}\n\tw->camera_claimed = true;\n''',
)
replace_once(
    path,
    '''\tbfree(w->extradata);\n\tbfree(w->camera_name);\n\tbfree(w->camera_key);\n''',
    '''\tbfree(w->extradata);\n\tif (w->camera_claimed)\n\t\trelease_camera_writer(w->camera_key);\n\tbfree(w->camera_name);\n\tbfree(w->camera_key);\n''',
)

print("camera writer claim guard applied")
