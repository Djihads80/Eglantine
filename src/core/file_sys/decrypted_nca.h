// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {

/// A decrypted view of an NCA, read on demand with the console keys: plaintext header with the
/// magic DNCA, every FS encryption type set to None and the FS header hashes recomputed, and
/// plaintext sections. The view has the size of the NCA. An NCA that is already decrypted is
/// returned as is. On failure, returns nullptr and sets `error`.
VirtualFile OpenDecryptedNca(VirtualFile nca, std::string& error);

/// A decrypted NSP (DNSP) view of an NSP or XCI: a PFS0 of the package's files, or of the XCI
/// secure partition, with every NCA replaced by its decrypted view. Nothing is written to disk.
VirtualFile OpenDecryptedPackage(VirtualFile package, std::string& error);

/// A decrypted gamecard image (DXCI) view of an XCI: the same bytes, with the magic DXCI and
/// every NCA outside the update partition replaced by its decrypted view. The update partition
/// (the console firmware) is kept as it is. Nothing is written to disk.
VirtualFile OpenDecryptedGamecard(VirtualFile image, std::string& error);

/// A read-only PFS0 (NSP layout) holding `files` in order, without copying their bytes.
VirtualFile BuildPartitionFile(std::string name, const std::vector<VirtualFile>& files);

/// A decrypted add-on content NCA for `title_id` whose one section is `romfs`, without a hash
/// layer, for add-ons kept as an extracted tree. Nothing is copied.
VirtualFile BuildAddOnContentNca(u64 title_id, VirtualFile romfs);

} // namespace FileSys
