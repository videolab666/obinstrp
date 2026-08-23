from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


output = Path("src/sr-event-output.c")
replace_once(
    output,
    '#include "sr-replay-channel.h"\n#include "sr-session.h"\n',
    '#include "sr-replay-channel.h"\n#include "sr-scene-tracker.h"\n#include "sr-session.h"\n',
)
replace_once(
    output,
    "static obs_properties_t *sr_event_output_properties(void *unused)\n",
    "static void sr_event_output_deactivate(void *data)\n{\n\tstruct sr_event_output *output = data;\n\tif (!output)\n\t\treturn;\n\n\t/* Keep the bus alive through an OUT transition so OBS can crossfade its\n\t * replay audio/video naturally. Deactivation happens when the transition\n\t * has finished and is also the safety net for an operator cutting away\n\t * manually instead of pressing RETURN LIVE. */\n\treset_audio_transport(output);\n\tsr_replay_channel_stop(output->bus);\n\tsr_scene_tracker_end_replay_guard();\n}\n\nstatic obs_properties_t *sr_event_output_properties(void *unused)\n",
)
replace_once(
    output,
    "\t.update = sr_event_output_update,\n\t.get_defaults = sr_event_output_defaults,\n",
    "\t.update = sr_event_output_update,\n\t.deactivate = sr_event_output_deactivate,\n\t.get_defaults = sr_event_output_defaults,\n",
)

take = Path("src/sr-replay-take.c")
replace_once(
    take,
    "\tsr_replay_channel_stop(SR_REPLAY_BUS_A);\n\tsr_replay_channel_stop(SR_REPLAY_BUS_B);\n\tsr_scene_tracker_end_replay_guard();\n",
    "\t/* Do not stop the replay bus before the OUT stinger: the native OBS\n\t * transition must still be able to mix the replay picture/audio. The Event\n\t * Output deactivation stops the bus once it has actually left program. */\n\tsr_scene_tracker_end_replay_guard();\n",
)

tracker = Path("src/sr-scene-tracker.c")
replace_once(
    tracker,
    "void sr_scene_tracker_note_replay_launch(void)\n{\n\tpthread_mutex_lock(&g_mutex);\n",
    "void sr_scene_tracker_note_replay_launch(void)\n{\n\tif (!g_started)\n\t\treturn;\n\tpthread_mutex_lock(&g_mutex);\n",
)
replace_once(
    tracker,
    "void sr_scene_tracker_end_replay_guard(void)\n{\n\tpthread_mutex_lock(&g_mutex);\n",
    "void sr_scene_tracker_end_replay_guard(void)\n{\n\tif (!g_started)\n\t\treturn;\n\tpthread_mutex_lock(&g_mutex);\n",
)
