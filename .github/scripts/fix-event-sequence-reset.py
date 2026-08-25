from pathlib import Path

p = Path('src/sr-replay-playlist.c')
s = p.read_text(encoding='utf-8')

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit(f'missing anchor: {label}')
    s = s.replace(old, new, 1)

rep('''static enum sr_replay_bus other_bus(enum sr_replay_bus bus)
{
	return bus == SR_REPLAY_BUS_A ? SR_REPLAY_BUS_B : SR_REPLAY_BUS_A;
}
''', '''static enum sr_replay_bus other_bus(enum sr_replay_bus bus)
{
	return bus == SR_REPLAY_BUS_A ? SR_REPLAY_BUS_B : SR_REPLAY_BUS_A;
}

/* A sequence item must always begin at Event IN. sr_replay_channel_cue() has
 * an intentional same-Event/different-camera fast path for live angle
 * switching that preserves the current playhead; that behavior is wrong for
 * sequential angle playback (and for a repeated Event two A/B hops later).
 * Clear the off-air transport before cueing, while preserving the operator's
 * per-bus replay-audio choice. */
static void reset_bus_for_sequence(enum sr_replay_bus bus)
{
	struct sr_replay_channel_state state = {0};
	const enum sr_replay_audio_mode audio_mode =
		sr_replay_channel_get_state(bus, &state) ? state.audio_mode : SR_REPLAY_AUDIO_MASTER;
	sr_replay_channel_clear(bus);
	sr_replay_channel_set_audio_mode(bus, audio_mode);
}
''', 'sequence reset helper')

rep('''	playlist->cross_bus_transitions = cross_bus_transitions;

	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_item_locked(bus, playlist, first) && sr_replay_channel_play(bus)) {
''', '''	playlist->cross_bus_transitions = cross_bus_transitions;

	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_item_locked(bus, playlist, first) && sr_replay_channel_play(bus)) {
''', 'list start stable anchor')

# The second occurrence is angle-sequence start: reset the selected bus once
# before trying its first usable camera.
angle_anchor = '''	playlist->count = count;
	playlist->cross_bus_transitions = cross_bus_transitions;

	size_t first = 0;
	bool cued = false;
'''
if s.count(angle_anchor) != 1:
    raise SystemExit(f'unexpected angle start anchor count: {s.count(angle_anchor)}')
s = s.replace(angle_anchor, '''	playlist->count = count;
	playlist->cross_bus_transitions = cross_bus_transitions;

	reset_bus_for_sequence(bus);
	size_t first = 0;
	bool cued = false;
''', 1)

rep('''	for (size_t next = playlist->position + 1; next < playlist->count; next++) {
		const bool cross_bus = playlist->cross_bus_transitions;
		const enum sr_replay_bus target_bus = cross_bus ? other_bus(bus) : bus;
		if (!cue_item_locked(target_bus, playlist, next))
''', '''	const bool cross_bus = playlist->cross_bus_transitions;
	const enum sr_replay_bus target_bus = cross_bus ? other_bus(bus) : bus;
	if (cross_bus || playlist->angle_sequence)
		reset_bus_for_sequence(target_bus);

	for (size_t next = playlist->position + 1; next < playlist->count; next++) {
		if (!cue_item_locked(target_bus, playlist, next))
''', 'fresh target before sequence advance')
p.write_text(s, encoding='utf-8')

p = Path('src/sr-dock.cpp')
s = p.read_text(encoding='utf-8')
old = '''	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		if (strcmp(obs_source_get_unversioned_id(transition), "obs_stinger_transition") == 0)
			continue;
		const QString name = QString::fromUtf8(obs_source_get_name(transition));
'''
new = '''	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		const char *transitionId = obs_source_get_unversioned_id(transition);
		if (strcmp(transitionId, "obs_stinger_transition") == 0 || strcmp(transitionId, "cut_transition") == 0)
			continue;
		const QString name = QString::fromUtf8(obs_source_get_name(transition));
'''
if old not in s:
    raise SystemExit('missing event transition enumeration anchor')
s = s.replace(old, new, 1)
p.write_text(s, encoding='utf-8')
