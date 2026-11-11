# NX test fixtures

Synthetic archives only; none contains game data.

## `lichii_synthetic.nx`

SHA-256 `05c238808353d7b47d55adc45337868d61bccc7ae61d8fe741990e5e0ce19607`. One Switch 2 application
(`0100123456780000`): base snapshot 0, update snapshot 1 (version 1, a complete tree that replaces
`romfs/shadowed.bin` and adds `romfs/added.bin`) and add-on snapshot 2 (`0100123456781001`).
`romfs/big.bin` is 17 MiB + 12345 bytes of a repeating 1021-byte pattern, stored as 1 MiB Zstandard
blocks; `romfs/random.bin` (160 KiB + 77 bytes) is incompressible and stored RAW.

Built with the packer's synthetic NUP writer and nxconvert from nxformat `V1.0.0`
(`ddf67861b58ec2d9611e80930670601a65c4a9c4`):

```sh
cargo build --release   # in a crate depending on nxformat/packer, with src/main.rs below
./target/release/nupgen2 lichii.nup
nxconvert pack-nup lichii.nup lichii_synthetic.nx --platform switch2 --no-trail --compressed-chunk-kib 0
```

```rust
use nxformat_packer::import::synthetic_nup::*;

fn fill(seed: u64, n: usize) -> Vec<u8> {
    let mut x = seed;
    (0..n)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            (x >> 32) as u8
        })
        .collect()
}

fn main() {
    let app = 0x0100_1234_5678_0000u64;
    let a: Vec<String> = std::env::args().collect();
    let period = fill(0x9e37_79b9_7f4a_7c15, 1021);
    let big: Vec<u8> = (0..(17usize << 20) + 12345).map(|i| period[i % period.len()]).collect();
    let random = fill(0x2545_f491_4f6c_dd1d, 160 * 1024 + 77);
    let mut b = base(app);
    let romfs = b.romfs.as_mut().unwrap();
    romfs.push(Node::File("big.bin", big));
    romfs.push(Node::File("random.bin", random));
    let pk = vec![b, update(app, 1, 0), addon(app)];
    std::fs::write(&a[1], build(&pk, false)).unwrap();
}
```

## `standalone_update.nx`

SHA-256 `3093e8789a3e208f1725e033fc5b4d42784d2de0acd15ccfbd4b8f68e86741ba`. `lichii_synthetic.nx` with a
standalone update (snapshot type 3, version 131072, no display version) added from a bare update
folder. The folder is the synthetic packer's `update(0x0100123456780000, 2, 0)` ExeFS and RomFS,
written out as `exefs/` and `romfs/` by a small Rust program, and added with nxconvert from the
nxformat `V1.0.0` release (`c70daf6f`):

```sh
nxconvert add-tree standalone_update.nx update2 --platform switch2 --update-version 131072 --no-trail
```

## Upstream conformance fixtures

Copied unchanged from nxformat `V1.0.0` `tests/fixtures/` (MIT, fab918); each file matches the
SHA-256 in upstream `fixture_index.json`. The expected result is upstream's.

| File | Expected | Upstream reason |
|---|---|---|
| minimal_valid.nx | accept | empty complete base snapshot |
| nested_vfs.nx | accept | root and child directory entries with independently addressable files |
| zstd_single.nx | accept | one independent Zstandard frame and full snapshot Build ID |
| shared_snapshots.nx | accept | two versioned complete roots share a tree, file and blocks |
| deleted_between_versions.nx | accept | later full tree omits a deleted file |
| multiple_programs.nx | accept | one app has two independent programs |
| localized_metadata_icons.nx | accept | localized title and separately addressable icon |
| jpeg_icon.nx | accept | on-demand JPEG icon retains its exact 285-byte synthetic JPEG payload |
| dlc_with_program.nx | accept | DLC may name a real source-provided executable Program ID |
| empty_files.nx | accept | zero-byte file has no block references |
| appended_generation.nx | accept | in-place edit: generation 1 tail after free space |
| interrupted_commit.nx | accept | torn newer slot falls back to generation 0; trailing residue ignored |
| lost_header_page_0.nx | accept | the other generation survives complete header-page damage |
| corrupted_manifest.nx | reject | Manifest digest mismatch |
| corrupted_hot_index.nx | reject | Hot-index checksum mismatch |
| truncated_archive.nx | reject | declared and actual archive lengths differ |
| invalid_offsets.nx | reject | Catalog out of bounds |
| invalid_path.nx | reject | unsafe path component |
| cyclic_directory.nx | reject | a directory Entry points back at the snapshot root |
| repeated_directory.nx | reject | two Entries of one snapshot reach the same directory |
| repeated_file.nx | reject | one File record appears at two paths of one snapshot |
| unknown_mandatory_feature.nx | reject | unknown required feature bit |
| truncated_header_area.nx | reject | file ends inside the second header page |
| slot_generation_conflict.nx | reject | both slots valid with generation 0 but different bytes |
| damaged_both_slots.nx | reject | no valid header slot |
| corrupted_block_payload.nx | reject_full_verify | RAW block differs from the original integrity digest |
| corrupted_orphan_block.nx | reject_full_verify | unreferenced physical block differs from its integrity digest |

## External-content regression fixtures

`native_external_base.nx` contains native decrypted NCAs for application
`0100123456780000` (base only). `native_embedded_update.nx` also includes
version 1. `external_update.nsp` supplies version 2 and `external_dlc.nsp`
supplies add-on `0100123456781002`. All contents are synthetic; there are no
keys, game files or executable game code. The update contains a complete
synthetic RomFS, not a BKTR delta. The test exercises the normal RomFS/ExeFS
update selection and disabling paths; it does not claim a real-game boot.

Generated by `generate_external_content.rs` using nxformat-packer at
`d0c45874d17ccae8343df4615ba2ffc7ea815a51` (MIT, fab918). To reproduce, copy
the generator to a scratch Rust crate's `src/main.rs` and use this dependency:

```toml
[dependencies]
nxformat-packer = { git = "https://github.com/fab918/nxformat", rev = "d0c45874d17ccae8343df4615ba2ffc7ea815a51" }
```

Run `cargo run --release -- /path/to/empty-output-directory`. Expected bytes:
base `data.bin`: 4096 × `0x11`; embedded update: 4096 × `0x22`; external update:
4096 × `0x33`; external `dlc.bin`: 8192 × `0x44`. Synthetic NSO build IDs use
the same marker as their corresponding RomFS.

| File | SHA-256 |
|---|---|
| `native_external_base.nx` | `4ab495a6d6963d27c8d7e6af059f1212efe2a377e8aa7db88fc856e848ddbb2c` |
| `native_embedded_update.nx` | `62ac32da3f1a86f49f067d742ea06990f61374476e9dc8b2d825a635b076e593` |
| `external_update.nsp` | `68a52ea169f60d9235c80f830036aa247481afc8a98a697e19eacfb938b7e588` |
| `external_dlc.nsp` | `b03a46462dff48888d6babffa381a6e6bc89757467b1e6a4207d5e7c8f117680` |
