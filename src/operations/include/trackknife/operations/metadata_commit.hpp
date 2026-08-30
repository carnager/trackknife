// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/metadata_journal.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace trackknife::operations {

struct MetadataCommitResult {
    core::StableId journal_id;
    std::string source_raw_path;
    std::string backup_raw_path;
    core::LocalSourceRevision previous_revision;
    core::LocalSourceRevision published_revision;
    metadata::MetadataDocument document;
    std::vector<std::size_t> occurrence_indexes;

    friend bool operator==(const MetadataCommitResult&, const MetadataCommitResult&) = default;
};

using MetadataDependentStateCommitter =
    std::function<core::Result<void>(const MetadataCommitResult&)>;

// Executes one ready native-FLAC source plan. The caller runs this synchronous
// operation on a bounded mutation worker. The dependent-state callback must be
// idempotent and all-or-nothing: recovery may replay it after a crash. Its
// success is part of commit; failure rolls the source back before returning.
[[nodiscard]] core::Result<MetadataCommitResult>
commit_flac_metadata_source(const metadata::MetadataWritePlanSource& source_plan,
                            MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation = {});

enum class MetadataRecoveryOutcome : std::uint8_t {
    completed,
    rolled_back,
    undone,
    needs_reconciliation,
};

struct MetadataRecoveryResult {
    core::StableId journal_id;
    MetadataRecoveryOutcome outcome{MetadataRecoveryOutcome::needs_reconciliation};
    std::optional<core::Error> issue;

    friend bool operator==(const MetadataRecoveryResult&, const MetadataRecoveryResult&) = default;
};

// Recovers every nonterminal metadata journal. Safe identities are either
// completed (including dependent-state replay) or rolled back; ambiguous files
// are retained and marked for reconciliation.
[[nodiscard]] core::Result<std::vector<MetadataRecoveryResult>>
recover_metadata_operations(MetadataOperationJournal& journal,
                            const MetadataDependentStateCommitter& dependent_state_committer,
                            const core::CancellationToken& cancellation = {});

// Restores the exact retained inode for one completed operation. Undo is a
// second journaled mutation with its own idempotency identity; a crash either
// resumes the restore or leaves explicit reconciliation evidence.
[[nodiscard]] core::Result<MetadataCommitResult>
undo_flac_metadata_operation(const core::StableId& journal_id, MetadataOperationJournal& journal,
                             const MetadataDependentStateCommitter& dependent_state_committer,
                             const core::CancellationToken& cancellation = {});

// Removes one verified retained backup without changing the published source.
// Missing executor-owned paths are treated as an idempotent completed release;
// unexpected identities become reconciliation evidence and are never deleted.
[[nodiscard]] core::Result<void>
release_metadata_backup(const core::StableId& journal_id, MetadataOperationJournal& journal,
                        const core::CancellationToken& cancellation = {});

struct MetadataBackupRetentionPolicy {
    // Backups are considered only during explicit maintenance (normally
    // startup), so a commit remains undoable for the rest of its first session.
    std::int64_t maximum_age_seconds{7 * 24 * 60 * 60};
    std::size_t maximum_entries{256U};
    std::uint64_t maximum_total_bytes{10U * 1024U * 1024U * 1024U};
};

enum class MetadataBackupMaintenanceOutcome : std::uint8_t {
    retained,
    released,
    needs_reconciliation,
};

struct MetadataBackupMaintenanceResult {
    core::StableId journal_id;
    MetadataBackupMaintenanceOutcome outcome{MetadataBackupMaintenanceOutcome::retained};
    std::optional<core::Error> issue;
};

// Retains only the newest undo for one raw source, then applies age, operation
// count, and old-inode byte budgets newest-first. Cleanup is identity-checked
// and reports per-record failures without deleting ambiguous evidence.
[[nodiscard]] core::Result<std::vector<MetadataBackupMaintenanceResult>> maintain_metadata_backups(
    MetadataOperationJournal& journal, const MetadataBackupRetentionPolicy& policy,
    std::int64_t now_unix_seconds, const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations
