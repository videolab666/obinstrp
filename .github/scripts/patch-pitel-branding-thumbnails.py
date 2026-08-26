from pathlib import Path
import json


def rep(path, old, new, count=-1):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"missing anchor in {path}: {old[:60]!r}")
    p.write_text(text.replace(old, new, count), encoding="utf-8")


# D3D11VA replay frames are hardware AVFrames. Download only the requested
# still before swscale; normal replay presentation remains GPU-resident.
rep(
    "src/sr-thumb.c",
    "#include <libavformat/avformat.h>\n#include <libavcodec/avcodec.h>\n#include <libswscale/swscale.h>",
    "#include <libavformat/avformat.h>\n#include <libavcodec/avcodec.h>\n#include <libavutil/hwcontext.h>\n#include <libswscale/swscale.h>",
)
rep(
    "src/sr-thumb.c",
    """\tbool ok = false;\n\tif (have_frame && frame) {\n\t\tstruct SwsContext *sws = sws_getContext(frame->width, frame->height, frame->format, w, h,\n\t\t\t\t\t\t\tAV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);\n\t\tif (sws) {\n\t\t\tuint8_t *buf = bzalloc((size_t)w * h * 4);\n\t\t\tuint8_t *dst[4] = {buf, NULL, NULL, NULL};\n\t\t\tint dst_linesize[4] = {w * 4, 0, 0, 0};\n\t\t\tsws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst,\n\t\t\t\t  dst_linesize);\n\t\t\tsws_freeContext(sws);\n\t\t\t*out = buf;\n\t\t\tok = true;\n\t\t}\n\t}\n\n\tav_frame_free(&frame);\n\tsr_disk_player_destroy(player);\n\treturn ok;\n""",
    """\t/* Disk replay normally decodes through D3D11VA on Windows. swscale cannot\n\t * read an AV_PIX_FMT_D3D11 surface directly, so download this one UI still\n\t * to a software frame. Normal replay remains GPU-resident. */\n\tAVFrame *software_frame = frame;\n\tAVFrame *transferred = NULL;\n\tif (have_frame && frame && frame->hw_frames_ctx) {\n\t\ttransferred = av_frame_alloc();\n\t\tif (!transferred || av_hwframe_transfer_data(transferred, frame, 0) < 0) {\n\t\t\tav_frame_free(&transferred);\n\t\t\thave_frame = false;\n\t\t} else {\n\t\t\tav_frame_copy_props(transferred, frame);\n\t\t\tsoftware_frame = transferred;\n\t\t}\n\t}\n\n\tbool ok = false;\n\tif (have_frame && software_frame) {\n\t\tstruct SwsContext *sws =\n\t\t\tsws_getContext(software_frame->width, software_frame->height, software_frame->format, w, h,\n\t\t\t\t       AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);\n\t\tif (sws) {\n\t\t\tuint8_t *buf = bzalloc((size_t)w * h * 4);\n\t\t\tuint8_t *dst[4] = {buf, NULL, NULL, NULL};\n\t\t\tint dst_linesize[4] = {w * 4, 0, 0, 0};\n\t\t\tsws_scale(sws, (const uint8_t *const *)software_frame->data, software_frame->linesize, 0,\n\t\t\t\t  software_frame->height, dst, dst_linesize);\n\t\t\tsws_freeContext(sws);\n\t\t\t*out = buf;\n\t\t\tok = true;\n\t\t}\n\t}\n\n\tav_frame_free(&transferred);\n\tav_frame_free(&frame);\n\tsr_disk_player_destroy(player);\n\treturn ok;\n""",
)

# Public module/package name.
p = Path("buildspec.json")
data = json.loads(p.read_text(encoding="utf-8"))
data["name"] = "pitel-instant-replay"
data["artifactName"] = "pitel-instant-replay"
data["displayName"] = "Pitel Instant Replay"
data["platformConfig"]["macos"]["bundleId"] = "com.systec.pitel-instant-replay"
p.write_text(json.dumps(data, indent=4) + "\n", encoding="utf-8")
rep("CMakeLists.txt", "__sports_replay_no_cmake__", "__pitel_instant_replay_no_cmake__")

# Non-persistent names can be fully rebranded.
rep("src/sr-segment-writer.c", "sports-replay-writer", "pitel-replay-writer")
rep("src/sr-master-audio.c", "sports-replay-audio", "pitel-replay-audio")
rep("src/sr-storage-manager.c", "sports-replay-storage", "pitel-replay-storage")
rep("src/sr-event-output.c", "SportsReplayEventOutput", "PitelInstantReplayEventOutput")
rep("src/capture-filter.c", "SportsReplayCapture", "PitelInstantReplayCapture")
rep("src/playback-source.c", 'obs_module_text("SportsReplay")', 'obs_module_text("PitelInstantReplay")')
rep("src/sr-credit.h", 'obs_module_text("SportsReplay")', 'obs_module_text("PitelInstantReplay")')
rep("data/locale/en-US.ini", "SportsReplayEventOutput=", "PitelInstantReplayEventOutput=")
rep("data/locale/en-US.ini", "SportsReplayCapture=", "PitelInstantReplayCapture=")
rep("data/locale/en-US.ini", "SportsReplay=", "PitelInstantReplay=")
rep("data/locale/es-ES.ini", "SportsReplayCapture=", "PitelInstantReplayCapture=")
rep(
    "data/locale/es-ES.ini",
    'SportsReplay="Pitel Instant Replay"',
    'PitelInstantReplay="Pitel Instant Replay"\nPitelInstantReplayEventOutput="Salida de evento de Pitel Instant Replay"',
)

# Module filename rename changes OBS module-config directory. Import legacy config
# once so storage/transition settings and played-MP4 history survive the upgrade.
rep(
    "src/sr-config.c",
    """\tchar *path = obs_module_config_path("config.json");\n\tobs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;\n\tbfree(path);\n\n\tconst char *saved = data ? obs_data_get_string(data, "save_dir") : "";\n""",
    """\tchar *path = obs_module_config_path("config.json");\n\tobs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;\n\tbfree(path);\n\n\tbool migrated_legacy_config = false;\n\tif (!data) {\n\t\tchar *legacy_path = obs_module_config_path("../sports-replay/config.json");\n\t\tdata = legacy_path ? obs_data_create_from_json_file(legacy_path) : NULL;\n\t\tif (data) {\n\t\t\tmigrated_legacy_config = true;\n\t\t\tblog(LOG_INFO, "Pitel Instant Replay: importing legacy sports-replay module configuration");\n\t\t}\n\t\tbfree(legacy_path);\n\t}\n\n\tconst char *saved = data ? obs_data_get_string(data, "save_dir") : "";\n""",
)
rep(
    "src/sr-config.c",
    """\tos_mkdirs(g_save_dir);\n\tos_mkdirs(g_session_root);\n\n\tif (data)\n\t\tobs_data_release(data);\n}\n""",
    """\tos_mkdirs(g_save_dir);\n\tos_mkdirs(g_session_root);\n\n\tif (migrated_legacy_config)\n\t\tsave_locked();\n\n\tif (data)\n\t\tobs_data_release(data);\n}\n""",
)
rep(
    "src/sr-dock.cpp",
    """\t\tQByteArray cfgUtf8 = cfgPath.toUtf8();\n\t\tobs_data_t *data = obs_data_create_from_json_file(cfgUtf8.constData());\n\t\tif (!data)\n\t\t\treturn;\n\n\t\tobs_data_array_t *arr = obs_data_get_array(data, "played");\n""",
    """\t\tQByteArray cfgUtf8 = cfgPath.toUtf8();\n\t\tobs_data_t *data = obs_data_create_from_json_file(cfgUtf8.constData());\n\t\tbool migratedLegacy = false;\n\t\tif (!data) {\n\t\t\tchar *legacyPath = obs_module_config_path("../sports-replay/played.json");\n\t\t\tif (legacyPath) {\n\t\t\t\tdata = obs_data_create_from_json_file(legacyPath);\n\t\t\t\tmigratedLegacy = data != nullptr;\n\t\t\t}\n\t\t\tbfree(legacyPath);\n\t\t}\n\t\tif (!data)\n\t\t\treturn;\n\n\t\tobs_data_array_t *arr = obs_data_get_array(data, "played");\n""",
)
rep(
    "src/sr-dock.cpp",
    """\t\tobs_data_release(data);\n\t}\n\n\tvoid savePlayedPaths()\n""",
    """\t\tobs_data_release(data);\n\t\tif (migratedLegacy)\n\t\t\tsavePlayedPaths();\n\t}\n\n\tvoid savePlayedPaths()\n""",
    1,
)

# Installer uses new files and removes the legacy DLL/data before upgrade.
rep(
    "installer/pitel-instant-replay.iss",
    'Source: "sports-replay\\bin\\64bit\\sports-replay.dll"; DestDir: "{app}\\obs-plugins\\64bit"; Flags: ignoreversion',
    'Source: "pitel-instant-replay\\bin\\64bit\\pitel-instant-replay.dll"; DestDir: "{app}\\obs-plugins\\64bit"; Flags: ignoreversion',
)
rep(
    "installer/pitel-instant-replay.iss",
    'Source: "sports-replay\\data\\locale\\*"; DestDir: "{app}\\data\\obs-plugins\\sports-replay\\locale"; Flags: ignoreversion recursesubdirs createallsubdirs',
    'Source: "pitel-instant-replay\\data\\locale\\*"; DestDir: "{app}\\data\\obs-plugins\\pitel-instant-replay\\locale"; Flags: ignoreversion recursesubdirs createallsubdirs',
)
rep(
    "installer/pitel-instant-replay.iss",
    '[UninstallDelete]\nType: filesandordirs; Name: "{app}\\data\\obs-plugins\\sports-replay"',
    '[InstallDelete]\n; Remove the legacy module before installing the renamed module.\nType: files; Name: "{app}\\obs-plugins\\64bit\\sports-replay.dll"\nType: files; Name: "{app}\\obs-plugins\\64bit\\sports-replay.pdb"\nType: filesandordirs; Name: "{app}\\data\\obs-plugins\\sports-replay"\n\n[UninstallDelete]\nType: filesandordirs; Name: "{app}\\data\\obs-plugins\\pitel-instant-replay"',
)

Path("docs/BRANDING_COMPATIBILITY.md").write_text(
    """# Pitel Instant Replay branding and compatibility\n\nNew packages use `pitel-instant-replay` consistently, including\n`pitel-instant-replay.dll`, the plugin data directory, package/bundle names and\nlogs.\n\nThe following hidden OBS persistence identifiers deliberately keep their legacy\nvalues so existing scene collections and hotkeys continue to work:\n\n- `sports_replay`, `sports_replay_capture`, `sports_replay_event_output`;\n- `SportsReplay.*` hotkey IDs;\n- existing dock IDs/object names used by saved OBS layouts.\n\nThese are compatibility keys, not user-facing product names. The Windows\ninstaller removes the old `sports-replay.dll`/PDB/data directory. Manual ZIP\nupgrades must remove the old DLL once before copying the new plugin so OBS cannot\nload both modules. Runtime config and played-history loading migrate the old\n`sports-replay` module config directory into the new one on first launch.\n\nReferences to `Voodoo25/obs-sports-replay` remain because that is the actual\nupstream repository name.\n""",
    encoding="utf-8",
)
