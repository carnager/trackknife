// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstdint>
#include <string>

namespace trackknife::core {

// How a no-replace publication actually happened (ADR-0111): the preferred
// atomic rename, or the checked plain rename for filesystems that reject
// renameat2 flags (NFS, sshfs) — where the VFS has already resolved target
// absence, or this call checked it — whose tiny window between check and
// rename is the documented cost of supporting them.
enum class NoReplacePublishMethod : std::uint8_t {
    atomic_rename,
    checked_rename,
};

// Moves the file at (source_dir_fd, source_name) to (target_dir_fd,
// target_name) without ever replacing an existing target; an occupied
// target reports ErrorCode::conflict at every rung. Pass AT_FDCWD with
// absolute paths when no directory descriptors are held.
[[nodiscard]] Result<NoReplacePublishMethod> publish_no_replace_at(int source_dir_fd,
                                                                   const std::string& source_name,
                                                                   int target_dir_fd,
                                                                   const std::string& target_name);

} // namespace trackknife::core
