#!/usr/bin/env python3
"""Inspect Sports Replay .srseg/.sridx files without OBS or FFmpeg bindings.

Format v1 is intentionally little-endian and currently targets Windows x64.
This tool validates headers, packet framing, index offsets/timestamps, keyframe
flags, and reports continuity/statistics. It uses only the Python stdlib.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import struct
import sys
from typing import BinaryIO

SEG_MAGIC = b"SRSEG01\0"
IDX_MAGIC = b"SRIDX01\0"
PACKET_MAGIC = 0x5352504B
FORMAT_VERSION = 1

SEGMENT_HEADER = struct.Struct("<8sIIIIIIIIQII")
PACKET_HEADER = struct.Struct("<IIBBHQqqq")
INDEX_HEADER = struct.Struct("<8sIIIIQ")
INDEX_ENTRY = struct.Struct("<QQIIB7x")

PACKET_VIDEO = 1
PACKET_AUDIO = 2
PACKET_FLAG_KEYFRAME = 0x01
PACKET_FLAG_DISCONTINUITY = 0x02
SEGMENT_FLAG_DISCONTINUITY = 0x00000001


@dataclasses.dataclass(frozen=True)
class SegmentHeader:
    version: int
    camera_hash: int
    sequence: int
    codec_id: int
    width: int
    height: int
    fps_num: int
    fps_den: int
    start_ns: int
    extradata_size: int
    flags: int


@dataclasses.dataclass(frozen=True)
class IndexEntry:
    timestamp_ns: int
    file_offset: int
    packet_size: int
    frame_number: int
    keyframe: bool


def die(message: str) -> "NoReturn":
    raise RuntimeError(message)


def read_exact(f: BinaryIO, n: int) -> bytes:
    data = f.read(n)
    if len(data) != n:
        die(f"unexpected EOF: needed {n} bytes, got {len(data)}")
    return data


def parse_segment_header(f: BinaryIO) -> SegmentHeader:
    values = SEGMENT_HEADER.unpack(read_exact(f, SEGMENT_HEADER.size))
    (
        magic,
        version,
        camera_hash,
        sequence,
        codec_id,
        width,
        height,
        fps_num,
        fps_den,
        start_ns,
        extradata_size,
        flags,
    ) = values
    if magic != SEG_MAGIC:
        die(f"bad segment magic {magic!r}")
    if version != FORMAT_VERSION:
        die(f"unsupported segment version {version}")
    return SegmentHeader(
        version,
        camera_hash,
        sequence,
        codec_id,
        width,
        height,
        fps_num,
        fps_den,
        start_ns,
        extradata_size,
        flags,
    )


def parse_index(path: pathlib.Path) -> tuple[tuple[int, int, int], list[IndexEntry]]:
    with path.open("rb") as f:
        magic, version, camera_hash, sequence, reserved, start_ns = INDEX_HEADER.unpack(
            read_exact(f, INDEX_HEADER.size)
        )
        if magic != IDX_MAGIC:
            die(f"bad index magic {magic!r}")
        if version != FORMAT_VERSION:
            die(f"unsupported index version {version}")
        if reserved != 0:
            print(f"warning: non-zero index reserved field: {reserved}", file=sys.stderr)

        payload = f.read()
        complete = len(payload) // INDEX_ENTRY.size
        trailing = len(payload) % INDEX_ENTRY.size
        if trailing:
            print(
                f"warning: index contains {trailing} trailing byte(s) after {complete} complete records",
                file=sys.stderr,
            )

        entries: list[IndexEntry] = []
        for offset in range(0, complete * INDEX_ENTRY.size, INDEX_ENTRY.size):
            ts, file_offset, packet_size, frame_number, keyframe = INDEX_ENTRY.unpack_from(payload, offset)
            entries.append(IndexEntry(ts, file_offset, packet_size, frame_number, bool(keyframe)))

    return (camera_hash, sequence, start_ns), entries


def validate_pair(segment_path: pathlib.Path, index_path: pathlib.Path, verbose: bool) -> int:
    errors = 0
    with segment_path.open("rb") as sf:
        sh = parse_segment_header(sf)
        extradata = read_exact(sf, sh.extradata_size)
        first_record_offset = SEGMENT_HEADER.size + sh.extradata_size

        (idx_camera, idx_sequence, idx_start), entries = parse_index(index_path)
        if (idx_camera, idx_sequence, idx_start) != (sh.camera_hash, sh.sequence, sh.start_ns):
            print("ERROR: segment/index identity mismatch")
            errors += 1

        segment_size = segment_path.stat().st_size
        previous_ts: int | None = None
        keyframes = 0
        discontinuities = 0
        total_payload = 0

        for i, entry in enumerate(entries):
            if entry.file_offset < first_record_offset or entry.file_offset + PACKET_HEADER.size > segment_size:
                print(f"ERROR: index[{i}] file offset {entry.file_offset} is out of range")
                errors += 1
                continue

            sf.seek(entry.file_offset)
            raw = read_exact(sf, PACKET_HEADER.size)
            magic, payload_size, pkt_type, flags, reserved, ts, pts, dts, duration = PACKET_HEADER.unpack(raw)

            if magic != PACKET_MAGIC:
                print(f"ERROR: index[{i}] points to bad packet magic 0x{magic:08x}")
                errors += 1
            if pkt_type != PACKET_VIDEO:
                print(f"ERROR: index[{i}] points to packet type {pkt_type}, expected video")
                errors += 1
            if reserved != 0:
                print(f"warning: packet[{i}] reserved field is {reserved}", file=sys.stderr)
            if payload_size != entry.packet_size:
                print(f"ERROR: index[{i}] payload size {entry.packet_size} != record {payload_size}")
                errors += 1
            if ts != entry.timestamp_ns:
                print(f"ERROR: index[{i}] timestamp {entry.timestamp_ns} != record {ts}")
                errors += 1
            if bool(flags & PACKET_FLAG_KEYFRAME) != entry.keyframe:
                print(f"ERROR: index[{i}] keyframe flag mismatch")
                errors += 1
            if entry.file_offset + PACKET_HEADER.size + payload_size > segment_size:
                print(f"ERROR: packet[{i}] payload extends past EOF")
                errors += 1

            if previous_ts is not None and ts < previous_ts:
                print(f"ERROR: timestamp moved backwards at record {i}: {ts} < {previous_ts}")
                errors += 1
            previous_ts = ts
            keyframes += int(bool(flags & PACKET_FLAG_KEYFRAME))
            discontinuities += int(bool(flags & PACKET_FLAG_DISCONTINUITY))
            total_payload += payload_size

            if verbose:
                print(
                    f"{i:7d} ts={ts} offset={entry.file_offset} size={payload_size} "
                    f"key={int(bool(flags & PACKET_FLAG_KEYFRAME))} disc={int(bool(flags & PACKET_FLAG_DISCONTINUITY))} "
                    f"pts={pts} dts={dts} dur={duration}"
                )

    fps = sh.fps_num / sh.fps_den if sh.fps_den else 0.0
    if entries:
        duration_s = max(0.0, (entries[-1].timestamp_ns - entries[0].timestamp_ns) / 1e9)
    else:
        duration_s = 0.0

    print(f"segment:       {segment_path}")
    print(f"index:         {index_path}")
    print(f"format:        v{sh.version}")
    print(f"camera hash:   {sh.camera_hash:08x}")
    print(f"sequence:      {sh.sequence}")
    print(f"codec id:      {sh.codec_id}")
    print(f"video:         {sh.width}x{sh.height} @ {fps:.3f} fps")
    print(f"segment start: {sh.start_ns} ns")
    print(f"segment flags: 0x{sh.flags:08x} (discontinuity={int(bool(sh.flags & SEGMENT_FLAG_DISCONTINUITY))})")
    print(f"extradata:     {len(extradata)} bytes")
    print(f"indexed video: {len(entries)} packet(s)")
    print(f"keyframes:     {keyframes}")
    print(f"packet disc.:  {discontinuities}")
    print(f"indexed span:  {duration_s:.3f} s")
    print(f"video payload: {total_payload / (1024 * 1024):.2f} MiB")
    print(f"validation:    {'OK' if errors == 0 else f'{errors} ERROR(S)'}")
    return errors


def default_index_path(segment: pathlib.Path) -> pathlib.Path:
    name = segment.name
    if name.endswith(".srseg.part"):
        return segment.with_name(name[: -len(".srseg.part")] + ".sridx.part")
    if name.endswith(".srseg"):
        return segment.with_name(name[: -len(".srseg")] + ".sridx")
    die("segment path must end in .srseg or .srseg.part")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("segment", type=pathlib.Path, help=".srseg or .srseg.part path")
    parser.add_argument("--index", type=pathlib.Path, help="matching .sridx/.sridx.part; inferred by default")
    parser.add_argument("-v", "--verbose", action="store_true", help="print every indexed packet")
    args = parser.parse_args()

    segment = args.segment
    index = args.index or default_index_path(segment)
    if not segment.is_file():
        parser.error(f"segment not found: {segment}")
    if not index.is_file():
        parser.error(f"index not found: {index}")

    try:
        errors = validate_pair(segment, index, args.verbose)
    except (OSError, RuntimeError, struct.error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
