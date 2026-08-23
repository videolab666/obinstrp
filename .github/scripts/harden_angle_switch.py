from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"expected block not found in {path}: {old[:160]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


main = Path("src/plugin-main.c")
replace_once(
    main,
    "static void register_event_hotkeys(void)\n{\n\thk_event_in =",
    "static void register_event_hotkeys(void)\n{\n\tfor (size_t i = 0; i < SR_ANGLE_HOTKEY_COUNT; i++)\n\t\thk_angles[i] = OBS_INVALID_HOTKEY_ID;\n\n\thk_event_in =",
)

channel = Path("src/sr-replay-channel.c")
old = r'''	pthread_mutex_lock(&channel->mutex);
	if (channel->cued && channel->event_id == expected_event_id && channel->event_in_ns == event_in_ns &&
	    channel->event_out_ns == event_out_ns && channel->playhead_ns >= new_in_ns &&
	    channel->playhead_ns <= new_out_ns) {
		switch_playhead_ns = channel->playhead_ns;
		old_player = channel->player;
		old_camera_name = channel->camera_name;
		channel->player = new_player;
		channel->camera_name = new_camera_name;
		channel->in_ns = new_in_ns;
		channel->out_ns = new_out_ns;
		channel->width = 0;
		channel->height = 0;
		channel->partial_coverage = new_in_ns != event_in_ns || new_out_ns != event_out_ns;
		partial = channel->partial_coverage;
		channel->last_clock_ns = 0;
		channel->need_frame = true;
		switched = true;
	}
	pthread_mutex_unlock(&channel->mutex);
'''
new = r'''	pthread_mutex_lock(&channel->mutex);
	if (channel->cued && channel->event_id == expected_event_id && channel->event_in_ns == event_in_ns &&
	    channel->event_out_ns == event_out_ns && channel->playhead_ns >= new_in_ns &&
	    channel->playhead_ns <= new_out_ns) {
		/* The transport may have advanced while the candidate player was being
		 * opened and warmed. Validate the exact commit-time playhead while the
		 * channel mutex is held; this briefly stalls that bus, but guarantees an
		 * on-air angle swap never commits a camera with an internal media gap at
		 * the frame that will actually be rendered next. The first probe above
		 * normally makes this second decode a cache hit or a tiny forward step. */
		AVFrame *commit_probe = NULL;
		const bool commit_ready =
			sr_disk_player_decode_at(new_player, channel->playhead_ns, &commit_probe, NULL) && commit_probe;
		av_frame_free(&commit_probe);
		if (commit_ready) {
			switch_playhead_ns = channel->playhead_ns;
			old_player = channel->player;
			old_camera_name = channel->camera_name;
			channel->player = new_player;
			channel->camera_name = new_camera_name;
			channel->in_ns = new_in_ns;
			channel->out_ns = new_out_ns;
			channel->width = 0;
			channel->height = 0;
			channel->partial_coverage = new_in_ns != event_in_ns || new_out_ns != event_out_ns;
			partial = channel->partial_coverage;
			channel->last_clock_ns = 0;
			channel->need_frame = true;
			switched = true;
		}
	}
	pthread_mutex_unlock(&channel->mutex);
'''
replace_once(channel, old, new)

print("angle switch commit hardening applied")
