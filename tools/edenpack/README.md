# EdenPack prototype (v1)

EdenPack is an experimental, uncompressed, indexed container for extracted
Switch game folders containing both ExeFS/ and RomFS/. Its purpose is to
provide one SD-card file and a compact path-to-offset index for a future
native Eden VFS backend.

**This is a tooling prototype, not an Eden runtime feature yet.** The current
Android build cannot mount .edenpack directly. Existing loose-folder loading
is unchanged.

## Create an archive

    python tools/edenpack/edenpack.py pack "/path/to/Game" "/path/to/Game.edenpack"

The source folder must contain:

    Game/
    ├── ExeFS/
    └── RomFS/

The archive is uncompressed. Every file is independently addressable by
absolute offset and size. File data starts on 4096-byte boundaries. The index
stores relative paths, file sizes, offsets, and SHA-256 digests. Directory
entries are retained as well.

## Verify

Verify the archive structure and every file's SHA-256:

    python tools/edenpack/edenpack.py verify "/path/to/Game.edenpack"

Also compare the packed file list, sizes and hashes with the original tree:

    python tools/edenpack/edenpack.py verify "/path/to/Game.edenpack" --source "/path/to/Game"

List paths:

    python tools/edenpack/edenpack.py list "/path/to/Game.edenpack"

## Format v1

All integer header fields are little-endian.

| Field | Type | Description |
|---|---|---|
| Magic | 8 bytes | EDENPK01 |
| Version | u32 | 1 |
| Flags | u32 | Must be zero |
| Index offset | u64 | Absolute archive offset |
| Index size | u64 | JSON index length |
| Entry count | u64 | Number of index entries |

The index is UTF-8 JSON. File entries contain path, type=file, offset,
size, and sha256. Directory entries contain path and type=directory. The
format intentionally does not compress data, allowing random reads without
decompressing a stream.

## Tests

    python -m unittest discover -s tools/edenpack -v

## Next integration stage

The native backend should implement read-only VfsFile and VfsDirectory
objects over a single seekable file descriptor. It must validate all index
ranges before exposing entries, use bounded descriptor and metadata caches,
and preserve the path and enumeration semantics expected by Eden's ExeFS and
RomFS loaders. It should not extract the archive during mounting.
