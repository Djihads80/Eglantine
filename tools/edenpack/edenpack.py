#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Create and inspect EdenPack v1 archives for extracted ExeFS + RomFS folders.

The format is deliberately uncompressed: files remain independently addressable
by offset, so the future native VFS backend can seek directly without extracting.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import BinaryIO, Iterable

MAGIC = b"EDENPK01"
VERSION = 1
HEADER = struct.Struct("<8sIIQQQ")
HEADER_SIZE = HEADER.size
ALIGNMENT = 4096
COPY_CHUNK = 1024 * 1024


class EdenPackError(Exception):
    pass


def _align(value: int, alignment: int = ALIGNMENT) -> int:
    return (value + alignment - 1) // alignment * alignment


def _safe_relative(path: Path, root: Path) -> str:
    rel = path.relative_to(root).as_posix()
    parsed = PurePosixPath(rel)
    if not rel or parsed.is_absolute() or any(part in ("", ".", "..") for part in parsed.parts):
        raise EdenPackError(f"unsafe relative path: {rel!r}")
    if "\x00" in rel:
        raise EdenPackError("NUL byte in path")
    return rel


def _walk_game_tree(root: Path) -> list[tuple[str, Path, bool]]:
    if not root.is_dir():
        raise EdenPackError(f"source is not a directory: {root}")
    for required in ("ExeFS", "RomFS"):
        if not (root / required).is_dir():
            raise EdenPackError(f"source must contain {required}/")

    found: list[tuple[str, Path, bool]] = [
        ("ExeFS", root / "ExeFS", True),
        ("RomFS", root / "RomFS", True),
    ]
    seen: set[str] = {"ExeFS", "RomFS"}
    for top in ("ExeFS", "RomFS"):
        base = root / top
        for current, dirs, files in os.walk(base, followlinks=False):
            current_path = Path(current)
            # Refuse symlinks rather than silently following paths outside the game root.
            for name in list(dirs):
                p = current_path / name
                if p.is_symlink():
                    raise EdenPackError(f"symlink is not supported: {p}")
            dirs.sort()
            for name in dirs:
                p = current_path / name
                rel = _safe_relative(p, root)
                if rel in seen:
                    raise EdenPackError(f"duplicate path: {rel}")
                seen.add(rel)
                found.append((rel, p, True))
            for name in sorted(files):
                p = current_path / name
                if p.is_symlink() or not p.is_file():
                    raise EdenPackError(f"non-regular file is not supported: {p}")
                rel = _safe_relative(p, root)
                if rel in seen:
                    raise EdenPackError(f"duplicate path: {rel}")
                seen.add(rel)
                found.append((rel, p, False))
    found.sort(key=lambda item: item[0].encode("utf-8"))
    return found


def _copy_and_hash(source: Path, output: BinaryIO) -> tuple[int, str]:
    digest = hashlib.sha256()
    total = 0
    with source.open("rb") as stream:
        while True:
            block = stream.read(COPY_CHUNK)
            if not block:
                break
            output.write(block)
            digest.update(block)
            total += len(block)
    return total, digest.hexdigest()


def pack(source: Path, destination: Path) -> dict:
    source = source.resolve()
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise EdenPackError("output archive must be outside the source game folder")

    entries = _walk_game_tree(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(prefix=destination.name + ".", suffix=".tmp",
                                           dir=destination.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    index_entries: list[dict] = []
    try:
        with temporary.open("w+b") as output:
            output.write(b"\0" * HEADER_SIZE)
            for rel, path, is_dir in entries:
                if is_dir:
                    index_entries.append({"path": rel, "type": "directory"})
                    continue
                aligned = _align(output.tell())
                output.write(b"\0" * (aligned - output.tell()))
                offset = output.tell()
                size, digest = _copy_and_hash(path, output)
                index_entries.append({
                    "path": rel, "type": "file", "offset": offset,
                    "size": size, "sha256": digest,
                })

            index_offset = _align(output.tell())
            output.write(b"\0" * (index_offset - output.tell()))
            index_payload = json.dumps(
                {"format": "EdenPack", "version": VERSION, "alignment": ALIGNMENT,
                 "entries": index_entries},
                ensure_ascii=False, separators=(",", ":"), sort_keys=True,
            ).encode("utf-8")
            output.write(index_payload)
            output.seek(0)
            output.write(HEADER.pack(MAGIC, VERSION, 0, index_offset,
                                     len(index_payload), len(index_entries)))
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(destination)
    except Exception:
        temporary.unlink(missing_ok=True)
        raise

    return {
        "archive": str(destination),
        "entries": len(index_entries),
        "files": sum(e["type"] == "file" for e in index_entries),
        "directories": sum(e["type"] == "directory" for e in index_entries),
        "source_bytes": sum(e.get("size", 0) for e in index_entries),
        "archive_bytes": destination.stat().st_size,
    }


def read_index(archive: Path) -> tuple[dict, int]:
    try:
        size = archive.stat().st_size
        with archive.open("rb") as stream:
            raw = stream.read(HEADER_SIZE)
            if len(raw) != HEADER_SIZE:
                raise EdenPackError("truncated header")
            magic, version, flags, index_offset, index_size, entry_count = HEADER.unpack(raw)
            if magic != MAGIC:
                raise EdenPackError("not an EdenPack archive")
            if version != VERSION:
                raise EdenPackError(f"unsupported EdenPack version: {version}")
            if flags != 0:
                raise EdenPackError(f"unsupported flags: {flags}")
            if index_offset < HEADER_SIZE or index_offset > size:
                raise EdenPackError("index offset is outside the archive")
            if index_size > size - index_offset:
                raise EdenPackError("index extends beyond end of archive")
            stream.seek(index_offset)
            payload = stream.read(index_size)
            if len(payload) != index_size:
                raise EdenPackError("truncated index")
        index = json.loads(payload.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, struct.error) as exc:
        if isinstance(exc, EdenPackError):
            raise
        raise EdenPackError(f"cannot read archive: {exc}") from exc

    if not isinstance(index, dict) or index.get("format") != "EdenPack" or index.get("version") != VERSION:
        raise EdenPackError("invalid index metadata")
    entries = index.get("entries")
    if not isinstance(entries, list) or len(entries) != entry_count:
        raise EdenPackError("index entry count does not match header")

    seen: set[str] = set()
    entry_types: dict[str, str] = {}
    ranges: list[tuple[int, int, str]] = []
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("type") not in ("file", "directory"):
            raise EdenPackError("invalid entry")
        path = entry.get("path")
        if not isinstance(path, str):
            raise EdenPackError("entry path must be a string")
        parsed = PurePosixPath(path)
        if (not path or parsed.is_absolute() or "\\" in path or
                any(part in ("", ".", "..") for part in parsed.parts) or "\x00" in path):
            raise EdenPackError(f"unsafe archive path: {path!r}")
        if not (path == "ExeFS" or path.startswith("ExeFS/") or
                path == "RomFS" or path.startswith("RomFS/")):
            raise EdenPackError(f"entry outside ExeFS/RomFS: {path}")
        if path in seen:
            raise EdenPackError(f"duplicate archive path: {path}")
        seen.add(path)
        entry_types[path] = entry["type"]
        if entry["type"] == "file":
            offset, length = entry.get("offset"), entry.get("size")
            digest = entry.get("sha256")
            if (not isinstance(offset, int) or isinstance(offset, bool) or
                    not isinstance(length, int) or isinstance(length, bool) or
                    offset < HEADER_SIZE or length < 0 or
                    offset > index_offset or length > index_offset - offset):
                raise EdenPackError(f"invalid data range for {path}")
            if not isinstance(digest, str) or len(digest) != 64:
                raise EdenPackError(f"invalid SHA-256 for {path}")
            ranges.append((offset, offset + length, path))

    # A path component declared as a file cannot also contain descendants.
    for path in seen:
        parts = path.split("/")
        for end in range(1, len(parts)):
            parent = "/".join(parts[:end])
            if entry_types.get(parent) == "file":
                raise EdenPackError(f"file path is also used as a directory: {parent}")

    if entry_types.get("ExeFS") != "directory" or entry_types.get("RomFS") != "directory":
        raise EdenPackError("ExeFS and RomFS must be directory entries")

    ranges.sort()
    previous_end = HEADER_SIZE
    for start, end, path in ranges:
        if start < previous_end:
            raise EdenPackError(f"overlapping file data at {path}")
        previous_end = max(previous_end, end)

    if not {"ExeFS", "RomFS"}.issubset(seen):
        raise EdenPackError("archive is missing ExeFS or RomFS root directory")
    return index, size


def verify(archive: Path, source: Path | None = None) -> dict:
    index, archive_size = read_index(archive)
    files = [e for e in index["entries"] if e["type"] == "file"]
    checked_bytes = 0
    with archive.open("rb") as stream:
        for entry in files:
            digest = hashlib.sha256()
            remaining = entry["size"]
            stream.seek(entry["offset"])
            while remaining:
                block = stream.read(min(COPY_CHUNK, remaining))
                if not block:
                    raise EdenPackError(f"truncated file data: {entry['path']}")
                digest.update(block)
                remaining -= len(block)
            if digest.hexdigest() != entry["sha256"]:
                raise EdenPackError(f"SHA-256 mismatch: {entry['path']}")
            checked_bytes += entry["size"]

    if source is not None:
        source = source.resolve()
        source_entries = _walk_game_tree(source)
        source_files = {rel: path for rel, path, is_dir in source_entries if not is_dir}
        archive_files = {e["path"]: e for e in files}
        if set(source_files) != set(archive_files):
            missing = sorted(set(source_files) - set(archive_files))
            extra = sorted(set(archive_files) - set(source_files))
            raise EdenPackError(f"source/archive file list differs; missing={missing[:5]}, extra={extra[:5]}")
        with archive.open("rb") as stream:
            for rel, path in source_files.items():
                entry = archive_files[rel]
                if path.stat().st_size != entry["size"]:
                    raise EdenPackError(f"size mismatch: {rel}")
                digest = hashlib.sha256()
                with path.open("rb") as src:
                    for block in iter(lambda: src.read(COPY_CHUNK), b""):
                        digest.update(block)
                if digest.hexdigest() != entry["sha256"]:
                    raise EdenPackError(f"source hash mismatch: {rel}")

    return {"valid": True, "files": len(files), "entries": len(index["entries"]),
            "verified_bytes": checked_bytes, "archive_bytes": archive_size,
            "source_compared": source is not None}


def list_entries(archive: Path) -> Iterable[dict]:
    index, _ = read_index(archive)
    return index["entries"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Pack extracted Eden ExeFS + RomFS folders")
    sub = parser.add_subparsers(dest="command", required=True)
    p_pack = sub.add_parser("pack", help="create an uncompressed .edenpack archive")
    p_pack.add_argument("source", type=Path, help="game base folder containing ExeFS/ and RomFS/")
    p_pack.add_argument("output", type=Path, help="output .edenpack file")
    p_verify = sub.add_parser("verify", help="verify archive structure and every file hash")
    p_verify.add_argument("archive", type=Path)
    p_verify.add_argument("--source", type=Path, help="also compare against original game folder")
    p_list = sub.add_parser("list", help="list indexed paths")
    p_list.add_argument("archive", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "pack":
            result = pack(args.source, args.output)
            print(json.dumps(result, indent=2))
        elif args.command == "verify":
            print(json.dumps(verify(args.archive, args.source), indent=2))
        else:
            for entry in list_entries(args.archive):
                if entry["type"] == "file":
                    print(f'{entry["size"]:>12}  {entry["path"]}')
                else:
                    print(f'{"<DIR>":>12}  {entry["path"]}/')
    except EdenPackError as exc:
        print(f"edenpack: error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
