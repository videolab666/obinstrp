from pathlib import Path

p = Path('src/sr-event-dock.cpp')
s = p.read_text(encoding='utf-8')
start_marker = '\tQString selectedCamera() const'
end_marker = '\n\tuint64_t selectedEventId() const'
start = s.index(start_marker)
end = s.index(end_marker, start)
region = s[start:end]
region = region.replace('\\t', '\t')
s = s[:start] + region + s[end:]
p.write_text(s, encoding='utf-8')
