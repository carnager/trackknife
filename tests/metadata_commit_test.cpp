// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/artwork_apply.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/operation_journal.hpp"

#include <flacfile.h>
#include <flacpicture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace metadata = trackknife::metadata;
namespace operations = trackknife::operations;
namespace persistence = trackknife::persistence;
using State = operations::MetadataOperationJournalState;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-metadata-commit-" + core::StableId::random().to_string())} {
        std::error_code error;
        CHECK(std::filesystem::create_directory(path_, error));
        CHECK(!error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        CHECK(!error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    for (std::size_t index = 0U; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::vector<unsigned char> decoded;
    unsigned accumulator = 0U;
    unsigned bits = 0U;
    for (const auto character : encoded) {
        if (character == '=') {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        const auto value = values[byte];
        if (value < 0) {
            if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
                continue;
            }
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
        }
    }
    return decoded;
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::string_view fixture_name,
                                                const std::filesystem::path& destination) {
    const auto bytes = decode_base64_file(fixture_directory / fixture_name);
    CHECK(bytes.has_value());
    if (bytes) {
        std::ofstream output{destination, std::ios::binary};
        output.write(reinterpret_cast<const char*>(bytes->data()),
                     static_cast<std::streamsize>(bytes->size()));
        CHECK(output.good());
    }
    return destination;
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::filesystem::path& destination) {
    return materialize(fixture_directory, "rich-metadata-flac.b64", destination);
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void add_secondary_picture(const std::filesystem::path& path,
                           const std::vector<unsigned char>& encoded) {
    TagLib::FLAC::File file{path.c_str(), false};
    CHECK(file.isValid());
    if (!file.isValid()) {
        return;
    }
    auto* picture = new TagLib::FLAC::Picture;
    picture->setType(TagLib::FLAC::Picture::BackCover);
    picture->setMimeType(TagLib::String{"image/jpeg", TagLib::String::UTF8});
    picture->setDescription(TagLib::String{"Secondary", TagLib::String::UTF8});
    picture->setWidth(8);
    picture->setHeight(6);
    picture->setColorDepth(24);
    picture->setNumColors(0);
    picture->setData(TagLib::ByteVector{reinterpret_cast<const char*>(encoded.data()),
                                        static_cast<unsigned int>(encoded.size())});
    file.addPicture(picture);
    CHECK(file.save());
}

[[nodiscard]] std::optional<metadata::MetadataWritePlanSource>
title_plan(const std::filesystem::path& source, const std::string& title) {
    auto read = metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return std::nullopt;
    }
    auto selection = metadata::StagedMetadataSelection::create({
        metadata::StagedMetadataSource{
            .raw_path = read->raw_path,
            .source_revision = read->source_revision,
            .baseline = read->document,
        },
    });
    CHECK(selection.has_value());
    if (!selection) {
        return std::nullopt;
    }
    const auto title_index = selection->field_index("TITLE");
    CHECK(title_index.has_value());
    if (!title_index) {
        return std::nullopt;
    }
    metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title_index, {title}).has_value());
    auto plan = metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready() && plan->sources.size() == 1U);
    if (!plan || !plan->ready() || plan->sources.size() != 1U) {
        return std::nullopt;
    }
    return std::move(plan->sources.front());
}

[[nodiscard]] std::optional<metadata::ArtworkWritePlanSource>
replacement_artwork_plan(const std::filesystem::path& source,
                         const std::filesystem::path& replacement,
                         const std::vector<std::size_t>& occurrences) {
    auto policy = metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    const auto inventory = metadata::read_local_artwork_inventory(source.native(), policy);
    CHECK(inventory && !inventory->items.empty() && !occurrences.empty());
    if (!inventory || inventory->items.empty() || occurrences.empty()) {
        return std::nullopt;
    }
    std::vector<metadata::ArtworkWritePlanIntent> intents;
    intents.reserve(occurrences.size());
    for (const auto occurrence : occurrences) {
        intents.push_back(metadata::ArtworkWritePlanIntent{
            .occurrence_index = occurrence,
            .raw_media_path = source.native(),
            .expected_media_revision = inventory->media_revision,
            .target_ordinal = 0U,
            .expected_target_fingerprint = inventory->items.front().content_fingerprint,
            .kind = metadata::ArtworkWritePlanIntentKind::replace,
            .replacement_raw_path = replacement.native(),
            .added_role = metadata::ArtworkRole::front,
            .added_description = {},
            .replacement_embedded_source = std::nullopt,
        });
    }
    auto plan = metadata::revalidate_artwork_write_plan(intents);
    CHECK(plan && plan->ready() && plan->sources.size() == 1U);
    return plan && plan->ready() && plan->sources.size() == 1U
               ? std::optional{std::move(plan->sources.front())}
               : std::nullopt;
}

[[nodiscard]] std::optional<operations::OutputPathPreflight>
destination_preflight(const std::filesystem::path& source, const std::filesystem::path& target) {
    auto revision = core::observe_local_source_revision(source.native());
    CHECK(revision.has_value());
    if (!revision) {
        return std::nullopt;
    }
    operations::OutputPathPlan plan{
        .layout = {},
        .destination = std::nullopt,
        .operations = {.rename_files = true, .move_files = false},
        .sources = {operations::PlannedOutputPathSource{
            .source_raw_path = source.native(),
            .source_revision = *revision,
            .target_raw_path = target.native(),
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = target.stem().native(),
            .sanitized_basename = target.stem().native(),
            .item_indexes = {0U},
            .sanitized = false,
            .no_change = false,
        }},
        .issues = {},
    };
    auto checked = operations::preflight_output_paths(plan);
    CHECK(checked.has_value() && checked->ready() && checked->sources.size() == 1U);
    return checked && checked->ready() ? std::optional{std::move(*checked)} : std::nullopt;
}

[[nodiscard]] std::optional<persistence::SqliteMetadataOperationJournal>
open_journal(const TemporaryDirectory& directory, const std::string_view name) {
    auto journal = persistence::SqliteMetadataOperationJournal::open(directory.path() / name);
    CHECK(journal.has_value());
    return journal ? std::optional{std::move(*journal)} : std::nullopt;
}

class CapturingJournal final : public operations::MetadataOperationJournal {
  public:
    explicit CapturingJournal(operations::MetadataOperationJournal& delegate)
        : delegate_{delegate} {}

    core::Result<void> create(const operations::MetadataOperationJournalRecord& record) override {
        created_id = record.id;
        return delegate_.create(record);
    }

    core::Result<void>
    transition(const core::StableId& id,
               const operations::MetadataOperationJournalTransition& transition) override {
        return delegate_.transition(id, transition);
    }

    core::Result<std::optional<operations::MetadataOperationJournalRecord>>
    load(const core::StableId& id) const override {
        return delegate_.load(id);
    }

    core::Result<std::vector<operations::MetadataOperationJournalRecord>>
    load_incomplete() const override {
        return delegate_.load_incomplete();
    }

    core::Result<std::optional<operations::MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const override {
        return delegate_.load_backup(id);
    }

    core::Result<std::vector<operations::MetadataOperationBackupRecord>>
    load_backups() const override {
        return delegate_.load_backups();
    }

    core::Result<void>
    transition_backup(const core::StableId& id,
                      const operations::MetadataOperationBackupTransition& transition) override {
        return delegate_.transition_backup(id, transition);
    }

    std::optional<core::StableId> created_id;

  private:
    operations::MetadataOperationJournal& delegate_;
};

class FailingPublishedTransitionJournal final : public operations::MetadataOperationJournal {
  public:
    explicit FailingPublishedTransitionJournal(operations::MetadataOperationJournal& delegate)
        : delegate_{delegate} {}

    core::Result<void> create(const operations::MetadataOperationJournalRecord& record) override {
        created_id = record.id;
        return delegate_.create(record);
    }

    core::Result<void>
    transition(const core::StableId& id,
               const operations::MetadataOperationJournalTransition& transition) override {
        if (!failed_ && transition.state == State::published) {
            failed_ = true;
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "injected published-transition failure",
                .context = {},
            });
        }
        return delegate_.transition(id, transition);
    }

    core::Result<std::optional<operations::MetadataOperationJournalRecord>>
    load(const core::StableId& id) const override {
        return delegate_.load(id);
    }

    core::Result<std::vector<operations::MetadataOperationJournalRecord>>
    load_incomplete() const override {
        return delegate_.load_incomplete();
    }

    core::Result<std::optional<operations::MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const override {
        return delegate_.load_backup(id);
    }

    core::Result<std::vector<operations::MetadataOperationBackupRecord>>
    load_backups() const override {
        return delegate_.load_backups();
    }

    core::Result<void>
    transition_backup(const core::StableId& id,
                      const operations::MetadataOperationBackupTransition& transition) override {
        return delegate_.transition_backup(id, transition);
    }

    std::optional<core::StableId> created_id;

  private:
    operations::MetadataOperationJournal& delegate_;
    bool failed_{false};
};

[[nodiscard]] core::Result<void>
successful_dependent_commit(const operations::MetadataCommitResult&) {
    return {};
}

void commits_atomically_and_retains_verified_backup(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "success.flac");
    CHECK(::chmod(source.c_str(), 0640) == 0);
    constexpr std::string_view attribute_name{"user.trackknife.metadata-commit-test"};
    constexpr std::string_view attribute_value{"preserved-xattr"};
    const bool xattrs_supported =
        ::setxattr(source.c_str(), attribute_name.data(), attribute_value.data(),
                   attribute_value.size(), 0) == 0;
    if (!xattrs_supported) {
        CHECK(errno == ENOTSUP || errno == EOPNOTSUPP);
    }
    const auto original_bytes = read_bytes(source);
    auto plan = title_plan(source, "Published title");
    auto journal = open_journal(directory, "success.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    std::size_t callback_count = 0U;
    const auto committed = operations::commit_flac_metadata_source(
        *plan, *journal,
        [&callback_count](const operations::MetadataCommitResult& result) -> core::Result<void> {
            ++callback_count;
            if (result.document.effective_values("title") !=
                std::vector<std::string>{"Published title"}) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invariant,
                    .message = "dependent commit received an unverified document",
                    .context = {},
                });
            }
            return {};
        });
    if (!committed) {
        std::cerr << committed.error().message << '\n';
    }
    CHECK(committed.has_value());
    CHECK(callback_count == 1U);
    if (!committed) {
        return;
    }
    CHECK(committed->occurrence_indexes == plan->occurrence_indexes);
    CHECK(read_bytes(source) != original_bytes);
    CHECK(read_bytes(committed->backup_raw_path) == original_bytes);
    CHECK(plan->observed_revision && committed->previous_revision == *plan->observed_revision);
    const auto reread = metadata::read_local_metadata(source.native());
    const auto backup = metadata::read_local_metadata(committed->backup_raw_path);
    CHECK(reread.has_value() && reread->document.effective_values("title") ==
                                    std::vector<std::string>{"Published title"});
    CHECK(backup.has_value() &&
          backup->document.effective_values("title") == plan->changes.front().original_values);
    struct stat status{};
    CHECK(::stat(source.c_str(), &status) == 0);
    CHECK((status.st_mode & 07777) == 0640);
    if (xattrs_supported) {
        std::array<char, 64> value{};
        const auto size =
            ::getxattr(source.c_str(), attribute_name.data(), value.data(), value.size());
        CHECK(size == static_cast<ssize_t>(attribute_value.size()));
        CHECK((size >= 0 &&
               std::string_view{value.data(), static_cast<std::size_t>(size)} == attribute_value));
    }
    const auto record = journal->load(committed->journal_id);
    CHECK(record.has_value() && record->has_value() && (**record).state == State::complete);
    const auto incomplete = journal->load_incomplete();
    CHECK(incomplete.has_value() && incomplete->empty());
}

void rolls_back_dependent_and_journal_failures(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;

    const auto dependent_source =
        materialize(fixture_directory, directory.path() / "dependent-failure.flac");
    const auto dependent_original = read_bytes(dependent_source);
    auto dependent_plan = title_plan(dependent_source, "Must roll back");
    auto dependent_journal = open_journal(directory, "dependent.sqlite3");
    CHECK(dependent_plan.has_value() && dependent_journal.has_value());
    if (!dependent_plan || !dependent_journal) {
        return;
    }
    CapturingJournal capturing{*dependent_journal};
    const auto failed = operations::commit_flac_metadata_source(
        *dependent_plan, capturing,
        [](const operations::MetadataCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "injected dependent-state failure",
                .context = {},
            });
        });
    CHECK(!failed && failed.error().code == core::ErrorCode::database);
    CHECK(read_bytes(dependent_source) == dependent_original);
    CHECK(capturing.created_id.has_value());
    if (capturing.created_id) {
        const auto record = dependent_journal->load(*capturing.created_id);
        CHECK(record.has_value() && record->has_value() && (**record).state == State::rolled_back);
        CHECK(record && *record && !std::filesystem::exists((**record).backup_raw_path));
        CHECK(record && *record && !std::filesystem::exists((**record).prepared_raw_path));
    }

    const auto journal_source =
        materialize(fixture_directory, directory.path() / "journal-failure.flac");
    const auto journal_original = read_bytes(journal_source);
    auto journal_plan = title_plan(journal_source, "Also rolls back");
    auto journal_store = open_journal(directory, "journal.sqlite3");
    CHECK(journal_plan.has_value() && journal_store.has_value());
    if (!journal_plan || !journal_store) {
        return;
    }
    FailingPublishedTransitionJournal injected{*journal_store};
    const auto journal_failed = operations::commit_flac_metadata_source(
        *journal_plan, injected, successful_dependent_commit);
    CHECK(!journal_failed && journal_failed.error().code == core::ErrorCode::database);
    CHECK(read_bytes(journal_source) == journal_original);
    CHECK(injected.created_id.has_value());
    if (injected.created_id) {
        const auto record = journal_store->load(*injected.created_id);
        CHECK(record.has_value() && record->has_value() && (**record).state == State::rolled_back);
    }
}

void preserves_ambiguous_external_changes_for_reconciliation(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "callback-race.flac");
    auto plan = title_plan(source, "Published before external race");
    auto journal = open_journal(directory, "callback-race.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    CapturingJournal capturing{*journal};
    const auto raced = operations::commit_flac_metadata_source(
        *plan, capturing, [](const operations::MetadataCommitResult& result) -> core::Result<void> {
            std::ofstream changed{result.source_raw_path, std::ios::binary | std::ios::app};
            changed.put('\0');
            return changed.good() ? core::Result<void>{}
                                  : std::unexpected(core::Error{
                                        .code = core::ErrorCode::io,
                                        .message = "could not inject external source change",
                                        .context = {},
                                    });
        });
    CHECK(!raced && raced.error().code == core::ErrorCode::conflict);
    CHECK(capturing.created_id.has_value());
    if (capturing.created_id) {
        const auto record = journal->load(*capturing.created_id);
        CHECK(record.has_value() && record->has_value() &&
              (**record).state == State::needs_reconciliation);
        CHECK(record && *record && std::filesystem::exists((**record).backup_raw_path));
    }
}

void serializes_sources_and_honors_cancellation(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "serialized.flac");
    auto plan = title_plan(source, "First publication");
    auto journal = open_journal(directory, "serialized.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }

    std::promise<void> callback_entered_promise;
    auto callback_entered = callback_entered_promise.get_future();
    std::promise<void> callback_release_promise;
    auto callback_release = callback_release_promise.get_future().share();
    std::optional<core::Result<operations::MetadataCommitResult>> first_result;
    std::thread first{[&] {
        first_result = operations::commit_flac_metadata_source(
            *plan, *journal, [&](const operations::MetadataCommitResult&) -> core::Result<void> {
                callback_entered_promise.set_value();
                callback_release.wait();
                return {};
            });
    }};
    callback_entered.wait();

    core::CancellationSource cancellation;
    std::promise<void> second_started_promise;
    auto second_started = second_started_promise.get_future();
    std::optional<core::Result<operations::MetadataCommitResult>> second_result;
    std::thread second{[&] {
        second_started_promise.set_value();
        second_result = operations::commit_flac_metadata_source(
            *plan, *journal, successful_dependent_commit, cancellation.token());
    }};
    second_started.wait();
    cancellation.request_cancellation();
    second.join();
    callback_release_promise.set_value();
    first.join();

    CHECK(first_result.has_value() && first_result->has_value());
    CHECK(second_result.has_value() && !second_result->has_value() &&
          second_result->error().code == core::ErrorCode::cancelled);
}

void rejects_hard_linked_sources_before_journaling(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "linked.flac");
    auto plan = title_plan(source, "Blocked title");
    auto journal = open_journal(directory, "linked.sqlite3");
    const auto alias = directory.path() / "linked-alias.flac";
    CHECK(::link(source.c_str(), alias.c_str()) == 0);
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    const auto committed =
        operations::commit_flac_metadata_source(*plan, *journal, successful_dependent_commit);
    CHECK(!committed && committed.error().code == core::ErrorCode::unsupported);
    const auto incomplete = journal->load_incomplete();
    CHECK(incomplete.has_value() && incomplete->empty());
}

[[nodiscard]] operations::MetadataOperationJournalRecord
interrupted_record(const metadata::MetadataWritePlanSource& plan, const core::StableId& id) {
    const auto parent = std::filesystem::path{plan.raw_path}.parent_path();
    const auto stem = ".trackknife-" + id.to_string() + ".metadata-";
    const auto& change = plan.changes.front();
    std::vector<std::size_t> item_indexes;
    item_indexes.reserve(change.intents.size());
    for (const auto& intent : change.intents) {
        item_indexes.push_back(intent.item_index);
    }
    return {
        .id = id,
        .state = State::planned,
        .source_raw_path = plan.raw_path,
        .prepared_raw_path = (parent / (stem + "prepared")).native(),
        .backup_raw_path = (parent / (stem + "backup")).native(),
        .expected_revision = *plan.observed_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = plan.occurrence_indexes,
        .content_kind = operations::MetadataOperationContentKind::text_fields,
        .changes =
            {
                operations::MetadataOperationJournalChange{
                    .field_index = change.field_index,
                    .canonical_name = change.canonical_name,
                    .property_name = "TITLE",
                    .original_present = change.original_present,
                    .original_values = change.original_values,
                    .kind = change.intents.front().kind,
                    .planned_values = change.intents.front().values,
                    .item_indexes = std::move(item_indexes),
                    .exact_native_name = change.exact_native_name,
                },
            },
        .artwork = std::nullopt,
        .failure = std::nullopt,
    };
}

void recovers_publication_interrupted_before_journal_transition(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "recovery.flac");
    auto plan = title_plan(source, "Recovered title");
    auto journal = open_journal(directory, "recovery.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    auto record = interrupted_record(*plan, core::StableId::random());
    CHECK(journal->create(record).has_value());
    const auto prepared =
        metadata::prepare_flac_metadata_write_copy(*plan, record.prepared_raw_path);
    CHECK(prepared.has_value());
    if (!prepared) {
        return;
    }
    record.prepared_revision = prepared->prepared_revision;
    CHECK(journal
              ->transition(record.id,
                           operations::MetadataOperationJournalTransition{
                               .expected_state = State::planned,
                               .state = State::prepared,
                               .prepared_revision = record.prepared_revision,
                               .published_revision = std::nullopt,
                               .failure = std::nullopt,
                           })
              .has_value());
    CHECK(::link(record.source_raw_path.c_str(), record.backup_raw_path.c_str()) == 0);
    CHECK(::rename(record.prepared_raw_path.c_str(), record.source_raw_path.c_str()) == 0);

    std::size_t callback_count = 0U;
    const auto recovered = operations::recover_metadata_operations(
        *journal,
        [&callback_count](const operations::MetadataCommitResult& result) -> core::Result<void> {
            ++callback_count;
            return result.document.effective_values("title") ==
                           std::vector<std::string>{"Recovered title"}
                       ? core::Result<void>{}
                       : std::unexpected(core::Error{
                             .code = core::ErrorCode::invariant,
                             .message = "recovery received the wrong document",
                             .context = {},
                         });
        });
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    CHECK(recovered.has_value() && recovered->size() == 1U);
    CHECK(recovered &&
          recovered->front().outcome == operations::MetadataRecoveryOutcome::completed);
    CHECK(callback_count == 1U);
    const auto loaded = journal->load(record.id);
    CHECK(loaded.has_value() && loaded->has_value() && (**loaded).state == State::complete);
    CHECK(std::filesystem::exists(record.backup_raw_path));
}

void commits_and_recovers_artwork_at_unchanged_paths(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    auto policy = metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();

    const auto committed_source = materialize(fixture_directory, "art-tone-flac.b64",
                                              directory.path() / "artwork-commit.flac");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "artwork-replacement.jpg");
    const auto replacement_bytes = read_bytes(replacement);
    add_secondary_picture(committed_source, replacement_bytes);
    const auto original_bytes = read_bytes(committed_source);
    const auto baseline_inventory =
        metadata::read_local_artwork_inventory(committed_source.native(), policy);
    CHECK(baseline_inventory && baseline_inventory->items.size() == 2U);
    auto commit_plan = replacement_artwork_plan(committed_source, replacement, {2U, 7U});
    const auto database_path = directory.path() / "artwork.sqlite3";
    auto journal = open_journal(directory, "artwork.sqlite3");
    CHECK(commit_plan && journal);
    if (!commit_plan || !journal) {
        return;
    }
    std::size_t refreshed_occurrences = 0U;
    const auto committed = operations::commit_flac_artwork_source(
        *commit_plan, *journal,
        [&refreshed_occurrences](
            const operations::MetadataCommitResult& result) -> core::Result<void> {
            refreshed_occurrences = result.occurrence_indexes.size();
            return result.occurrence_indexes == std::vector<std::size_t>{2U, 7U}
                       ? core::Result<void>{}
                       : std::unexpected(core::Error{
                             .code = core::ErrorCode::invariant,
                             .message = "artwork commit did not refresh every occurrence",
                             .context = {},
                         });
        });
    if (!committed) {
        std::cerr << committed.error().message << '\n';
    }
    CHECK(committed && refreshed_occurrences == 2U &&
          committed->previous_revision != committed->published_revision);
    const auto committed_inventory =
        metadata::read_local_artwork_inventory(committed_source.native(), policy);
    CHECK(committed_inventory && committed_inventory->items.size() == 2U &&
          committed_inventory->items.front().content_fingerprint ==
              commit_plan->change.replacement->content_fingerprint);
    const auto durable_record =
        committed ? journal->load(committed->journal_id)
                  : core::Result<std::optional<operations::MetadataOperationJournalRecord>>{
                        std::optional<operations::MetadataOperationJournalRecord>{}};
    CHECK(durable_record && *durable_record &&
          (**durable_record).content_kind ==
              operations::MetadataOperationContentKind::embedded_artwork &&
          (**durable_record).changes.empty() && (**durable_record).artwork &&
          std::filesystem::exists((**durable_record).backup_raw_path) &&
          read_bytes((**durable_record).backup_raw_path) == original_bytes);
    const auto undone =
        committed ? operations::undo_flac_metadata_operation(
                        committed->journal_id, *journal,
                        [](const operations::MetadataCommitResult& result) -> core::Result<void> {
                            return result.occurrence_indexes == std::vector<std::size_t>{2U, 7U}
                                       ? core::Result<void>{}
                                       : std::unexpected(core::Error{
                                             .code = core::ErrorCode::invariant,
                                             .message = "artwork undo missed an occurrence",
                                             .context = {},
                                         });
                        })
                  : core::Result<operations::MetadataCommitResult>{std::unexpected(core::Error{
                        .code = core::ErrorCode::invariant,
                        .message = "artwork commit was unavailable for undo",
                        .context = {},
                    })};
    CHECK(undone && read_bytes(committed_source) == original_bytes);

    auto remove_plan = *commit_plan;
    remove_plan.change.kind = metadata::ArtworkWritePlanIntentKind::remove;
    remove_plan.change.replacement.reset();
    const auto removed = operations::commit_flac_artwork_source(
        remove_plan, *journal,
        [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; });
    const auto removed_inventory =
        metadata::read_local_artwork_inventory(committed_source.native(), policy);
    const auto removed_record =
        removed ? journal->load(removed->journal_id)
                : core::Result<std::optional<operations::MetadataOperationJournalRecord>>{
                      std::optional<operations::MetadataOperationJournalRecord>{}};
    CHECK(removed && baseline_inventory && removed_inventory &&
          removed_inventory->items.size() == 1U &&
          removed_inventory->items.front().content_fingerprint ==
              baseline_inventory->items[1].content_fingerprint &&
          removed_record && *removed_record && (**removed_record).artwork &&
          (**removed_record).artwork->kind == metadata::ArtworkWritePlanIntentKind::remove &&
          !(**removed_record).artwork->replacement_fingerprint &&
          (**removed_record).artwork->planned_item_count == 1U);
    const auto remove_undone =
        removed
            ? operations::undo_flac_metadata_operation(
                  removed->journal_id, *journal,
                  [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; })
            : core::Result<operations::MetadataCommitResult>{std::unexpected(core::Error{
                  .code = core::ErrorCode::invariant,
                  .message = "artwork removal was unavailable for undo",
                  .context = {},
              })};
    CHECK(remove_undone && read_bytes(committed_source) == original_bytes);

    const auto add_source = materialize(fixture_directory, "tagged-tone-flac.b64",
                                        directory.path() / "artwork-add.flac");
    const auto add_original_bytes = read_bytes(add_source);
    const auto add_inventory = metadata::read_local_artwork_inventory(add_source.native(), policy);
    CHECK(add_inventory && add_inventory->items.empty());
    if (!add_inventory) {
        return;
    }
    const metadata::ArtworkWritePlanIntent add_intent{
        .occurrence_index = 13U,
        .raw_media_path = add_source.native(),
        .expected_media_revision = add_inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = {},
        .kind = metadata::ArtworkWritePlanIntentKind::add,
        .replacement_raw_path = replacement.native(),
        .added_role = metadata::ArtworkRole::front,
        .added_description = "Added cover",
        .replacement_embedded_source = std::nullopt,
    };
    const auto add_plan = metadata::revalidate_artwork_write_plan({add_intent});
    CHECK(add_plan && add_plan->ready() && add_plan->sources.size() == 1U);
    if (!add_plan || !add_plan->ready() || add_plan->sources.size() != 1U) {
        return;
    }
    const auto added = operations::commit_flac_artwork_source(
        add_plan->sources.front(), *journal,
        [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; });
    const auto added_inventory =
        metadata::read_local_artwork_inventory(add_source.native(), policy);
    const auto added_record =
        added ? journal->load(added->journal_id)
              : core::Result<std::optional<operations::MetadataOperationJournalRecord>>{
                    std::optional<operations::MetadataOperationJournalRecord>{}};
    CHECK(added && added_inventory && added_inventory->items.size() == 1U &&
          added_inventory->items.front().role == metadata::ArtworkRole::front &&
          added_inventory->items.front().description == "Added cover" && added_record &&
          *added_record && (**added_record).artwork &&
          (**added_record).artwork->kind == metadata::ArtworkWritePlanIntentKind::add &&
          !(**added_record).artwork->original_target_fingerprint &&
          (**added_record).artwork->replacement_fingerprint &&
          (**added_record).artwork->original_item_count == 0U &&
          (**added_record).artwork->planned_item_count == 1U);
    const auto add_undone =
        added
            ? operations::undo_flac_metadata_operation(
                  added->journal_id, *journal,
                  [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; })
            : core::Result<operations::MetadataCommitResult>{std::unexpected(core::Error{
                  .code = core::ErrorCode::invariant,
                  .message = "artwork Add was unavailable for undo",
                  .context = {},
              })};
    CHECK(add_undone && read_bytes(add_source) == add_original_bytes);

    const auto recovery_source = materialize(fixture_directory, "art-tone-flac.b64",
                                             directory.path() / "artwork-recovery.flac");
    add_secondary_picture(recovery_source, replacement_bytes);
    auto recovery_plan = replacement_artwork_plan(recovery_source, replacement, {3U, 9U});
    CHECK(recovery_plan.has_value());
    if (!recovery_plan) {
        return;
    }
    const auto original_inventory =
        metadata::read_local_artwork_inventory(recovery_source.native(), policy);
    CHECK(original_inventory && original_inventory->items.size() == 2U);
    if (!original_inventory || original_inventory->items.size() != 2U) {
        return;
    }
    auto planned_items = original_inventory->items;
    const auto& replacement_evidence = *recovery_plan->change.replacement;
    planned_items.front().mime_type = replacement_evidence.mime_type;
    planned_items.front().width = replacement_evidence.width;
    planned_items.front().height = replacement_evidence.height;
    planned_items.front().byte_size = replacement_evidence.byte_size;
    planned_items.front().content_fingerprint = replacement_evidence.content_fingerprint;
    planned_items.front().duplicate_of.reset();
    const auto original_fingerprint =
        metadata::fingerprint_embedded_artwork_inventory(original_inventory->items);
    const auto planned_fingerprint =
        metadata::fingerprint_embedded_artwork_inventory(planned_items);
    CHECK(original_fingerprint && planned_fingerprint);
    if (!original_fingerprint || !planned_fingerprint) {
        return;
    }
    const auto recovery_id = core::StableId::random();
    const auto parent = recovery_source.parent_path();
    const auto stem = ".trackknife-" + recovery_id.to_string() + ".metadata-";
    operations::MetadataOperationJournalRecord record{
        .id = recovery_id,
        .state = State::planned,
        .source_raw_path = recovery_source.native(),
        .prepared_raw_path = (parent / (stem + "prepared")).native(),
        .backup_raw_path = (parent / (stem + "backup")).native(),
        .expected_revision = *recovery_plan->observed_media_revision,
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = recovery_plan->occurrence_indexes,
        .content_kind = operations::MetadataOperationContentKind::embedded_artwork,
        .changes = {},
        .artwork =
            operations::MetadataOperationJournalArtwork{
                .kind = metadata::ArtworkWritePlanIntentKind::replace,
                .target_ordinal = 0U,
                .original_item_count = 2U,
                .planned_item_count = 2U,
                .original_target_fingerprint =
                    original_inventory->items.front().content_fingerprint,
                .replacement_fingerprint = replacement_evidence.content_fingerprint,
                .original_inventory_fingerprint = *original_fingerprint,
                .planned_inventory_fingerprint = *planned_fingerprint,
            },
        .failure = std::nullopt,
    };
    CHECK(journal->create(record).has_value());
    const auto prepared =
        metadata::prepare_flac_artwork_write_copy(*recovery_plan, record.prepared_raw_path);
    CHECK(prepared.has_value());
    if (!prepared) {
        return;
    }
    record.prepared_revision = prepared->prepared_revision;
    CHECK(journal
              ->transition(record.id,
                           operations::MetadataOperationJournalTransition{
                               .expected_state = State::planned,
                               .state = State::prepared,
                               .prepared_revision = record.prepared_revision,
                               .published_revision = std::nullopt,
                               .failure = std::nullopt,
                           })
              .has_value());
    CHECK(::link(record.source_raw_path.c_str(), record.backup_raw_path.c_str()) == 0);
    CHECK(::rename(record.prepared_raw_path.c_str(), record.source_raw_path.c_str()) == 0);
    journal.reset();

    auto restarted = persistence::SqliteMetadataOperationJournal::open(database_path);
    CHECK(restarted.has_value());
    if (!restarted) {
        return;
    }
    std::vector<std::size_t> recovered_occurrences;
    const auto recovered = operations::recover_metadata_operations(
        *restarted,
        [&recovered_occurrences](
            const operations::MetadataCommitResult& result) -> core::Result<void> {
            recovered_occurrences = result.occurrence_indexes;
            return {};
        });
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    CHECK(recovered && recovered->size() == 1U &&
          recovered->front().outcome == operations::MetadataRecoveryOutcome::completed &&
          recovered_occurrences == std::vector<std::size_t>({3U, 9U}));
    const auto recovered_record = restarted->load(record.id);
    CHECK(recovered_record && *recovered_record && (**recovered_record).state == State::complete &&
          (**recovered_record).published_revision == prepared->prepared_revision);
}

void recovers_safe_prepublication_debris_but_retains_ambiguous_paths(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;

    const auto clean_source =
        materialize(fixture_directory, directory.path() / "recover-clean.flac");
    auto clean_plan = title_plan(clean_source, "Unused clean plan");
    auto clean_journal = open_journal(directory, "recover-clean.sqlite3");
    CHECK(clean_plan.has_value() && clean_journal.has_value());
    if (!clean_plan || !clean_journal) {
        return;
    }
    const auto clean_record = interrupted_record(*clean_plan, core::StableId::random());
    CHECK(clean_journal->create(clean_record).has_value());
    CHECK(std::filesystem::copy_file(clean_source, clean_record.prepared_raw_path));
    const auto cleaned =
        operations::recover_metadata_operations(*clean_journal, successful_dependent_commit);
    CHECK(cleaned.has_value() && cleaned->size() == 1U &&
          cleaned->front().outcome == operations::MetadataRecoveryOutcome::rolled_back);
    CHECK(!std::filesystem::exists(clean_record.prepared_raw_path));
    const auto cleaned_record = clean_journal->load(clean_record.id);
    CHECK(cleaned_record.has_value() && cleaned_record->has_value() &&
          (**cleaned_record).state == State::rolled_back);

    const auto ambiguous_source =
        materialize(fixture_directory, directory.path() / "recover-ambiguous.flac");
    auto ambiguous_plan = title_plan(ambiguous_source, "Unused ambiguous plan");
    auto ambiguous_journal = open_journal(directory, "recover-ambiguous.sqlite3");
    CHECK(ambiguous_plan.has_value() && ambiguous_journal.has_value());
    if (!ambiguous_plan || !ambiguous_journal) {
        return;
    }
    const auto ambiguous_record = interrupted_record(*ambiguous_plan, core::StableId::random());
    CHECK(ambiguous_journal->create(ambiguous_record).has_value());
    CHECK(std::filesystem::copy_file(ambiguous_source, ambiguous_record.prepared_raw_path));
    {
        std::ofstream modified{ambiguous_source, std::ios::binary | std::ios::app};
        modified.put('\0');
        CHECK(modified.good());
    }
    const auto ambiguous =
        operations::recover_metadata_operations(*ambiguous_journal, successful_dependent_commit);
    CHECK(ambiguous.has_value() && ambiguous->size() == 1U &&
          ambiguous->front().outcome == operations::MetadataRecoveryOutcome::needs_reconciliation);
    CHECK(std::filesystem::exists(ambiguous_record.prepared_raw_path));
    const auto retained_record = ambiguous_journal->load(ambiguous_record.id);
    CHECK(retained_record.has_value() && retained_record->has_value() &&
          (**retained_record).state == State::needs_reconciliation);
}

void undoes_completed_metadata_and_recovers_interrupted_undo(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "undo.flac");
    const auto original_bytes = read_bytes(source);
    auto plan = title_plan(source, "Undo this title");
    auto journal = open_journal(directory, "undo.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    auto committed =
        operations::commit_flac_metadata_source(*plan, *journal, successful_dependent_commit);
    CHECK(committed.has_value());
    if (!committed) {
        return;
    }
    std::size_t undo_callback_count = 0U;
    const auto undone = operations::undo_flac_metadata_operation(
        committed->journal_id, *journal,
        [&](const operations::MetadataCommitResult& result) -> core::Result<void> {
            ++undo_callback_count;
            CHECK(result.journal_id != committed->journal_id);
            CHECK(result.previous_revision == committed->published_revision);
            CHECK(result.published_revision == committed->previous_revision);
            CHECK(result.document.effective_values("title") ==
                  plan->changes.front().original_values);
            return {};
        });
    // Undo needs RENAME_EXCHANGE; filesystems without it (NFS) report the
    // deliberate typed unavailability instead of succeeding (ADR-0111).
    if (!undone && undone.error().code == trackknife::core::ErrorCode::unsupported) {
        std::cerr << "undo unavailable on this filesystem: " << undone.error().message << '\n';
        return;
    }
    if (!undone) {
        std::cerr << undone.error().message << '\n';
    }
    CHECK(undone.has_value());
    CHECK(undo_callback_count == 1U);
    if (!undone) {
        return;
    }
    CHECK(read_bytes(source) == original_bytes);
    CHECK(!std::filesystem::exists(committed->backup_raw_path));
    const auto lifecycle = journal->load_backup(committed->journal_id);
    CHECK(lifecycle && *lifecycle &&
          (**lifecycle).state == operations::MetadataOperationBackupState::undone &&
          (**lifecycle).undo_id == undone->journal_id);

    const auto recovery_source =
        materialize(fixture_directory, directory.path() / "undo-recovery.flac");
    const auto recovery_original = read_bytes(recovery_source);
    auto recovery_plan = title_plan(recovery_source, "Interrupted undo title");
    CHECK(recovery_plan.has_value());
    if (!recovery_plan) {
        return;
    }
    auto recovery_commit = operations::commit_flac_metadata_source(*recovery_plan, *journal,
                                                                   successful_dependent_commit);
    CHECK(recovery_commit.has_value());
    if (!recovery_commit) {
        return;
    }
    auto recovery_record = journal->load(recovery_commit->journal_id);
    CHECK(recovery_record && *recovery_record);
    if (!recovery_record || !*recovery_record) {
        return;
    }
    const auto undo_id = core::StableId::random();
    CHECK(journal
              ->transition_backup(
                  recovery_commit->journal_id,
                  operations::MetadataOperationBackupTransition{
                      .expected_state = operations::MetadataOperationBackupState::retained,
                      .state = operations::MetadataOperationBackupState::undoing,
                      .undo_id = undo_id,
                      .failure = std::nullopt,
                  })
              .has_value());
    CHECK(::rename((**recovery_record).source_raw_path.c_str(),
                   (**recovery_record).prepared_raw_path.c_str()) == 0);
    CHECK(::rename((**recovery_record).backup_raw_path.c_str(),
                   (**recovery_record).source_raw_path.c_str()) == 0);
    CHECK(::rename((**recovery_record).prepared_raw_path.c_str(),
                   (**recovery_record).backup_raw_path.c_str()) == 0);

    std::size_t recovery_callback_count = 0U;
    const auto recovered = operations::recover_metadata_operations(
        *journal, [&](const operations::MetadataCommitResult& result) -> core::Result<void> {
            if (result.journal_id == undo_id) {
                ++recovery_callback_count;
            }
            return {};
        });
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    CHECK(recovered.has_value());
    CHECK(recovered && std::ranges::any_of(*recovered, [&](const auto& result) {
              return result.journal_id == recovery_commit->journal_id &&
                     result.outcome == operations::MetadataRecoveryOutcome::undone;
          }));
    CHECK(recovery_callback_count == 1U);
    CHECK(read_bytes(recovery_source) == recovery_original);
    CHECK(!std::filesystem::exists(recovery_commit->backup_raw_path));
}

void retention_releases_only_verified_backups(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "retention.flac");
    auto plan = title_plan(source, "Retention title");
    auto journal = open_journal(directory, "retention.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    auto committed =
        operations::commit_flac_metadata_source(*plan, *journal, successful_dependent_commit);
    CHECK(committed.has_value());
    if (!committed) {
        return;
    }
    const auto maintained = operations::maintain_metadata_backups(
        *journal,
        operations::MetadataBackupRetentionPolicy{
            .maximum_age_seconds = 0, .maximum_entries = 0U, .maximum_total_bytes = 0U},
        static_cast<std::int64_t>(std::time(nullptr)) + 1);
    CHECK(maintained.has_value() && maintained->size() == 1U &&
          maintained->front().outcome == operations::MetadataBackupMaintenanceOutcome::released);
    CHECK(!std::filesystem::exists(committed->backup_raw_path));
    const auto lifecycle = journal->load_backup(committed->journal_id);
    CHECK(lifecycle && *lifecycle &&
          (**lifecycle).state == operations::MetadataOperationBackupState::released);
}

void undo_conflicts_become_visible_reconciliation_evidence(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "undo-conflict.flac");
    auto plan = title_plan(source, "Published before external change");
    auto journal = open_journal(directory, "undo-conflict.sqlite3");
    CHECK(plan.has_value() && journal.has_value());
    if (!plan || !journal) {
        return;
    }
    auto committed =
        operations::commit_flac_metadata_source(*plan, *journal, successful_dependent_commit);
    CHECK(committed.has_value());
    if (!committed) {
        return;
    }
    {
        std::ofstream changed{source, std::ios::binary | std::ios::app};
        changed.put('\0');
        CHECK(changed.good());
    }
    const auto undone = operations::undo_flac_metadata_operation(committed->journal_id, *journal,
                                                                 successful_dependent_commit);
    CHECK(!undone && undone.error().code == core::ErrorCode::conflict);
    CHECK(std::filesystem::exists(committed->backup_raw_path));
    const auto lifecycle = journal->load_backup(committed->journal_id);
    CHECK(lifecycle && *lifecycle &&
          (**lifecycle).state == operations::MetadataOperationBackupState::needs_reconciliation &&
          (**lifecycle).failure.has_value());
}

void retention_releases_older_verified_backup_for_the_same_source(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "retention-chain.flac");
    auto journal = open_journal(directory, "retention-chain.sqlite3");
    auto first_plan = title_plan(source, "First retained title");
    CHECK(first_plan.has_value() && journal.has_value());
    if (!first_plan || !journal) {
        return;
    }
    auto first =
        operations::commit_flac_metadata_source(*first_plan, *journal, successful_dependent_commit);
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    auto second_plan = title_plan(source, "Newest retained title");
    CHECK(second_plan.has_value());
    if (!second_plan) {
        return;
    }
    auto second = operations::commit_flac_metadata_source(*second_plan, *journal,
                                                          successful_dependent_commit);
    CHECK(second.has_value());
    if (!second) {
        return;
    }

    const auto maintained =
        operations::maintain_metadata_backups(*journal, operations::MetadataBackupRetentionPolicy{},
                                              static_cast<std::int64_t>(std::time(nullptr)));
    CHECK(maintained.has_value() && maintained->size() == 2U);
    const auto first_lifecycle = journal->load_backup(first->journal_id);
    const auto second_lifecycle = journal->load_backup(second->journal_id);
    CHECK(first_lifecycle && *first_lifecycle &&
          (**first_lifecycle).state == operations::MetadataOperationBackupState::released);
    CHECK(second_lifecycle && *second_lifecycle &&
          (**second_lifecycle).state == operations::MetadataOperationBackupState::retained);
    CHECK(!std::filesystem::exists(first->backup_raw_path));
    CHECK(std::filesystem::exists(second->backup_raw_path));
}

void batch_apply_commits_real_sources_and_reports_partial_results(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto first_source = materialize(fixture_directory, directory.path() / "batch-first.flac");
    const auto second_source =
        materialize(fixture_directory, directory.path() / "batch-second.flac");
    auto first_plan = title_plan(first_source, "First batch title");
    auto second_plan = title_plan(second_source, "Second batch title");
    auto journal = open_journal(directory, "batch.sqlite3");
    CHECK(first_plan.has_value() && second_plan.has_value() && journal.has_value());
    if (!first_plan || !second_plan || !journal) {
        return;
    }
    metadata::MetadataWritePlan plan{
        .sources = {*first_plan, *second_plan},
        .patch_count = 2U,
    };
    std::atomic_size_t callbacks{0U};
    std::vector<operations::MetadataApplyProgress> progress;
    const auto applied = operations::apply_metadata_write_plan(
        plan,
        [&](const metadata::MetadataWritePlanSource& source_plan,
            const core::CancellationToken& cancellation)
            -> core::Result<operations::MetadataCommitResult> {
            return operations::commit_flac_metadata_source(
                source_plan, *journal,
                [&callbacks](const operations::MetadataCommitResult&) -> core::Result<void> {
                    callbacks.fetch_add(1U, std::memory_order_relaxed);
                    return {};
                },
                cancellation);
        },
        [&progress](const operations::MetadataApplyProgress& update) {
            progress.push_back(update);
        },
        {}, operations::MetadataApplyOptions{.maximum_parallelism = 2U});
    if (!applied) {
        std::cerr << applied.error().message << '\n';
    }
    CHECK(applied.has_value());
    CHECK(applied && applied->committed_source_count() == 2U &&
          applied->failed_source_count() == 0U && applied->cancelled_source_count() == 0U);
    CHECK(callbacks.load(std::memory_order_relaxed) == 2U);
    CHECK(progress.size() == 4U);
    CHECK(!progress.empty() && progress.back().completed_sources == 2U);
    const auto first_read = metadata::read_local_metadata(first_source.native());
    const auto second_read = metadata::read_local_metadata(second_source.native());
    CHECK(first_read && first_read->document.effective_values("title") ==
                            std::vector<std::string>{"First batch title"});
    CHECK(second_read && second_read->document.effective_values("title") ==
                             std::vector<std::string>{"Second batch title"});

    auto partial_plan = plan;
    for (std::size_t index = 0U; index < partial_plan.sources.size(); ++index) {
        partial_plan.sources[index].raw_path = "synthetic-" + std::to_string(index);
    }
    partial_plan.sources.push_back(partial_plan.sources.front());
    partial_plan.sources.back().raw_path = "synthetic-2";
    const auto partial = operations::apply_metadata_write_plan(
        partial_plan,
        [](const metadata::MetadataWritePlanSource& source_plan,
           const core::CancellationToken&) -> core::Result<operations::MetadataCommitResult> {
            if (source_plan.raw_path == "synthetic-1") {
                return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                                   .message = "injected source failure",
                                                   .context = {}});
            }
            return operations::MetadataCommitResult{
                .journal_id = core::StableId::random(),
                .source_raw_path = source_plan.raw_path,
                .backup_raw_path = source_plan.raw_path + ".backup",
                .previous_revision = {},
                .published_revision = {},
                .document = {},
                .occurrence_indexes = source_plan.occurrence_indexes,
            };
        });
    CHECK(partial && partial->sources.size() == 3U && partial->committed_source_count() == 2U &&
          partial->failed_source_count() == 1U && partial->sources[1].issue &&
          partial->sources[1].issue->message == "injected source failure");
}

void batch_apply_cancellation_stops_new_source_admission() {
    metadata::MetadataWritePlan plan{.sources = {}, .patch_count = 4U};
    for (std::size_t index = 0U; index < 4U; ++index) {
        plan.sources.push_back(metadata::MetadataWritePlanSource{
            .raw_path = "cancel-" + std::to_string(index),
            .occurrence_indexes = {index},
            .expected_revision = core::LocalSourceRevision{},
            .observed_revision = core::LocalSourceRevision{},
            .adapter_name = "taglib-flac-v1",
            .changes = {},
            .issues = {},
        });
    }
    core::CancellationSource cancellation;
    std::atomic_size_t admitted{0U};
    auto future = std::async(std::launch::async, [&] {
        return operations::apply_metadata_write_plan(
            plan,
            [&admitted](const metadata::MetadataWritePlanSource& source_plan,
                        const core::CancellationToken& token)
                -> core::Result<operations::MetadataCommitResult> {
                admitted.fetch_add(1U, std::memory_order_relaxed);
                while (!token.is_cancellation_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "cancelled " + source_plan.raw_path,
                    .context = {},
                });
            },
            {}, cancellation.token(), operations::MetadataApplyOptions{.maximum_parallelism = 2U});
    });
    while (admitted.load(std::memory_order_relaxed) < 2U) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    cancellation.request_cancellation();
    const auto cancelled = future.get();
    CHECK(cancelled && cancelled->cancellation_requested &&
          cancelled->cancelled_source_count() == 4U && cancelled->committed_source_count() == 0U &&
          admitted.load() == 2U);
}

[[nodiscard]] metadata::ArtworkWritePlan synthetic_artwork_plan(const std::size_t count) {
    metadata::ArtworkWritePlan plan{.sources = {}, .logical_intent_count = count};
    plan.sources.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const auto raw_path = "artwork-apply-" + std::to_string(index) + ".flac";
        plan.sources.push_back(metadata::ArtworkWritePlanSource{
            .raw_media_path = raw_path,
            .occurrence_indexes = {index},
            .expected_media_revision = core::LocalSourceRevision{},
            .observed_media_revision = core::LocalSourceRevision{},
            .adapter_name = "taglib-flac-picture-v1",
            .change =
                metadata::ArtworkWritePlanChange{
                    .kind = metadata::ArtworkWritePlanIntentKind::remove,
                    .target_ordinal = 0U,
                    .expected_target_fingerprint = {},
                    .original =
                        metadata::ArtworkInventoryItem{
                            .role = metadata::ArtworkRole::front,
                            .native_type = "Front Cover",
                            .mime_type = "image/png",
                            .description = {},
                            .width = 1U,
                            .height = 1U,
                            .byte_size = 1U,
                            .content_fingerprint = {},
                            .provenance = metadata::ArtworkProvenance::embedded,
                            .raw_source_path = raw_path,
                            .source_revision = {},
                            .source_ordinal = 0U,
                            .duplicate_of = std::nullopt,
                        },
                    .replacement = std::nullopt,
                    .added_role = metadata::ArtworkRole::front,
                    .added_description = {},
                },
            .issues = {},
        });
    }
    return plan;
}

void artwork_batch_apply_reports_ordered_partial_results_and_cancellation() {
    auto plan = synthetic_artwork_plan(3U);
    CHECK(plan.ready());
    std::vector<operations::ArtworkApplyProgress> progress;
    const auto partial = operations::apply_artwork_write_plan(
        plan,
        [](const metadata::ArtworkWritePlanSource& source_plan,
           const core::CancellationToken&) -> core::Result<operations::MetadataCommitResult> {
            if (source_plan.raw_media_path == "artwork-apply-1.flac") {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::io,
                    .message = "injected artwork source failure",
                    .context = {},
                });
            }
            return operations::MetadataCommitResult{
                .journal_id = core::StableId::random(),
                .source_raw_path = source_plan.raw_media_path,
                .backup_raw_path = source_plan.raw_media_path + ".backup",
                .previous_revision = {},
                .published_revision = {},
                .document = {},
                .occurrence_indexes = source_plan.occurrence_indexes,
            };
        },
        [&progress](const operations::ArtworkApplyProgress& update) { progress.push_back(update); },
        {}, operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
    CHECK(partial && partial->sources.size() == 3U &&
          partial->sources[0].state == operations::ArtworkApplySourceState::committed &&
          partial->sources[1].state == operations::ArtworkApplySourceState::failed &&
          partial->sources[1].issue &&
          partial->sources[1].issue->message == "injected artwork source failure" &&
          partial->sources[2].state == operations::ArtworkApplySourceState::committed &&
          partial->committed_source_count() == 2U && partial->failed_source_count() == 1U &&
          progress.size() == 6U && progress.back().completed_sources == 3U);

    core::CancellationSource cancellation;
    std::atomic_size_t admitted{0U};
    auto future = std::async(std::launch::async, [&] {
        return operations::apply_artwork_write_plan(
            plan,
            [&admitted](const metadata::ArtworkWritePlanSource& source_plan,
                        const core::CancellationToken& token)
                -> core::Result<operations::MetadataCommitResult> {
                admitted.fetch_add(1U, std::memory_order_relaxed);
                while (!token.is_cancellation_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "cancelled " + source_plan.raw_media_path,
                    .context = {},
                });
            },
            {}, cancellation.token(), operations::ArtworkApplyOptions{.maximum_parallelism = 2U});
    });
    while (admitted.load(std::memory_order_relaxed) < 2U) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    cancellation.request_cancellation();
    const auto cancelled = future.get();
    CHECK(cancelled && cancelled->cancellation_requested &&
          cancelled->cancelled_source_count() == 3U &&
          admitted.load(std::memory_order_relaxed) == 2U);
}

void publishes_verified_native_flac_directly_at_changed_destination(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "combined-source.flac");
    const auto target = directory.path() / "Organized" / "combined-target.flac";
    auto source_plan = title_plan(source, "Combined destination title");
    auto checked = destination_preflight(source, target);
    CHECK(source_plan.has_value() && checked.has_value());
    if (!source_plan || !checked) {
        return;
    }
    auto journal = persistence::SqliteFilePublicationJournal::open(directory.path() /
                                                                   "combined-destination.sqlite3");
    CHECK(journal.has_value());
    if (!journal) {
        return;
    }
    std::optional<metadata::MetadataDocument> prepared_document;
    std::size_t dependent_count = 0U;
    auto committed = operations::commit_destination_artifact_publication(
        *checked, 0U, *journal,
        [&](const std::string& prepared_raw_path, const core::CancellationToken& cancellation)
            -> core::Result<core::LocalSourceRevision> {
            auto prepared = metadata::prepare_flac_metadata_write_copy(
                *source_plan, prepared_raw_path, cancellation);
            if (!prepared) {
                return std::unexpected(std::move(prepared.error()));
            }
            prepared_document = prepared->document;
            return prepared->prepared_revision;
        },
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++dependent_count;
            auto reread = metadata::read_local_metadata(result.target_raw_path);
            CHECK(std::filesystem::exists(result.source_raw_path) && reread.has_value() &&
                  prepared_document && reread->source_revision == result.target_revision &&
                  result.content ==
                      operations::FilePublicationContentKind::prepared_destination_artifact &&
                  reread->document == *prepared_document &&
                  reread->document.effective_values("title") ==
                      std::vector<std::string>{"Combined destination title"});
            return reread && prepared_document && reread->document == *prepared_document
                       ? core::Result<void>{}
                       : std::unexpected(core::Error{
                             .code = core::ErrorCode::conflict,
                             .message = "published native FLAC differs from prepared metadata",
                             .context = {},
                         });
        });
    CHECK(committed.has_value() && dependent_count == 1U && !std::filesystem::exists(source) &&
          std::filesystem::exists(target));
    if (!committed) {
        return;
    }
    auto reread = metadata::read_local_metadata(target.native());
    auto record = journal->load(committed->journal_id);
    CHECK(reread &&
          reread->document.effective_values("title") ==
              std::vector<std::string>{"Combined destination title"} &&
          record && *record &&
          (**record).content ==
              operations::FilePublicationContentKind::prepared_destination_artifact &&
          (**record).state == operations::FilePublicationJournalState::complete &&
          !std::filesystem::exists((**record).prepared_raw_path));
}

void bounded_preparation_apply_combines_metadata_and_relocation_transaction(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "apply-source.flac");
    const auto target = directory.path() / "Library" / "apply-target.flac";
    auto source_read = metadata::read_local_metadata(source.native());
    auto source_plan = title_plan(source, "Combined Apply title");
    auto checked = destination_preflight(source, target);
    CHECK(source_read.has_value() && source_plan.has_value() && checked.has_value());
    if (!source_read || !source_plan || !checked) {
        return;
    }

    const auto database_path = directory.path() / "combined-apply.sqlite3";
    auto repository = persistence::ListRepository::open(database_path);
    CHECK(repository.has_value());
    if (!repository) {
        return;
    }
    const std::vector documents{persistence::ListDocument{
        .id = core::StableId::random(),
        .kind = persistence::ListKind::scratch,
        .name = "Combined Apply",
        .pinned = false,
        .dirty = true,
        .items = {persistence::ListItem{
            .source = persistence::ListSource::local,
            .profile_id = std::nullopt,
            .source_reference = source.native(),
            .logical_reference = std::nullopt,
            .segment = std::nullopt,
            .source_selection = std::nullopt,
            .duration_ms = std::nullopt,
            .source_revision = source_read->source_revision,
            .fields = {{.name = "title",
                        .value = "Before combined Apply",
                        .native_name = "TITLE",
                        .provenance = metadata::FieldProvenance::embedded}},
        }},
    }};
    CHECK(repository->replace_all(documents).has_value());
    CHECK(repository
              ->relocate_local_source(persistence::LocalSourceRelocation{
                  .operation_id = core::StableId::random(),
                  .source_reference = source.native(),
                  .target_reference = target.native(),
                  .previous_revision = source_read->source_revision,
                  .published_revision = source_read->source_revision,
                  .published_document = std::nullopt,
              })
              .has_value());
    CHECK(repository->replace_all(documents).has_value());
    const auto pre_resolved = repository->load_all();
    CHECK(pre_resolved && pre_resolved->front().items.front().source_reference == target.native() &&
          pre_resolved->front().items.front().source_revision == source_read->source_revision &&
          std::filesystem::exists(source) && !std::filesystem::exists(target));

    auto plan = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 1U,
        metadata::MetadataWritePlan{.sources = {*source_plan}, .patch_count = 1U}, checked->plan,
        *checked);
    auto file_journal = persistence::SqliteFilePublicationJournal::open(database_path);
    auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database_path);
    CHECK(plan && plan->ready() && file_journal && metadata_journal);
    if (!plan || !plan->ready() || !file_journal || !metadata_journal) {
        return;
    }

    std::size_t metadata_only_callbacks = 0U;
    std::size_t path_only_callbacks = 0U;
    std::size_t combined_callbacks = 0U;
    auto applied = operations::apply_preparation_publications(
        *plan, *file_journal, *metadata_journal,
        [&metadata_only_callbacks](const operations::MetadataCommitResult&) -> core::Result<void> {
            ++metadata_only_callbacks;
            return {};
        },
        [&path_only_callbacks](
            const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++path_only_callbacks;
            return {};
        },
        [&repository,
         &combined_callbacks](const operations::FilePublicationCommitResult& result,
                              const metadata::MetadataDocument& document) -> core::Result<void> {
            ++combined_callbacks;
            auto relocated = repository->relocate_local_source(persistence::LocalSourceRelocation{
                .operation_id = result.journal_id,
                .source_reference = result.source_raw_path,
                .target_reference = result.target_raw_path,
                .previous_revision = result.source_revision,
                .published_revision = result.target_revision,
                .published_document = document,
            });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        },
        {}, {}, operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
    if (!applied) {
        std::cerr << applied.error().message << '\n';
    }
    CHECK(applied && applied->committed_source_count() == 1U &&
          applied->sources.front().commit.has_value() &&
          !applied->sources.front().metadata_commit.has_value() &&
          applied->sources.front().published_metadata.has_value());
    CHECK(metadata_only_callbacks == 0U && path_only_callbacks == 0U && combined_callbacks == 1U);
    CHECK(!std::filesystem::exists(source) && std::filesystem::exists(target));
    const auto reread = metadata::read_local_metadata(target.native());
    const auto persisted = repository->load_all();
    CHECK(reread && reread->document.effective_values("title") ==
                        std::vector<std::string>{"Combined Apply title"});
    CHECK(persisted && persisted->front().items.front().source_reference == target.native() &&
          persisted->front().items.front().source_revision == reread->source_revision &&
          std::ranges::any_of(persisted->front().items.front().fields, [](const auto& field) {
              return field.name == "title" && field.value == "Combined Apply title";
          }));
}

void bounded_preparation_apply_commits_metadata_when_path_is_unchanged(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, directory.path() / "unchanged-path.flac");
    auto source_plan = title_plan(source, "Metadata-only batch member");
    CHECK(source_plan.has_value());
    if (!source_plan) {
        return;
    }

    operations::OutputPathPlan path_plan{
        .layout = {},
        .destination = std::nullopt,
        .operations = {.rename_files = true, .move_files = false},
        .sources = {operations::PlannedOutputPathSource{
            .source_raw_path = source.native(),
            .source_revision = *source_plan->observed_revision,
            .target_raw_path = source.native(),
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = source.stem().native(),
            .sanitized_basename = source.stem().native(),
            .item_indexes = {0U},
            .sanitized = false,
            .no_change = true,
        }},
        .issues = {},
    };
    auto checked = operations::preflight_output_paths(path_plan);
    CHECK(checked && checked->ready() && checked->sources.size() == 1U &&
          checked->sources.front().publication == operations::OutputPathPublicationKind::no_change);
    if (!checked || !checked->ready()) {
        return;
    }
    auto plan = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 1U,
        metadata::MetadataWritePlan{.sources = {*source_plan}, .patch_count = 1U}, path_plan,
        *checked);
    const auto database_path = directory.path() / "unchanged-path.sqlite3";
    auto file_journal = persistence::SqliteFilePublicationJournal::open(database_path);
    auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database_path);
    CHECK(plan && plan->ready() && file_journal && metadata_journal);
    if (!plan || !plan->ready() || !file_journal || !metadata_journal) {
        return;
    }

    std::size_t metadata_callbacks = 0U;
    std::size_t file_callbacks = 0U;
    std::size_t artifact_callbacks = 0U;
    auto applied = operations::apply_preparation_publications(
        *plan, *file_journal, *metadata_journal,
        [&metadata_callbacks](const operations::MetadataCommitResult&) -> core::Result<void> {
            ++metadata_callbacks;
            return {};
        },
        [&file_callbacks](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++file_callbacks;
            return {};
        },
        [&artifact_callbacks](const operations::FilePublicationCommitResult&,
                              const metadata::MetadataDocument&) -> core::Result<void> {
            ++artifact_callbacks;
            return {};
        });
    CHECK(applied && applied->committed_source_count() == 1U &&
          applied->sources.front().metadata_commit.has_value() &&
          !applied->sources.front().commit.has_value() &&
          applied->sources.front().published_metadata.has_value());
    CHECK(metadata_callbacks == 1U && file_callbacks == 0U && artifact_callbacks == 0U);
    const auto reread = metadata::read_local_metadata(source.native());
    CHECK(reread && reread->document.effective_values("title") ==
                        std::vector<std::string>{"Metadata-only batch member"});
}

void combined_apply_reuses_directories_created_by_a_rolled_back_member(
    const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto first = materialize(fixture_directory, directory.path() / "first-source.flac");
    const auto second = materialize(fixture_directory, directory.path() / "second-source.flac");
    const auto target_parent = directory.path() / "Organized" / "Artist" / "Album";
    const auto first_target = target_parent / "first-target.flac";
    const auto second_target = target_parent / "second-target.flac";
    auto first_plan = title_plan(first, "First rolled-back title");
    auto second_plan = title_plan(second, "Second committed title");
    CHECK(first_plan && second_plan);
    if (!first_plan || !second_plan) {
        return;
    }

    operations::OutputPathPlan path_plan{
        .layout = {},
        .destination = std::nullopt,
        .operations = {.rename_files = true, .move_files = false},
        .sources = {},
        .issues = {},
    };
    const auto add_path = [&path_plan](const metadata::MetadataWritePlanSource& source_plan,
                                       const std::filesystem::path& target,
                                       const std::size_t index) {
        path_plan.sources.push_back(operations::PlannedOutputPathSource{
            .source_raw_path = source_plan.raw_path,
            .source_revision = *source_plan.observed_revision,
            .target_raw_path = target.native(),
            .raw_relative_directory = {},
            .sanitized_relative_directory = {},
            .raw_basename = target.stem().native(),
            .sanitized_basename = target.stem().native(),
            .item_indexes = {index},
            .sanitized = false,
            .no_change = false,
        });
    };
    add_path(*first_plan, first_target, 0U);
    add_path(*second_plan, second_target, 1U);
    auto checked = operations::preflight_output_paths(path_plan);
    CHECK(checked && checked->ready() && checked->sources.size() == 2U &&
          checked->sources[0].missing_directory_raw_paths ==
              checked->sources[1].missing_directory_raw_paths);
    if (!checked || !checked->ready()) {
        return;
    }
    auto plan = operations::assemble_preparation_plan(
        {.save_tags = true, .rename_files = true, .move_files = false, .replaygain = false}, 2U,
        metadata::MetadataWritePlan{.sources = {*first_plan, *second_plan}, .patch_count = 2U},
        path_plan, *checked);
    const auto database_path = directory.path() / "rollback-directories.sqlite3";
    auto file_journal = persistence::SqliteFilePublicationJournal::open(database_path);
    auto metadata_journal = persistence::SqliteMetadataOperationJournal::open(database_path);
    CHECK(plan && plan->ready() && file_journal && metadata_journal);
    if (!plan || !plan->ready() || !file_journal || !metadata_journal) {
        return;
    }

    auto applied = operations::apply_preparation_publications(
        *plan, *file_journal, *metadata_journal,
        [](const operations::MetadataCommitResult&) -> core::Result<void> { return {}; },
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; },
        [&first](const operations::FilePublicationCommitResult& result,
                 const metadata::MetadataDocument&) -> core::Result<void> {
            if (result.source_raw_path == first.native()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "injected combined dependent-state conflict",
                    .context = {},
                });
            }
            return {};
        },
        {}, {}, operations::FilePublicationApplyOptions{.maximum_parallelism = 1U});
    CHECK(applied && applied->failed_source_count() == 1U &&
          applied->committed_source_count() == 1U && applied->sources[0].issue &&
          applied->sources[0].issue->message == "injected combined dependent-state conflict" &&
          applied->sources[1].state == operations::FilePublicationApplySourceState::committed);
    CHECK(std::filesystem::exists(first) && !std::filesystem::exists(first_target) &&
          !std::filesystem::exists(second) && std::filesystem::exists(second_target));
    const auto reread = metadata::read_local_metadata(second_target.native());
    CHECK(reread && reread->document.effective_values("title") ==
                        std::vector<std::string>{"Second committed title"});
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        commits_atomically_and_retains_verified_backup(fixture_directory);
        rolls_back_dependent_and_journal_failures(fixture_directory);
        preserves_ambiguous_external_changes_for_reconciliation(fixture_directory);
        serializes_sources_and_honors_cancellation(fixture_directory);
        rejects_hard_linked_sources_before_journaling(fixture_directory);
        recovers_publication_interrupted_before_journal_transition(fixture_directory);
        commits_and_recovers_artwork_at_unchanged_paths(fixture_directory);
        recovers_safe_prepublication_debris_but_retains_ambiguous_paths(fixture_directory);
        undoes_completed_metadata_and_recovers_interrupted_undo(fixture_directory);
        retention_releases_only_verified_backups(fixture_directory);
        undo_conflicts_become_visible_reconciliation_evidence(fixture_directory);
        retention_releases_older_verified_backup_for_the_same_source(fixture_directory);
        batch_apply_commits_real_sources_and_reports_partial_results(fixture_directory);
        batch_apply_cancellation_stops_new_source_admission();
        artwork_batch_apply_reports_ordered_partial_results_and_cancellation();
        publishes_verified_native_flac_directly_at_changed_destination(fixture_directory);
        bounded_preparation_apply_combines_metadata_and_relocation_transaction(fixture_directory);
        bounded_preparation_apply_commits_metadata_when_path_is_unchanged(fixture_directory);
        combined_apply_reuses_directories_created_by_a_rolled_back_member(fixture_directory);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
