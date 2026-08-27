from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def patch(rel, old, new, label):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# Session time 0 is a valid first timestamp. Recording existence is represented
# by the explicit haveRecording boolean, never by startNs being non-zero.
patch(
    "src/sr-event-dock.cpp",
    "\t\tif (!startNs || endNs <= startNs) {",
    "\t\tif (endNs <= startNs) {",
    "main timeline accepts zero Session start",
)

# The first Multiview frame of a new Session can legitimately be at 0 ns.
# Decoder availability depends on the player, not timestamp truthiness.
patch(
    "src/sr-multiview-dock.cpp",
    "\t\t\tif (player && timestampNs) {",
    "\t\t\tif (player) {",
    "multiview decoder accepts zero Session timestamp",
)

print("Zero Session timestamp UI compatibility applied successfully")
