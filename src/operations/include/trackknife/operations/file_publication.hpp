// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

struct FilePublicationCommitResult {
    core::StableId journal_id;
    std::string source_raw_path;
    std::string target_raw_path;
    core::LocalSourceRevision source_revision;
    core::LocalSourceRevision target_revision;
    std::vector<std::size_t> occurrence_indexes;

    friend bool operator==(const FilePublicationCommitResult&,
                           const FilePublicationCommitResult&) = default;
};

using FilePublicationDependentStateCommitter =
    std::function<core::Result<void>(const FilePublicationCommitResult&)>;

// Executes one ready same-filesystem source preflight. The caller runs this
// synchronous operation on a bounded mutation worker. The dependent-state
// callback must be idempotent and all-or-nothing because recovery may replay
// it after a crash. Its failure restores the original path before returning.
[[nodiscard]] core::Result<FilePublicationCommitResult> commit_same_filesystem_publication(
    const OutputPathPreflight& preflight, std::size_t source_index, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

// Reverses one completed same-filesystem publication as a second journaled
// no-replace rename. The reverse record has its own dependent-state identity,
// is recovered by the normal publication recovery path, and makes repeated
// successful undo requests idempotent.
[[nodiscard]] core::Result<FilePublicationCommitResult> undo_same_filesystem_publication(
    const core::StableId& journal_id, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

enum class FilePublicationRecoveryOutcome : std::uint8_t {
    completed,
    rolled_back,
    needs_reconciliation,
};

struct FilePublicationRecoveryResult {
    core::StableId journal_id;
    FilePublicationRecoveryOutcome outcome{FilePublicationRecoveryOutcome::needs_reconciliation};
    std::optional<core::Error> issue;

    friend bool operator==(const FilePublicationRecoveryResult&,
                           const FilePublicationRecoveryResult&) = default;
};

// Recovers incomplete same-filesystem records. Cross-filesystem records are
// intentionally left for the separate copy-publication executor.
[[nodiscard]] core::Result<std::vector<FilePublicationRecoveryResult>>
recover_same_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
