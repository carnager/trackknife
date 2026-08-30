// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace operations = trackknife::operations;
namespace persistence = trackknife::persistence;
using State = operations::FilePublicationJournalState;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-file-publication-executor-" + core::StableId::random().to_string())} {
        require(std::filesystem::create_directory(path_),
                "temporary publication directory must be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(stream.good(), "publication fixture must be written");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

operations::OutputPathPreflight preflight(const std::filesystem::path& source,
                                          const std::filesystem::path& target) {
    auto revision = core::observe_local_source_revision(source.native());
    require(revision.has_value(), "publication source revision must be observable");
    operations::PlannedOutputPathSource planned{
        .source_raw_path = source.native(),
        .source_revision = *revision,
        .target_raw_path = target.native(),
        .raw_relative_directory = {},
        .sanitized_relative_directory = {},
        .raw_basename = target.stem().native(),
        .sanitized_basename = target.stem().native(),
        .item_indexes = {2U, 7U},
        .sanitized = false,
        .no_change = false,
    };
    operations::OutputPathPlan plan{
        .layout = {},
        .destination = std::nullopt,
        .operations = {.rename_files = true, .move_files = false},
        .sources = {planned},
        .issues = {},
    };
    auto checked = operations::preflight_output_paths(plan);
    if (!checked) {
        std::cerr << checked.error().message << '\n';
    }
    require(checked.has_value() && checked->ready(),
            "publication fixture must pass fresh preflight");
    require(checked->sources.front().publication ==
                operations::OutputPathPublicationKind::same_filesystem_rename,
            "publication fixture must be a same-filesystem rename");
    return std::move(*checked);
}

persistence::SqliteFilePublicationJournal open_journal(const TemporaryDirectory& directory,
                                                       const std::string_view name) {
    auto opened = persistence::SqliteFilePublicationJournal::open(directory.path() / name);
    if (!opened) {
        std::cerr << opened.error().message << '\n';
    }
    require(opened.has_value(), "file-publication journal must open");
    return std::move(*opened);
}

const operations::FilePublicationDependentStateCommitter successful_dependent_commit =
    [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; };

class FailingOnceTransitionJournal final : public operations::FilePublicationJournal {
  public:
    FailingOnceTransitionJournal(operations::FilePublicationJournal& journal,
                                 const State failed_state)
        : journal_{journal}, failed_state_{failed_state} {}

    core::Result<void> create(const operations::FilePublicationJournalRecord& record) override {
        return journal_.create(record);
    }

    core::Result<void>
    transition(const core::StableId& id,
               const operations::FilePublicationJournalTransition& requested) override {
        if (!failed_ && requested.state == failed_state_) {
            failed_ = true;
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected publication transition",
                                               .context = {}});
        }
        return journal_.transition(id, requested);
    }

    core::Result<std::optional<operations::FilePublicationJournalRecord>>
    load(const core::StableId& id) const override {
        return journal_.load(id);
    }

    core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_incomplete() const override {
        return journal_.load_incomplete();
    }

  private:
    operations::FilePublicationJournal& journal_;
    State failed_state_;
    bool failed_{false};
};

void commitsAtomicRenameAndCreatesOnlyPlannedDirectories() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "Artist" / "Album" / "track.flac";
    write_file(source, "exact audio bytes");
    const auto checked = preflight(source, target);
    require(!std::filesystem::exists(target.parent_path()),
            "preflight must not create target directories");
    auto journal = open_journal(directory, "success.sqlite3");
    std::size_t callback_count = 0U;
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.source_raw_path == source.native() &&
                        result.target_raw_path == target.native() &&
                        result.source_revision == result.target_revision &&
                        result.occurrence_indexes == (std::vector<std::size_t>{2U, 7U}),
                    "dependent commit must receive exact publication evidence");
            require(!std::filesystem::exists(source) && read_file(target) == "exact audio bytes",
                    "dependent commit must run after physical publication");
            return {};
        });
    if (!committed) {
        std::cerr << committed.error().message << '\n';
    }
    require(committed.has_value() && callback_count == 1U,
            "same-filesystem publication must commit once");
    auto record = journal.load(committed->journal_id);
    require(record && *record && (**record).state == State::complete &&
                (**record).target_revision == committed->target_revision,
            "successful publication must retain complete target evidence");
}

void dependentFailureRestoresSourceAndLeavesHarmlessDirectories() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "new" / "tree" / "track.flac";
    write_file(source, "rollback bytes");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "dependent-failure.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected dependent failure",
                                               .context = {}});
        });
    require(!committed && committed.error().code == core::ErrorCode::database,
            "dependent-state failure must fail publication");
    require(read_file(source) == "rollback bytes" && !std::filesystem::exists(target),
            "dependent-state failure must restore the original path");
    require(std::filesystem::is_directory(target.parent_path()),
            "rollback must leave potentially shared empty directories intact");
    const auto incomplete = journal.load_incomplete();
    require(incomplete && incomplete->empty(),
            "rolled-back dependent failure must not replay at startup");
}

void journalFailureAfterRenameAlsoRestoresSource() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "journal rollback");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "journal-failure.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::target_published};
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, failing, successful_dependent_commit);
    require(!committed && committed.error().code == core::ErrorCode::database,
            "injected publication-journal failure must surface");
    require(read_file(source) == "journal rollback" && !std::filesystem::exists(target),
            "publication-journal failure must roll the physical rename back");
    require(durable.load_incomplete()->empty(), "journal-transition rollback must become terminal");
}

void dependentJournalFailureReplaysIdempotentStateCommit() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "dependent replay");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "dependent-journal-failure.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::dependent_state_committed};
    std::size_t callback_count = 0U;
    const auto callback =
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
        ++callback_count;
        return {};
    };
    const auto committed =
        operations::commit_same_filesystem_publication(checked, 0U, failing, callback);
    require(!committed && committed.error().code == core::ErrorCode::database &&
                callback_count == 1U && !std::filesystem::exists(source) &&
                read_file(target) == "dependent replay",
            "journal failure after state commit must retain the published target");
    const auto incomplete = durable.load_incomplete();
    require(incomplete && incomplete->size() == 1U &&
                incomplete->front().state == State::target_published,
            "failed dependent-state transition must remain replayable");
    const auto recovered = operations::recover_same_filesystem_publications(durable, callback);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 2U,
            "recovery must replay an idempotent dependent-state commit and complete");
}

void changedSourceOrAppearedTargetFailsBeforeJournalCreation() {
    TemporaryDirectory directory;
    const auto changed_source = directory.path() / "changed.flac";
    const auto changed_target = directory.path() / "changed-target.flac";
    write_file(changed_source, "before");
    const auto changed = preflight(changed_source, changed_target);
    write_file(changed_source, "different size after preflight");
    auto changed_journal = open_journal(directory, "changed.sqlite3");
    const auto changed_result = operations::commit_same_filesystem_publication(
        changed, 0U, changed_journal, successful_dependent_commit);
    require(!changed_result && changed_result.error().code == core::ErrorCode::conflict &&
                changed_journal.load_incomplete()->empty(),
            "changed source must fail before journal creation");

    const auto occupied_source = directory.path() / "occupied.flac";
    const auto occupied_target = directory.path() / "occupied-target.flac";
    write_file(occupied_source, "source");
    const auto occupied = preflight(occupied_source, occupied_target);
    write_file(occupied_target, "intruder");
    auto occupied_journal = open_journal(directory, "occupied.sqlite3");
    const auto occupied_result = operations::commit_same_filesystem_publication(
        occupied, 0U, occupied_journal, successful_dependent_commit);
    require(!occupied_result && occupied_result.error().code == core::ErrorCode::conflict &&
                occupied_journal.load_incomplete()->empty() &&
                read_file(occupied_source) == "source" && read_file(occupied_target) == "intruder",
            "appeared target must fail before journal creation without replacement");
}

void recoversBothSidesOfTheAtomicJournalBoundary() {
    TemporaryDirectory directory;
    const auto unstarted_source = directory.path() / "unstarted.flac";
    const auto unstarted_target = directory.path() / "unstarted-target.flac";
    write_file(unstarted_source, "unstarted");
    const auto unstarted_preflight = preflight(unstarted_source, unstarted_target);
    auto unstarted_journal = open_journal(directory, "unstarted.sqlite3");
    auto unstarted_record = operations::make_file_publication_journal_record(
        unstarted_preflight, 0U, core::StableId::random());
    require(unstarted_record && unstarted_journal.create(*unstarted_record),
            "unstarted recovery record must be durable");
    const auto unstarted = operations::recover_same_filesystem_publications(
        unstarted_journal, successful_dependent_commit);
    require(
        unstarted && unstarted->size() == 1U &&
            unstarted->front().outcome == operations::FilePublicationRecoveryOutcome::rolled_back &&
            std::filesystem::exists(unstarted_source) && !std::filesystem::exists(unstarted_target),
        "planned journal with original source must recover as rolled back");

    const auto published_source = directory.path() / "published.flac";
    const auto published_target = directory.path() / "published-target.flac";
    write_file(published_source, "published");
    const auto published_preflight = preflight(published_source, published_target);
    auto published_journal = open_journal(directory, "published.sqlite3");
    auto published_record = operations::make_file_publication_journal_record(
        published_preflight, 0U, core::StableId::random());
    require(published_record && published_journal.create(*published_record),
            "interrupted publication record must be durable");
    std::filesystem::rename(published_source, published_target);
    require(!std::filesystem::exists(published_source) && std::filesystem::exists(published_target),
            "fixture must interrupt after atomic rename");
    std::size_t callback_count = 0U;
    const auto recovered = operations::recover_same_filesystem_publications(
        published_journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.journal_id == published_record->id &&
                        result.target_raw_path == published_target.native(),
                    "recovery must replay exact dependent-state evidence");
            return {};
        });
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    require(recovered && recovered->size() == 1U && callback_count == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                !std::filesystem::exists(published_source) &&
                read_file(published_target) == "published",
            "rename before its journal transition must recover to completion");
    const auto durable = published_journal.load(published_record->id);
    require(durable && *durable && (**durable).state == State::complete,
            "recovered rename must durably complete");
}

void recoveryRollbackAndPostDependentCompletionAreExact() {
    TemporaryDirectory directory;
    const auto failed_source = directory.path() / "failed.flac";
    const auto failed_target = directory.path() / "failed-target.flac";
    write_file(failed_source, "recovery rollback");
    const auto failed_preflight = preflight(failed_source, failed_target);
    auto failed_journal = open_journal(directory, "recovery-rollback.sqlite3");
    auto failed_record = operations::make_file_publication_journal_record(failed_preflight, 0U,
                                                                          core::StableId::random());
    require(failed_record && failed_journal.create(*failed_record),
            "recovery rollback record must be durable");
    std::filesystem::rename(failed_source, failed_target);
    const auto failed = operations::recover_same_filesystem_publications(
        failed_journal, [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "recovery dependent failure",
                                               .context = {}});
        });
    require(failed && failed->size() == 1U &&
                failed->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::rolled_back &&
                read_file(failed_source) == "recovery rollback" &&
                !std::filesystem::exists(failed_target),
            "recovery callback failure must safely restore the original path");

    const auto committed_source = directory.path() / "committed.flac";
    const auto committed_target = directory.path() / "committed-target.flac";
    write_file(committed_source, "already dependent");
    const auto committed_preflight = preflight(committed_source, committed_target);
    auto committed_journal = open_journal(directory, "already-dependent.sqlite3");
    auto committed_record = operations::make_file_publication_journal_record(
        committed_preflight, 0U, core::StableId::random());
    require(committed_record && committed_journal.create(*committed_record),
            "post-dependent recovery record must be durable");
    std::filesystem::rename(committed_source, committed_target);
    require(
        committed_journal.transition(committed_record->id,
                                     {.expected_state = State::planned,
                                      .state = State::target_published,
                                      .prepared_revision = std::nullopt,
                                      .target_revision = committed_record->expected_source_revision,
                                      .failure = std::nullopt}) &&
            committed_journal.transition(
                committed_record->id,
                {.expected_state = State::target_published,
                 .state = State::dependent_state_committed,
                 .prepared_revision = std::nullopt,
                 .target_revision = committed_record->expected_source_revision,
                 .failure = std::nullopt}),
        "post-dependent fixture must reach its durable boundary");
    std::size_t callback_count = 0U;
    const auto completed = operations::recover_same_filesystem_publications(
        committed_journal,
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(completed && completed->size() == 1U && callback_count == 0U &&
                completed->front().outcome == operations::FilePublicationRecoveryOutcome::completed,
            "durable dependent-state boundary must complete without replaying its callback");
}

void ambiguousRecoveryNeverDeletesEitherPath() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "source bytes");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "ambiguous.sqlite3");
    auto record =
        operations::make_file_publication_journal_record(checked, 0U, core::StableId::random());
    require(record && journal.create(*record), "ambiguous record must be durable");
    write_file(target, "unrelated target");
    const auto recovered =
        operations::recover_same_filesystem_publications(journal, successful_dependent_commit);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::needs_reconciliation &&
                read_file(source) == "source bytes" && read_file(target) == "unrelated target",
            "ambiguous recovery must preserve both filesystem entries");
    const auto durable = journal.load(record->id);
    require(durable && *durable && (**durable).state == State::needs_reconciliation &&
                (**durable).failure,
            "ambiguous recovery must remain visible with failure evidence");
}

void realPublicationCommitsTheAllOccurrenceRelocationTransaction() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "nested" / "target.flac";
    write_file(source, "physical relocation bytes");
    const auto checked = preflight(source, target);
    const auto source_revision = checked.sources.front().observed_revision;
    const auto state_path = directory.path() / "state.sqlite3";
    auto repository_result = persistence::ListRepository::open(state_path);
    require(repository_result.has_value(), "relocation integration repository must open");
    auto repository = std::move(*repository_result);
    const std::vector documents{
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "First",
            .pinned = false,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source.native(),
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_revision,
                .fields = {{"title", "First"}},
            }},
        },
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = "Duplicate",
            .pinned = true,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source.native(),
                .logical_reference = std::string{"cue-v1\0track", 12U},
                .segment = persistence::ListItemSegment{.start_sample = 0, .end_sample = 1},
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_revision,
                .fields = {{"title", "Logical"}},
            }},
        },
    };
    require(repository.replace_all(documents).has_value(),
            "relocation integration occurrences must persist");
    auto journal = open_journal(directory, "state.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            auto relocated = repository.relocate_local_source(persistence::LocalSourceRelocation{
                .operation_id = result.journal_id,
                .source_reference = result.source_raw_path,
                .target_reference = result.target_raw_path,
                .previous_revision = result.source_revision,
                .published_revision = result.target_revision,
            });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        });
    require(committed.has_value() && !std::filesystem::exists(source) &&
                read_file(target) == "physical relocation bytes",
            "physical publication and dependent state must complete together");
    auto loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == target.native() &&
                (*loaded)[1].items[0].source_reference == target.native() &&
                (*loaded)[0].items[0].source_revision == committed->target_revision,
            "the real executor callback must re-key every persisted occurrence");
    require(repository.replace_all(documents).has_value(),
            "a pre-publication workspace snapshot may arrive after commit");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == target.native() &&
                (*loaded)[1].items[0].source_reference == target.native(),
            "publication relocation history must suppress stale source paths");
}

void cancellationBeforeCommitCreatesNoJournal() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "cancelled");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "cancelled.sqlite3");
    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal, successful_dependent_commit, cancellation.token());
    require(!committed && committed.error().code == core::ErrorCode::cancelled &&
                journal.load_incomplete()->empty() && std::filesystem::exists(source) &&
                !std::filesystem::exists(target),
            "pre-commit cancellation must not create journal or filesystem mutations");
}

} // namespace

int main() {
    commitsAtomicRenameAndCreatesOnlyPlannedDirectories();
    dependentFailureRestoresSourceAndLeavesHarmlessDirectories();
    journalFailureAfterRenameAlsoRestoresSource();
    dependentJournalFailureReplaysIdempotentStateCommit();
    changedSourceOrAppearedTargetFailsBeforeJournalCreation();
    recoversBothSidesOfTheAtomicJournalBoundary();
    recoveryRollbackAndPostDependentCompletionAreExact();
    ambiguousRecoveryNeverDeletesEitherPath();
    realPublicationCommitsTheAllOccurrenceRelocationTransaction();
    cancellationBeforeCommitCreatesNoJournal();
    std::cout << "file publication executor tests passed\n";
    return 0;
}
