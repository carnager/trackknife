// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/staged_patch.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class MetadataOperationJournalState : std::uint8_t {
    planned,
    prepared,
    published,
    complete,
    rolled_back,
    needs_reconciliation,
};

// The file-mutation journal reaches `complete` before its exact old inode can
// be offered as undo. Backup lifecycle is deliberately separate: commit
// recovery never mistakes a retained undo artifact for an incomplete write.
enum class MetadataOperationBackupState : std::uint8_t {
    retained,
    undoing,
    undone,
    released,
    needs_reconciliation,
};

struct MetadataOperationJournalChange {
    std::size_t field_index{0U};
    std::string canonical_name;
    std::string property_name;
    bool original_present{false};
    std::vector<std::string> original_values;
    metadata::StagedMetadataPatchKind kind{metadata::StagedMetadataPatchKind::replace_values};
    std::vector<std::string> planned_values;
    std::vector<std::size_t> item_indexes;

    friend bool operator==(const MetadataOperationJournalChange&,
                           const MetadataOperationJournalChange&) = default;
};

struct MetadataOperationJournalRecord {
    core::StableId id;
    MetadataOperationJournalState state{MetadataOperationJournalState::planned};
    std::string source_raw_path;
    std::string prepared_raw_path;
    std::string backup_raw_path;
    core::LocalSourceRevision expected_revision;
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> published_revision;
    std::vector<std::size_t> occurrence_indexes;
    std::vector<MetadataOperationJournalChange> changes;
    std::optional<core::Error> failure;

    friend bool operator==(const MetadataOperationJournalRecord&,
                           const MetadataOperationJournalRecord&) = default;
};

struct MetadataOperationJournalTransition {
    MetadataOperationJournalState expected_state{MetadataOperationJournalState::planned};
    MetadataOperationJournalState state{MetadataOperationJournalState::planned};
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> published_revision;
    std::optional<core::Error> failure;
};

struct MetadataOperationBackupRecord {
    MetadataOperationJournalRecord operation;
    MetadataOperationBackupState state{MetadataOperationBackupState::retained};
    std::optional<core::StableId> undo_id;
    std::int64_t completed_at_unix_seconds{0};
    std::int64_t updated_at_unix_seconds{0};
    std::optional<core::Error> failure;

    friend bool operator==(const MetadataOperationBackupRecord&,
                           const MetadataOperationBackupRecord&) = default;
};

struct MetadataOperationBackupTransition {
    MetadataOperationBackupState expected_state{MetadataOperationBackupState::retained};
    MetadataOperationBackupState state{MetadataOperationBackupState::retained};
    std::optional<core::StableId> undo_id;
    std::optional<core::Error> failure;
};

class MetadataOperationJournal {
  public:
    MetadataOperationJournal() = default;
    MetadataOperationJournal(MetadataOperationJournal&&) noexcept = default;
    MetadataOperationJournal& operator=(MetadataOperationJournal&&) noexcept = default;
    MetadataOperationJournal(const MetadataOperationJournal&) = delete;
    MetadataOperationJournal& operator=(const MetadataOperationJournal&) = delete;
    virtual ~MetadataOperationJournal() = default;

    [[nodiscard]] virtual core::Result<void>
    create(const MetadataOperationJournalRecord& record) = 0;
    [[nodiscard]] virtual core::Result<void>
    transition(const core::StableId& id, const MetadataOperationJournalTransition& transition) = 0;
    [[nodiscard]] virtual core::Result<std::optional<MetadataOperationJournalRecord>>
    load(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<MetadataOperationJournalRecord>>
    load_incomplete() const = 0;
    [[nodiscard]] virtual core::Result<std::optional<MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<MetadataOperationBackupRecord>>
    load_backups() const = 0;
    [[nodiscard]] virtual core::Result<void>
    transition_backup(const core::StableId& id,
                      const MetadataOperationBackupTransition& transition) = 0;
};

} // namespace trackknife::operations
