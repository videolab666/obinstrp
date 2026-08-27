from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel, text):
    (ROOT / rel).write_text(text, encoding="utf-8", newline="\n")


def once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


# Main Event editor timeline: selecting a different Event focuses the viewport
# once. Manual wheel zoom/pan/FIT remains untouched while the same Event stays
# selected. 20% left + 20% right margins means the Event occupies ~60% width.
rel = "src/sr-event-dock.cpp"
text = read(rel)
old = '''\tvoid setSelection(uint64_t inNs, uint64_t outNs)\n\t{\n\t\tif (!haveRecording || outNs <= inNs) {\n\t\t\thaveSelection = false;\n\t\t\tupdate();\n\t\t\treturn;\n\t\t}\n\t\thaveSelection = true;\n\t\tselectionInNs = inNs;\n\t\tselectionOutNs = outNs;\n\t\tif (zoomLocked && (selectionOutNs < viewStartNs || selectionInNs > viewEndNs)) {\n\t\t\tconst uint64_t midpoint = selectionInNs + (selectionOutNs - selectionInNs) / 2;\n\t\t\tcenterView(midpoint);\n\t\t}\n\t\tupdate();\n\t}\n'''
new = old + '''\n\tvoid focusSelection(uint64_t inNs, uint64_t outNs)\n\t{\n\t\tif (!haveRecording || outNs <= inNs || recordEndNs <= recordStartNs)\n\t\t\treturn;\n\t\tinNs = clampToRecording(inNs);\n\t\toutNs = clampToRecording(outNs);\n\t\tif (outNs <= inNs)\n\t\t\treturn;\n\n\t\tconst uint64_t total = recordEndNs - recordStartNs;\n\t\tconst uint64_t eventSpan = outNs - inNs;\n\t\t/* Leave about 20% of the viewport on each side of the selected Event.\n\t\t * A 250 ms floor keeps single-frame/very short Events editable. */\n\t\tuint64_t span = (uint64_t)std::ceil((long double)eventSpan / 0.60L);\n\t\tspan = std::max<uint64_t>(250000000ULL, std::max<uint64_t>(eventSpan, span));\n\t\tspan = std::min<uint64_t>(span, total);\n\t\tif (span >= total) {\n\t\t\tfitView();\n\t\t\treturn;\n\t\t}\n\n\t\tconst uint64_t center = inNs + eventSpan / 2;\n\t\tzoomLocked = true;\n\t\tviewStartNs = center > span / 2 ? center - span / 2 : recordStartNs;\n\t\tviewEndNs = viewStartNs <= UINT64_MAX - span ? viewStartNs + span : recordEndNs;\n\t\tclampView();\n\t\tupdate();\n\t}\n'''
text = once(text, old, new, "main timeline focusSelection")
text = once(
    text,
    '''\t\ttimelineEventId = event.id;\n\t\teditTimeline->setEnabled(true);\n\t\teditTimeline->setSelection(event.in_ns, event.out_ns);\n''',
    '''\t\tconst bool eventChanged = timelineEventId != event.id;\n\t\ttimelineEventId = event.id;\n\t\teditTimeline->setEnabled(true);\n\t\teditTimeline->setSelection(event.in_ns, event.out_ns);\n\t\tif (eventChanged)\n\t\t\teditTimeline->focusSelection(event.in_ns, event.out_ns);\n''',
    "focus newly selected Event",
)
write(rel, text)


# Replay Multiview timeline: mirror the same one-shot Event focus and make its
# ruler explicit/readable. The previous ruler existed but used low-contrast
# text and a fixed <=8-tick heuristic that could overlap on long sessions.
rel = "src/sr-multiview-dock.cpp"
text = read(rel)
old = '''\tvoid setState(const sr_event_editor_snapshot &state)\n\t{\n\t\tconst bool first = !haveRecording;\n\t\thaveRecording = state.record_end_ns > state.record_start_ns;\n\t\tif (!haveRecording) {\n\t\t\tupdate();\n\t\t\treturn;\n\t\t}\n\t\trecordStartNs = state.record_start_ns;\n\t\trecordEndNs = state.record_end_ns;\n\t\tif (first || !zoomLocked) {\n\t\t\tviewStartNs = recordStartNs;\n\t\t\tviewEndNs = recordEndNs;\n\t\t} else {\n\t\t\tclampView();\n\t\t}\n\t\tif (drag == Drag::None) {\n\t\t\tplayheadNs = clamp(state.playhead_ns);\n\t\t\tinNs = clamp(state.in_ns);\n\t\t\toutNs = clamp(state.out_ns);\n\t\t\thaveRange = state.out_ns > state.in_ns;\n\t\t}\n\t\tupdate();\n\t}\n'''
new = '''\tvoid setState(const sr_event_editor_snapshot &state)\n\t{\n\t\tconst bool first = !haveRecording;\n\t\tconst bool eventChanged = state.event_id && state.event_id != focusedEventId;\n\t\tif (!state.event_id)\n\t\t\tfocusedEventId = 0;\n\t\thaveRecording = state.record_end_ns > state.record_start_ns;\n\t\tif (!haveRecording) {\n\t\t\tupdate();\n\t\t\treturn;\n\t\t}\n\t\trecordStartNs = state.record_start_ns;\n\t\trecordEndNs = state.record_end_ns;\n\t\tif (first || !zoomLocked) {\n\t\t\tviewStartNs = recordStartNs;\n\t\t\tviewEndNs = recordEndNs;\n\t\t} else {\n\t\t\tclampView();\n\t\t}\n\t\tif (drag == Drag::None) {\n\t\t\tplayheadNs = clamp(state.playhead_ns);\n\t\t\tinNs = clamp(state.in_ns);\n\t\t\toutNs = clamp(state.out_ns);\n\t\t\thaveRange = state.out_ns > state.in_ns;\n\t\t\tif (eventChanged && haveRange)\n\t\t\t\tfocusRange(inNs, outNs);\n\t\t}\n\t\tif (state.event_id)\n\t\t\tfocusedEventId = state.event_id;\n\t\tupdate();\n\t}\n'''
text = once(text, old, new, "multiview setState event focus")

old = '''\tvoid paintRuler(QPainter &painter, const QRect &area)\n\t{\n\t\tconst uint64_t span = viewSpan();\n\t\tif (!span)\n\t\t\treturn;\n\t\tconst uint64_t candidates[] = {100000000ULL,   250000000ULL,    500000000ULL,   1000000000ULL,\n\t\t\t\t\t       2000000000ULL,  5000000000ULL,   10000000000ULL, 30000000000ULL,\n\t\t\t\t\t       60000000000ULL, 300000000000ULL, 600000000000ULL};\n\t\tuint64_t step = candidates[0];\n\t\tfor (uint64_t candidate : candidates) {\n\t\t\tstep = candidate;\n\t\t\tif (span / candidate <= 8)\n\t\t\t\tbreak;\n\t\t}\n\t\tconst uint64_t first = ((viewStartNs + step - 1) / step) * step;\n\t\tpainter.setPen(palette().mid().color());\n\t\tfor (uint64_t t = first; t <= viewEndNs && t <= UINT64_MAX - step; t += step) {\n\t\t\tconst int x = xFromTimestamp(t);\n\t\t\tpainter.drawLine(x, area.top(), x, area.top() + 6);\n\t\t\tconst uint64_t relative = t > recordStartNs ? t - recordStartNs : 0;\n\t\t\tpainter.drawText(QRect(x - 48, 1, 96, 15), Qt::AlignCenter, clockText(relative));\n\t\t}\n\t}\n'''
new = '''\tvoid paintRuler(QPainter &painter, const QRect &area)\n\t{\n\t\tconst uint64_t span = viewSpan();\n\t\tif (!span || area.width() <= 0)\n\t\t\treturn;\n\n\t\tconst QRect rulerBand(area.left(), 0, area.width(), area.top());\n\t\tpainter.fillRect(rulerBand, palette().alternateBase());\n\t\tpainter.setPen(palette().mid().color());\n\t\tpainter.drawLine(area.left(), area.top() - 1, area.right(), area.top() - 1);\n\n\t\t/* Select the major step by pixel density, not just tick count. This keeps\n\t\t * labels readable from sub-second edits through multi-hour sessions. */\n\t\tconst uint64_t candidates[] = {\n\t\t\t100000000ULL,      250000000ULL,       500000000ULL,       1000000000ULL,\n\t\t\t2000000000ULL,     5000000000ULL,      10000000000ULL,     30000000000ULL,\n\t\t\t60000000000ULL,    120000000000ULL,    300000000000ULL,    600000000000ULL,\n\t\t\t900000000000ULL,   1800000000000ULL,   3600000000000ULL,   7200000000000ULL,\n\t\t\t21600000000000ULL, 43200000000000ULL,  86400000000000ULL};\n\t\tuint64_t step = candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];\n\t\tfor (uint64_t candidate : candidates) {\n\t\t\tconst long double pixels = (long double)area.width() * candidate / (long double)span;\n\t\t\tif (pixels >= 88.0L) {\n\t\t\t\tstep = candidate;\n\t\t\t\tbreak;\n\t\t\t}\n\t\t}\n\n\t\tconst uint64_t relativeStart = viewStartNs > recordStartNs ? viewStartNs - recordStartNs : 0;\n\t\tuint64_t firstRelative = (relativeStart / step) * step;\n\t\tif (firstRelative < relativeStart && firstRelative <= UINT64_MAX - step)\n\t\t\tfirstRelative += step;\n\t\tif (firstRelative > UINT64_MAX - recordStartNs)\n\t\t\treturn;\n\t\tconst uint64_t first = recordStartNs + firstRelative;\n\n\t\t/* Minor ticks make the scale useful while scrubbing even when the next\n\t\t * labelled major mark is relatively far away. */\n\t\tconst uint64_t minorStep = step / 4;\n\t\tif (minorStep) {\n\t\t\tuint64_t firstMinorRelative = (relativeStart / minorStep) * minorStep;\n\t\t\tif (firstMinorRelative < relativeStart && firstMinorRelative <= UINT64_MAX - minorStep)\n\t\t\t\tfirstMinorRelative += minorStep;\n\t\t\tif (firstMinorRelative <= UINT64_MAX - recordStartNs) {\n\t\t\t\tfor (uint64_t relative = firstMinorRelative; relative <= viewEndNs - recordStartNs;) {\n\t\t\t\t\tif (relative % step != 0) {\n\t\t\t\t\t\tconst int x = xFromTimestamp(recordStartNs + relative);\n\t\t\t\t\t\tpainter.drawLine(x, area.top() - 4, x, area.top() - 1);\n\t\t\t\t\t}\n\t\t\t\t\tif (relative > UINT64_MAX - minorStep)\n\t\t\t\t\t\tbreak;\n\t\t\t\t\trelative += minorStep;\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\n\t\tconst QRect zoomReserved(area.right() - 86, 0, 86, area.top());\n\t\tfor (uint64_t t = first; t <= viewEndNs;) {\n\t\t\tconst int x = xFromTimestamp(t);\n\t\t\tpainter.setPen(palette().mid().color());\n\t\t\tpainter.drawLine(x, area.top() - 8, x, area.top() - 1);\n\t\t\tconst uint64_t relative = t > recordStartNs ? t - recordStartNs : 0;\n\t\t\tconst QString labelText = clockText(relative);\n\t\t\tconst int labelWidth = std::max(76, painter.fontMetrics().horizontalAdvance(labelText) + 10);\n\t\t\tconst QRect label(x - labelWidth / 2, 1, labelWidth, std::max(12, area.top() - 8));\n\t\t\tif (label.right() >= area.left() && label.left() <= area.right() &&\n\t\t\t    !label.intersects(zoomReserved)) {\n\t\t\t\tpainter.setPen(palette().text().color());\n\t\t\t\tpainter.drawText(label, Qt::AlignHCenter | Qt::AlignTop, labelText);\n\t\t\t}\n\t\t\tif (t > UINT64_MAX - step)\n\t\t\t\tbreak;\n\t\t\tt += step;\n\t\t}\n\t}\n'''
text = once(text, old, new, "multiview readable ruler")

marker = '''\tvoid pan(int direction, uint64_t amount)\n'''
focus = '''\tvoid focusRange(uint64_t rangeInNs, uint64_t rangeOutNs)\n\t{\n\t\tif (!haveRecording || rangeOutNs <= rangeInNs || recordEndNs <= recordStartNs)\n\t\t\treturn;\n\t\trangeInNs = clamp(rangeInNs);\n\t\trangeOutNs = clamp(rangeOutNs);\n\t\tif (rangeOutNs <= rangeInNs)\n\t\t\treturn;\n\n\t\tconst uint64_t total = recordEndNs - recordStartNs;\n\t\tconst uint64_t rangeSpan = rangeOutNs - rangeInNs;\n\t\tuint64_t span = (uint64_t)std::ceil((long double)rangeSpan / 0.60L);\n\t\tspan = std::max<uint64_t>(250000000ULL, std::max<uint64_t>(rangeSpan, span));\n\t\tspan = std::min<uint64_t>(span, total);\n\t\tif (span >= total) {\n\t\t\tzoomLocked = false;\n\t\t\tviewStartNs = recordStartNs;\n\t\t\tviewEndNs = recordEndNs;\n\t\t\treturn;\n\t\t}\n\n\t\tconst uint64_t center = rangeInNs + rangeSpan / 2;\n\t\tzoomLocked = true;\n\t\tviewStartNs = center > span / 2 ? center - span / 2 : recordStartNs;\n\t\tviewEndNs = viewStartNs <= UINT64_MAX - span ? viewStartNs + span : recordEndNs;\n\t\tclampView();\n\t}\n\n'''
text = once(text, marker, focus + marker, "multiview focusRange")
text = once(
    text,
    '''\tuint64_t outNs = 0;\n\tDrag drag = Drag::None;\n''',
    '''\tuint64_t outNs = 0;\n\tuint64_t focusedEventId = 0;\n\tDrag drag = Drag::None;\n''',
    "multiview focused event id",
)
write(rel, text)

print("Event timeline focus and Multiview ruler patch applied successfully")
