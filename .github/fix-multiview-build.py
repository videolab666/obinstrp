from pathlib import Path


path = Path("src/sr-multiview-dock.cpp")
text = path.read_text(encoding="utf-8")

replacements = [
    (
        "int dstLinesize[4] = {converted.bytesPerLine(), 0, 0, 0};",
        "int dstLinesize[4] = {static_cast<int>(converted.bytesPerLine()), 0, 0, 0};",
        "QImage bytesPerLine narrowing",
    ),
    (
        '''\tvoid setControlsEnabled(bool enabled)\n\t{\n\t\tfor (QWidget *widget : {static_cast<QWidget *>(autoAngle), playPause, playFromIn, gotoIn, setIn, setOut,\n\t\t\t\t\tgotoOut, prevFrame, nextFrame, loop, fit, live})\n\t\t\twidget->setEnabled(enabled);\n\t\ttimeline->setEnabled(enabled);\n\t}\n''',
        '''\tvoid setControlsEnabled(bool enabled)\n\t{\n\t\tQWidget *controls[] = {autoAngle, playPause, playFromIn, gotoIn, setIn, setOut,\n\t\t\t\t       gotoOut, prevFrame, nextFrame, loop, fit, live};\n\t\tfor (QWidget *widget : controls)\n\t\t\twidget->setEnabled(enabled);\n\t\ttimeline->setEnabled(enabled);\n\t}\n''',
        "heterogeneous Qt control initializer",
    ),
]

for old, new, label in replacements:
    if new in text:
        continue
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    text = text.replace(old, new, 1)

# The Multiview dock talks to the authoritative Event dock through the shared
# editor bridge, so the old controller member is intentionally unnecessary.
text = text.replace("\tsr_event_controller *controller = nullptr;\n", "")

path.write_text(text, encoding="utf-8")
