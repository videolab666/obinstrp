from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


disk_h = Path("src/sr-disk-player.h")
replace_once(
    disk_h,
    '''bool sr_disk_player_decode_at(struct sr_disk_player *player, uint64_t target_ns, AVFrame **frame,\n\t\t\t      uint64_t *actual_timestamp_ns);\n\n#ifdef __cplusplus\n''',
    '''bool sr_disk_player_decode_at(struct sr_disk_player *player, uint64_t target_ns, AVFrame **frame,\n\t\t\t      uint64_t *actual_timestamp_ns);\n\n/* Finds the timestamp of the immediately adjacent indexed frame without\n * disturbing the persistent decoder state. direction must be -1 or +1. */\nbool sr_disk_player_neighbor_timestamp(struct sr_disk_player *player, uint64_t current_ns, int direction,\n\t\t\t\t       uint64_t *timestamp_ns);\n\n#ifdef __cplusplus\n''',
)

disk_c = Path("src/sr-disk-player.c")
append = r'''

static bool segment_neighbor_timestamp(const struct sr_segment_descriptor *segment, uint64_t current_ns, int direction,
                                       uint64_t *timestamp_ns)
{
    struct sr_segment_reader *reader = sr_segment_reader_open(segment->segment_path, segment->index_path);
    if (!reader)
        return false;
    if (segment->active)
        sr_segment_reader_refresh_index(reader);

    const size_t count = sr_segment_reader_entry_count(reader);
    bool found = false;
    if (count) {
        if (direction > 0) {
            struct sr_index_entry first;
            if (sr_segment_reader_entry_at(reader, 0, &first) && first.timestamp_ns > current_ns) {
                *timestamp_ns = first.timestamp_ns;
                found = true;
            } else {
                size_t pos = 0;
                if (sr_segment_reader_find_position(reader, current_ns, false, &pos, NULL)) {
                    for (size_t i = pos + 1; i < count; i++) {
                        struct sr_index_entry entry;
                        if (sr_segment_reader_entry_at(reader, i, &entry) && entry.timestamp_ns > current_ns) {
                            *timestamp_ns = entry.timestamp_ns;
                            found = true;
                            break;
                        }
                    }
                }
            }
        } else {
            struct sr_index_entry last;
            if (sr_segment_reader_entry_at(reader, count - 1, &last) && last.timestamp_ns < current_ns) {
                *timestamp_ns = last.timestamp_ns;
                found = true;
            } else {
                size_t pos = 0;
                struct sr_index_entry entry;
                if (sr_segment_reader_find_position(reader, current_ns, false, &pos, &entry)) {
                    if (entry.timestamp_ns < current_ns) {
                        *timestamp_ns = entry.timestamp_ns;
                        found = true;
                    } else {
                        while (pos > 0) {
                            pos--;
                            if (sr_segment_reader_entry_at(reader, pos, &entry) && entry.timestamp_ns < current_ns) {
                                *timestamp_ns = entry.timestamp_ns;
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    sr_segment_reader_close(reader);
    return found;
}

bool sr_disk_player_neighbor_timestamp(struct sr_disk_player *p, uint64_t current_ns, int direction,
                                       uint64_t *timestamp_ns)
{
    if (!p || !timestamp_ns || (direction != -1 && direction != 1))
        return false;
    if (!sr_disk_player_refresh(p) || !p->segment_count)
        return false;

    if (direction > 0) {
        for (size_t i = 0; i < p->segment_count; i++) {
            if (p->segments[i].end_ns <= current_ns)
                continue;
            if (segment_neighbor_timestamp(&p->segments[i], current_ns, direction, timestamp_ns))
                return true;
        }
    } else {
        for (size_t i = p->segment_count; i > 0; i--) {
            if (p->segments[i - 1].start_ns >= current_ns)
                continue;
            if (segment_neighbor_timestamp(&p->segments[i - 1], current_ns, direction, timestamp_ns))
                return true;
        }
    }
    return false;
}
'''
text = disk_c.read_text(encoding="utf-8")
if "bool sr_disk_player_neighbor_timestamp(" not in text:
    disk_c.write_text(text.rstrip() + append + "\n", encoding="utf-8")

channel_h = Path("src/sr-replay-channel.h")
replace_once(
    channel_h,
    '''bool sr_replay_channel_seek(enum sr_replay_bus bus, uint64_t timestamp_ns);\nbool sr_replay_channel_seek_relative(enum sr_replay_bus bus, int64_t delta_ns);\n''',
    '''bool sr_replay_channel_seek(enum sr_replay_bus bus, uint64_t timestamp_ns);\nbool sr_replay_channel_seek_relative(enum sr_replay_bus bus, int64_t delta_ns);\nbool sr_replay_channel_step_frames(enum sr_replay_bus bus, int frames);\n''',
)

channel_c = Path("src/sr-replay-channel.c")
replace_once(
    channel_c,
    '''bool sr_replay_channel_get_state(enum sr_replay_bus bus, struct sr_replay_channel_state *state)\n{\n''',
    '''bool sr_replay_channel_step_frames(enum sr_replay_bus bus, int frames)\n{\n\tstruct sr_replay_channel *channel = get_bus(bus);\n\tif (!channel || !frames)\n\t\treturn false;\n\tif (frames > 1000)\n\t\tframes = 1000;\n\tif (frames < -1000)\n\t\tframes = -1000;\n\n\tconst int direction = frames > 0 ? 1 : -1;\n\tunsigned remaining = frames > 0 ? (unsigned)frames : (unsigned)(-frames);\n\tbool moved = false;\n\n\tpthread_mutex_lock(&channel->mutex);\n\tif (!channel->cued || !channel->player) {\n\t\tpthread_mutex_unlock(&channel->mutex);\n\t\treturn false;\n\t}\n\n\twhile (remaining--) {\n\t\tuint64_t adjacent = 0;\n\t\tif (!sr_disk_player_neighbor_timestamp(channel->player, channel->playhead_ns, direction, &adjacent))\n\t\t\tbreak;\n\t\tif (adjacent < channel->in_ns || adjacent > channel->out_ns)\n\t\t\tbreak;\n\t\tchannel->playhead_ns = adjacent;\n\t\tmoved = true;\n\t}\n\n\tif (moved) {\n\t\tchannel->playing = true;\n\t\tchannel->paused = true;\n\t\tchannel->last_clock_ns = 0;\n\t\tchannel->need_frame = true;\n\t}\n\tpthread_mutex_unlock(&channel->mutex);\n\treturn moved;\n}\n\nbool sr_replay_channel_get_state(enum sr_replay_bus bus, struct sr_replay_channel_state *state)\n{\n''',
)

# Add operator buttons beside Restart/Reverse.
dock = Path("src/sr-event-dock.cpp")
replace_once(
    dock,
    '''\t\tauto *restart = new QPushButton(T("EventDock.Restart"), this);\n\t\treverseButton = new QPushButton(T("EventDock.Reverse"), this);\n''',
    '''\t\tauto *restart = new QPushButton(T("EventDock.Restart"), this);\n\t\tauto *prevFrame = new QPushButton(T("EventDock.PrevFrame"), this);\n\t\tauto *nextFrame = new QPushButton(T("EventDock.NextFrame"), this);\n\t\tprevFrame->setToolTip(T("EventDock.PrevFrame.Tooltip"));\n\t\tnextFrame->setToolTip(T("EventDock.NextFrame.Tooltip"));\n\t\treverseButton = new QPushButton(T("EventDock.Reverse"), this);\n''',
)
replace_once(
    dock,
    '''\t\tcueBar->addWidget(restart);\n\t\tcueBar->addWidget(reverseButton);\n''',
    '''\t\tcueBar->addWidget(restart);\n\t\tcueBar->addWidget(prevFrame);\n\t\tcueBar->addWidget(nextFrame);\n\t\tcueBar->addWidget(reverseButton);\n''',
)
replace_once(
    dock,
    '''\t\tconnect(restart, &QPushButton::clicked, this, [this]() { restartTransport(); });\n\t\tconnect(reverseButton, &QPushButton::clicked, this,\n''',
    '''\t\tconnect(restart, &QPushButton::clicked, this, [this]() { restartTransport(); });\n\t\tconnect(prevFrame, &QPushButton::clicked, this, [this]() { stepFrame(-1); });\n\t\tconnect(nextFrame, &QPushButton::clicked, this, [this]() { stepFrame(1); });\n\t\tconnect(reverseButton, &QPushButton::clicked, this,\n''',
)
replace_once(
    dock,
    '''\tvoid setMarkIn()\n\t{\n''',
    '''\tvoid stepFrame(int direction)\n\t{\n\t\tif (!sr_replay_channel_step_frames(transportBus(), direction)) {\n\t\t\tsetStatus("EventDock.FrameStepFailed");\n\t\t\treturn;\n\t\t}\n\t\trefreshTransportStatus();\n\t}\n\n\tvoid setMarkIn()\n\t{\n''',
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    '''EventDock.Restart="Restart"\nEventDock.Reverse="Reverse"\n''',
    '''EventDock.Restart="Restart"\nEventDock.PrevFrame="◀|"\nEventDock.NextFrame="|▶"\nEventDock.PrevFrame.Tooltip="Previous recorded frame"\nEventDock.NextFrame.Tooltip="Next recorded frame"\nEventDock.FrameStepFailed="No adjacent frame is available inside this Event"\nEventDock.Reverse="Reverse"\n''',
)
