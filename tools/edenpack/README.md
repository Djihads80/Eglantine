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

## Benchmark loose folders vs EdenPack

After creating an archive, run the benchmark against the **same extracted source
folder and archive**, ideally both stored on the SD card you want to evaluate:

    python tools/edenpack/benchmark_edenpack.py "/path/to/Game" "/path/to/Game.edenpack"

It first measures loose-tree enumeration and EdenPack index-load time, then runs
a deterministic set of random reads against both layouts. Defaults are 500 reads
of up to 64 KiB each. Increase the workload for a more stable measurement:

    python tools/edenpack/benchmark_edenpack.py "/path/to/Game" "/path/to/Game.edenpack" --operations 3000 --chunk-kib 64

Run it several times and compare medians. The operating system's file cache can
make later runs substantially faster, so do not interpret one run as a cold-SD
measurement. Keep the source and archive on the same storage device. The output
includes read latency (median and p95), throughput, tree-scan time, and index-load
time.

**Important:** this benchmark compares ordinary filesystem access with the
EdenPack file layout. It does not measure Android SAF, Eden's native VFS, or
time-to-first-frame. Those require the later Android/native mounting integration
and an on-device benchmark.
