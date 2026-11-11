# nxformat reader

Header-only reader for `.nx` archives, from [nxformat](https://github.com/fab918/nxformat)
by fab918 (MIT; see `LICENSE`). These headers are copied unchanged from Lichii PR
[#1078](https://github.com/zortos293/Lichii/pull/1078), head
`ad4a6d720904c8077cb6c9d9fd7b42df022777eb` (nxformat 1.0.1); they are unchanged in
nxformat 1.0.3.

The emulator integration ports Lichii PRs [#923](https://github.com/zortos293/Lichii/pull/923),
[#1078](https://github.com/zortos293/Lichii/pull/1078) and
[#1232](https://github.com/zortos293/Lichii/pull/1232).

Loading archives uses these vendored headers and the emulator's Zstandard library;
it requires no nxformat download. The Qt conversion tools separately use nxformat
1.0.3's `static_nozstd` library, downloaded with pinned SHA-256 digests by
`CMakeModules/NxFormat.cmake`. Set `EGLANTINE_NXFORMAT_ROOT` to an unpacked release
to use a local copy. When the download is unavailable, loading still works and only
the conversion tools are disabled. No nxformat shared library needs to be packaged.
