from pathlib import Path

path = Path("src/sr-event-dock.cpp")
text = path.read_text(encoding="utf-8")
old = "cell->findChild<ToggleSwitch *>()"
new = "cell->findChild<QCheckBox *>()"
count = text.count(old)
if count != 2:
    raise SystemExit(f"expected exactly 2 ToggleSwitch findChild calls, got {count}")
path.write_text(text.replace(old, new), encoding="utf-8")
print("Replaced local ToggleSwitch findChild calls with Qt6-safe QCheckBox lookup")
