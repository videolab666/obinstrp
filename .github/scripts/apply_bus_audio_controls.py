from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


header = Path("src/sr-replay-channel.h")
replace_once(
    header,
    '''enum sr_replay_bus {\n\tSR_REPLAY_BUS_A = 0,\n\tSR_REPLAY_BUS_B = 1,\n\tSR_REPLAY_BUS_COUNT = 2,\n};\n\nstruct sr_replay_channel_state {\n''',
    '''enum sr_replay_bus {\n\tSR_REPLAY_BUS_A = 0,\n\tSR_REPLAY_BUS_B = 1,\n\tSR_REPLAY_BUS_COUNT = 2,\n};\n\nenum sr_replay_audio_mode {\n\tSR_REPLAY_AUDIO_OFF = 0,\n\tSR_REPLAY_AUDIO_MASTER = 1,\n};\n\nstruct sr_replay_channel_state {\n''',
)
replace_once(
    header,
    '''\tdouble speed_percent;\n\tuint32_t width;\n''',
    '''\tdouble speed_percent;\n\tenum sr_replay_audio_mode audio_mode;\n\tuint32_t width;\n''',
)
replace_once(
    header,
    '''bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent);\nbool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward);\n''',
    '''bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent);\nbool sr_replay_channel_set_audio_mode(enum sr_replay_bus bus, enum sr_replay_audio_mode audio_mode);\nbool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward);\n''',
)

channel = Path("src/sr-replay-channel.c")
replace_once(
    channel,
    '''\tdouble speed_percent;\n\tuint32_t width;\n''',
    '''\tdouble speed_percent;\n\tenum sr_replay_audio_mode audio_mode;\n\tuint32_t width;\n''',
)
replace_once(
    channel,
    '''\tchannel->speed_percent = 100.0;\n\tchannel->width = 0;\n''',
    '''\tchannel->speed_percent = 100.0;\n\tchannel->audio_mode = SR_REPLAY_AUDIO_MASTER;\n\tchannel->width = 0;\n''',
)
replace_once(
    channel,
    '''\t\tpthread_mutex_init(&channels->buses[i].mutex, NULL);\n\t\tchannels->buses[i].speed_percent = 100.0;\n''',
    '''\t\tpthread_mutex_init(&channels->buses[i].mutex, NULL);\n\t\tchannels->buses[i].speed_percent = 100.0;\n\t\tchannels->buses[i].audio_mode = SR_REPLAY_AUDIO_MASTER;\n''',
)
replace_once(
    channel,
    '''\tpthread_mutex_lock(&channel->mutex);\n\tclear_locked(channel);\n\tchannel->player = player;\n''',
    '''\tpthread_mutex_lock(&channel->mutex);\n\tconst enum sr_replay_audio_mode audio_mode = channel->audio_mode;\n\tclear_locked(channel);\n\tchannel->audio_mode = audio_mode;\n\tchannel->player = player;\n''',
)
replace_once(
    channel,
    '''bool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward)\n{\n''',
    '''bool sr_replay_channel_set_audio_mode(enum sr_replay_bus bus, enum sr_replay_audio_mode audio_mode)\n{\n\tstruct sr_replay_channel *channel = get_bus(bus);\n\tif (!channel || (audio_mode != SR_REPLAY_AUDIO_OFF && audio_mode != SR_REPLAY_AUDIO_MASTER))\n\t\treturn false;\n\tpthread_mutex_lock(&channel->mutex);\n\tchannel->audio_mode = audio_mode;\n\tpthread_mutex_unlock(&channel->mutex);\n\treturn true;\n}\n\nbool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward)\n{\n''',
)
replace_once(
    channel,
    '''\tstate->speed_percent = channel->speed_percent;\n\tstate->width = channel->width;\n''',
    '''\tstate->speed_percent = channel->speed_percent;\n\tstate->audio_mode = channel->audio_mode;\n\tstate->width = channel->width;\n''',
)

output_header = Path("src/sr-event-output.h")
replace_once(
    output_header,
    '''enum sr_event_output_audio_mode {\n\tSR_EVENT_OUTPUT_AUDIO_OFF = 0,\n\tSR_EVENT_OUTPUT_AUDIO_MASTER = 1,\n};\n''',
    '''enum sr_event_output_audio_mode {\n\tSR_EVENT_OUTPUT_AUDIO_OFF = 0,\n\tSR_EVENT_OUTPUT_AUDIO_MASTER = 1,\n\tSR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS = 2,\n};\n''',
)

output = Path("src/sr-event-output.c")
replace_once(
    output,
    '''static bool master_audio_allowed(const struct sr_event_output *output, const struct sr_replay_channel_state *state)\n{\n\treturn output->audio_mode == SR_EVENT_OUTPUT_AUDIO_MASTER && state->cued && state->playing && !state->paused &&\n\t       !state->backward && fabs(state->speed_percent - 100.0) < 0.01;\n}\n''',
    '''static bool master_audio_allowed(const struct sr_event_output *output, const struct sr_replay_channel_state *state)\n{\n\tconst bool master_enabled =\n\t\toutput->audio_mode == SR_EVENT_OUTPUT_AUDIO_MASTER ||\n\t\t(output->audio_mode == SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS && state->audio_mode == SR_REPLAY_AUDIO_MASTER);\n\treturn master_enabled && state->cued && state->playing && !state->paused && !state->backward &&\n\t       fabs(state->speed_percent - 100.0) < 0.01;\n}\n''',
)
replace_once(
    output,
    '''\tif (audio_mode < SR_EVENT_OUTPUT_AUDIO_OFF || audio_mode > SR_EVENT_OUTPUT_AUDIO_MASTER)\n\t\taudio_mode = SR_EVENT_OUTPUT_AUDIO_MASTER;\n''',
    '''\tif (audio_mode < SR_EVENT_OUTPUT_AUDIO_OFF || audio_mode > SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS)\n\t\taudio_mode = SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS;\n''',
)
replace_once(
    output,
    '''\toutput->audio_mode = SR_EVENT_OUTPUT_AUDIO_MASTER;\n''',
    '''\toutput->audio_mode = SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS;\n''',
)
replace_once(
    output,
    '''\tobs_property_list_add_int(audio, obs_module_text("EventOutput.AudioOff"), SR_EVENT_OUTPUT_AUDIO_OFF);\n\tobs_property_list_add_int(audio, obs_module_text("EventOutput.AudioMaster"), SR_EVENT_OUTPUT_AUDIO_MASTER);\n''',
    '''\tobs_property_list_add_int(audio, obs_module_text("EventOutput.AudioFollowBus"), SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS);\n\tobs_property_list_add_int(audio, obs_module_text("EventOutput.AudioOff"), SR_EVENT_OUTPUT_AUDIO_OFF);\n\tobs_property_list_add_int(audio, obs_module_text("EventOutput.AudioMaster"), SR_EVENT_OUTPUT_AUDIO_MASTER);\n''',
)
replace_once(
    output,
    '''\tobs_data_set_default_int(settings, SR_EVENT_OUTPUT_SETTING_AUDIO_MODE, SR_EVENT_OUTPUT_AUDIO_MASTER);\n''',
    '''\tobs_data_set_default_int(settings, SR_EVENT_OUTPUT_SETTING_AUDIO_MODE, SR_EVENT_OUTPUT_AUDIO_FOLLOW_BUS);\n''',
)

# Operator dock audio control follows the selected A/B transport bus.
dock = Path("src/sr-event-dock.cpp")
replace_once(
    dock,
    '''\tif (state.partial_coverage)\n\t\tflags += QStringLiteral(" PARTIAL");\n\n\treturn QStringLiteral("%1: #%2  %3  %4%  %5/%6 s  %7%8")\n''',
    '''\tif (state.partial_coverage)\n\t\tflags += QStringLiteral(" PARTIAL");\n\tflags += state.audio_mode == SR_REPLAY_AUDIO_MASTER ? QStringLiteral(" AUDIO") : QStringLiteral(" MUTE");\n\n\treturn QStringLiteral("%1: #%2  %3  %4%  %5/%6 s  %7%8")\n''',
)
replace_once(
    dock,
    '''\t\tspeedCombo->setCurrentIndex(speedCombo->findData(100));\n\t\tcueBar->addWidget(speedCombo);\n\t\troot->addLayout(cueBar);\n''',
    '''\t\tspeedCombo->setCurrentIndex(speedCombo->findData(100));\n\t\tcueBar->addWidget(speedCombo);\n\t\tcueBar->addWidget(new QLabel(T("EventDock.Audio"), this));\n\t\taudioCombo = new QComboBox(this);\n\t\taudioCombo->addItem(T("EventDock.AudioMaster"), SR_REPLAY_AUDIO_MASTER);\n\t\taudioCombo->addItem(T("EventDock.AudioOff"), SR_REPLAY_AUDIO_OFF);\n\t\tcueBar->addWidget(audioCombo);\n\t\troot->addLayout(cueBar);\n''',
)
replace_once(
    dock,
    '''\t\tconnect(speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {\n\t\t\tif (index >= 0)\n\t\t\t\tsr_replay_channel_set_speed(transportBus(), speedCombo->itemData(index).toDouble());\n\t\t});\n''',
    '''\t\tconnect(speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {\n\t\t\tif (index >= 0)\n\t\t\t\tsr_replay_channel_set_speed(transportBus(), speedCombo->itemData(index).toDouble());\n\t\t});\n\t\tconnect(audioCombo, &QComboBox::currentIndexChanged, this, [this](int index) {\n\t\t\tif (index >= 0)\n\t\t\t\tsr_replay_channel_set_audio_mode(\n\t\t\t\t\ttransportBus(), static_cast<sr_replay_audio_mode>(audioCombo->itemData(index).toInt()));\n\t\t\trefreshTransportStatus();\n\t\t});\n''',
)
replace_once(
    dock,
    '''\t\tconst int speedIndex = speedCombo->findData((int)state.speed_percent);\n\t\tif (speedIndex >= 0)\n\t\t\tspeedCombo->setCurrentIndex(speedIndex);\n\t\trefreshTransportStatus();\n''',
    '''\t\tconst int speedIndex = speedCombo->findData((int)state.speed_percent);\n\t\tif (speedIndex >= 0)\n\t\t\tspeedCombo->setCurrentIndex(speedIndex);\n\t\tconst int audioIndex = audioCombo->findData((int)state.audio_mode);\n\t\tif (audioIndex >= 0)\n\t\t\taudioCombo->setCurrentIndex(audioIndex);\n\t\trefreshTransportStatus();\n''',
)
replace_once(
    dock,
    '''\tQComboBox *speedCombo = nullptr;\n\tQPushButton *reverseButton = nullptr;\n''',
    '''\tQComboBox *speedCombo = nullptr;\n\tQComboBox *audioCombo = nullptr;\n\tQPushButton *reverseButton = nullptr;\n''',
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    '''EventDock.Loop="Loop"\nEventDock.TakeA="TAKE A"\n''',
    '''EventDock.Loop="Loop"\nEventDock.Audio="Audio"\nEventDock.AudioMaster="Master"\nEventDock.AudioOff="Off"\nEventDock.TakeA="TAKE A"\n''',
)
replace_once(
    locale,
    '''EventOutput.Audio="Replay audio"\nEventOutput.AudioOff="Off"\n''',
    '''EventOutput.Audio="Replay audio"\nEventOutput.AudioFollowBus="Follow A/B bus"\nEventOutput.AudioOff="Off"\n''',
)
replace_once(
    locale,
    '''EventOutput.Audio.Description="Master audio currently plays only at 100% forward speed. Reverse and slow/fast replay are muted until varispeed/time-stretch support is added."\n''',
    '''EventOutput.Audio.Description="Follow A/B bus is recommended so the operator dock controls replay audio independently on buses A and B. Master audio currently plays only at 100% forward speed; reverse and slow/fast replay are muted until varispeed/time-stretch support is added."\n''',
)
