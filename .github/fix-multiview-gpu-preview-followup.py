from pathlib import Path

path = Path('src/sr-multiview-dock.cpp')
text = path.read_text(encoding='utf-8')


def replace_once(old: str, new: str, label: str):
    global text
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one match, found {count}')
    text = text.replace(old, new, 1)


replace_once(
'''\t\tsetSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);\n\t\tsetMinimumSize(160, 90);\n\t\trenderer = sr_gpu_renderer_create();\n'''.replace('\\t','\t').replace('\\n','\n'),
'''\t\tsetSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);\n\t\tsetMinimumSize(160, 90);\n\t\tpthread_mutex_init(&frameMutex, nullptr);\n\t\trenderer = sr_gpu_renderer_create();\n'''.replace('\\t','\t').replace('\\n','\n'),
'GPU display mutex init')

replace_once(
'''\tvoid hideEvent(QHideEvent *event) override\n\t{\n\t\tif (display)\n\t\t\tobs_display_set_enabled(display, false);\n\t\tQWidget::hideEvent(event);\n\t}\n\nprivate:\n'''.replace('\\t','\t').replace('\\n','\n'),
'''\tvoid hideEvent(QHideEvent *event) override\n\t{\n\t\tif (display)\n\t\t\tobs_display_set_enabled(display, false);\n\t\tQWidget::hideEvent(event);\n\t}\n\n\tvoid resizeEvent(QResizeEvent *event) override\n\t{\n\t\tQWidget::resizeEvent(event);\n\t\tcreateDisplay();\n\t\tif (display) {\n\t\t\tconst QSize size = previewPixelSize(this);\n\t\t\tobs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());\n\t\t}\n\t}\n\nprivate:\n'''.replace('\\t','\t').replace('\\n','\n'),
'GPU display resize')

# Remove the stale tile resize handler left from the old QLabel/QPixmap implementation.
stale = '''\tvoid resizeEvent(QResizeEvent *event) override\n\t{\n\t\tQFrame::resizeEvent(event);\n\t\trefreshPixmap();\n\t}\n\n'''.replace('\\t','\t').replace('\\n','\n')
text = text.replace(stale, '')

text = text.replace('\tpthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;\n', '\tpthread_mutex_t frameMutex;\n')

replace_once(
'''\t\tif (coverage == SR_REPLAY_COVERAGE_NONE)\n\t\t\tsetMessage(T("Multiview.NoCoverage"));\n\t\telse if (!atPlayhead)\n\t\t\tsetMessage(T("Multiview.NoMediaAtCursor"));\n'''.replace('\\t','\t').replace('\\n','\n'),
'''\t\tif (coverage == SR_REPLAY_COVERAGE_NONE) {\n\t\t\tclearFrame();\n\t\t\tsetMessage(T("Multiview.NoCoverage"));\n\t\t} else if (!atPlayhead) {\n\t\t\tclearFrame();\n\t\t\tsetMessage(T("Multiview.NoMediaAtCursor"));\n\t\t}\n'''.replace('\\t','\t').replace('\\n','\n'),
'clear stale frame outside coverage')

# GPU preview renders at the native tile pixel size, so the old QImage downscale selector is misleading.
quality_block = '''\t\ttoolbar->addWidget(new QLabel(T("Multiview.Quality"), this));\n\t\tquality = new QComboBox(this);\n\t\tquality->addItem(T("Multiview.QualityAuto"), 0);\n\t\tquality->addItem(QStringLiteral("360p"), 360);\n\t\tquality->addItem(QStringLiteral("540p"), 540);\n\t\tquality->addItem(QStringLiteral("720p"), 720);\n\t\tquality->addItem(T("Multiview.QualitySource"), -1);\n\t\ttoolbar->addWidget(quality);\n'''.replace('\\t','\t').replace('\\n','\n')
text = text.replace(quality_block, '')
text = text.replace('\t\tconnect(quality, &QComboBox::currentIndexChanged, this, [this](int) { forceDecode = true; });\n', '')
text = text.replace('\tQComboBox *quality = nullptr;\n', '')

# Remove now-unused software-preview target height plumbing.
old_target = '''\tint targetHeight() const\n\t{\n\t\tconst int configured = quality->currentData().toInt();\n\t\tif (configured)\n\t\t\treturn configured;\n\t\tconst int count = (int)visibleTiles().size();\n\t\treturn count <= 2 ? 720 : count <= 4 ? 540 : 360;\n\t}\n\n'''.replace('\\t','\t').replace('\\n','\n')
text = text.replace(old_target, '')
text = text.replace('''\tvoid request(uint64_t timestampNs, int targetHeight)\n\t{\n\t\tUNUSED_PARAMETER(targetHeight);\n'''.replace('\\t','\t').replace('\\n','\n'),
                    '''\tvoid request(uint64_t timestampNs)\n\t{\n'''.replace('\\t','\t').replace('\\n','\n'))
text = text.replace('\t\tconst int height = targetHeight();\n', '')
text = text.replace('\t\t\ttile->decoder().request(cameraTimestamp, height);\n', '\t\t\ttile->decoder().request(cameraTimestamp);\n')

path.write_text(text, encoding='utf-8')
