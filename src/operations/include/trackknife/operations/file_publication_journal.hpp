// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class FilePublicationJournalState : std::uint8_t {
    planned = 0,
    target_prepared = 1,
    target_published = 2,
    dependent_state_committed = 3,
    source_removed = 4,
    complete = 5,
    rolled_back = 6,
    needs_reconciliation = 7,
};

struct FilePublicationJournalRecord {
    core::StableId id;
    FilePublicationJournalState state{FilePublicationJournalState::planned};
    OutputPathPublicationKind publication{OutputPathPublicationKind::same_filesystem_rename};
    std::string source_raw_path;
    std::string target_raw_path;
    // Empty for same-filesystem rename. Cross-filesystem publication uses the
    // exact executor-owned sibling returned by file_publication_prepared_path.
    std::string prepared_raw_path;
    core::LocalSourceRevision expected_source_revision;
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> target_revision;
    std::vector<std::size_t> occurrence_indexes;
    std::vector<std::string> planned_missing_directory_raw_paths;
    std::optional<core::Error> failure;

    friend bool operator==(const FilePublicationJournalRecord&,
                           const FilePublicationJournalRecord&) = default;
};

struct FilePublicationJournalTransition {
    FilePublicationJournalState expected_state{FilePublicationJournalState::planned};
    FilePublicationJournalState state{FilePublicationJournalState::planned};
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> target_revision;
    std::optional<core::Error> failure;
};

[[nodiscard]] std::filesystem::path
file_publication_prepared_path(const std::filesystem::path& target,
                               const core::StableId& journal_id);

[[nodiscard]] core::Result<FilePublicationJournalRecord>
make_file_publication_journal_record(const OutputPathPreflight& preflight, std::size_t source_index,
                                     const core::StableId& journal_id);

class FilePublicationJournal {
  public:
    FilePublicationJournal() = default;
    FilePublicationJournal(FilePublicationJournal&&) noexcept = default;
    FilePublicationJournal& operator=(FilePublicationJournal&&) noexcept = default;
    FilePublicationJournal(const FilePublicationJournal&) = delete;
    FilePublicationJournal& operator=(const FilePublicationJournal&) = delete;
    virtual ~FilePublicationJournal() = default;

    [[nodiscard]] virtual core::Result<void> create(const FilePublicationJournalRecord& record) = 0;
    [[nodiscard]] virtual core::Result<void>
    transition(const core::StableId& id, const FilePublicationJournalTransition& transition) = 0;
    [[nodiscard]] virtual core::Result<std::optional<FilePublicationJournalRecord>>
    load(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<FilePublicationJournalRecord>>
    load_incomplete() const = 0;
};

} // namespace trackknife::operations
