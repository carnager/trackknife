// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/operation_journal.hpp"
#include "uicommon/list_persistence_service.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {

enum class MetadataOperationJobKind : std::uint8_t { startup, reload, undo, release, file_undo };

struct MetadataOperationJobOutcome {
    MetadataOperationJobKind kind{MetadataOperationJobKind::startup};
    std::optional<core::Error> error;
    std::vector<operations::MetadataRecoveryResult> recovery;
    std::vector<operations::MetadataBackupMaintenanceResult> maintenance;
    std::vector<operations::MetadataOperationBackupRecord> backups;
    std::vector<operations::MetadataOperationJournalRecord> reconciliation;
    std::vector<operations::MetadataCommitResult> refreshed_sources;
    std::vector<operations::FilePublicationRecoveryResult> file_recovery;
    std::vector<operations::FilePublicationJournalRecord> file_publications;
    std::vector<operations::FilePublicationCommitResult> relocated_sources;
};

namespace {

constexpr auto metadata_retention_days = 7;
constexpr auto metadata_retention_entries = 256U;
constexpr auto metadata_retention_bytes = 10U * 1024U * 1024U * 1024U;

struct MetadataHistoryRow {
    core::StableId journal_id;
    QString state;
    QString source;
    QString detail;
    QString completed;
    bool file_publication{false};
    bool can_undo{false};
    bool can_release{false};
};

class MetadataOperationHistoryModel final : public QAbstractTableModel {
  public:
    explicit MetadataOperationHistoryModel(std::vector<MetadataHistoryRow> rows,
                                           QObject* parent = nullptr)
        : QAbstractTableModel(parent), rows_(std::move(rows)) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == Qt::ToolTipRole) {
            return row.detail;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case 0:
            return row.state;
        case 1:
            return row.source;
        case 2:
            return row.completed;
        case 3:
            return row.detail;
        default:
            return {};
        }
    }
    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        static constexpr std::array labels{"State", "Source", "Completed", "Details"};
        return section >= 0 && section < static_cast<int>(labels.size())
                   ? QString::fromLatin1(labels[static_cast<std::size_t>(section)])
                   : QVariant{};
    }
    [[nodiscard]] const MetadataHistoryRow* row(const int index) const {
        return index >= 0 && index < static_cast<int>(rows_.size())
                   ? &rows_[static_cast<std::size_t>(index)]
                   : nullptr;
    }

  private:
    std::vector<MetadataHistoryRow> rows_;
};

constexpr std::array<std::string_view, 12> default_metadata_fields{
    "Title",        "Artist",      "Album Artist", "Album", "Date",     "Track Number",
    "Total Tracks", "Disc Number", "Total Discs",  "Genre", "Composer", "Comment",
};

[[nodiscard]] persistence::LocalMetadataRefresh
metadata_refresh(const operations::MetadataCommitResult& result) {
    return persistence::LocalMetadataRefresh{
        .operation_id = result.journal_id,
        .source_reference = result.source_raw_path,
        .previous_revision = result.previous_revision,
        .published_revision = result.published_revision,
        .document = result.document,
    };
}

[[nodiscard]] std::shared_ptr<MetadataOperationJobOutcome> run_metadata_operation_job(
    const std::filesystem::path& database_path,
    const QPointer<ui::ListPersistenceService>& persistence_service,
    audio::LocalAuditionService* player_service, const MetadataOperationJobKind kind,
    const std::optional<core::StableId>& journal_id, const core::CancellationToken& cancellation) {
    auto outcome = std::make_shared<MetadataOperationJobOutcome>();
    outcome->kind = kind;
    auto metadata_opened = persistence::SqliteMetadataOperationJournal::open(database_path);
    if (!metadata_opened) {
        outcome->error = std::move(metadata_opened.error());
        return outcome;
    }
    auto file_opened = persistence::SqliteFilePublicationJournal::open(database_path);
    if (!file_opened) {
        outcome->error = std::move(file_opened.error());
        return outcome;
    }
    auto metadata_journal = std::move(*metadata_opened);
    auto file_journal = std::move(*file_opened);
    const auto remember_error = [outcome](core::Error error) {
        if (!outcome->error) {
            outcome->error = std::move(error);
        }
    };
    const auto metadata_dependent =
        [persistence_service,
         outcome](const operations::MetadataCommitResult& result) -> core::Result<void> {
        if (!persistence_service) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "Trackbench closed during metadata recovery",
                .context = {},
            });
        }
        auto refreshed = persistence_service->refreshLocalMetadataAndWait(metadata_refresh(result));
        if (!refreshed) {
            return std::unexpected(std::move(refreshed.error()));
        }
        outcome->refreshed_sources.push_back(result);
        return {};
    };
    const auto file_dependent =
        [persistence_service, player_service,
         outcome](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
        if (!persistence_service) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "Trackbench closed during source reconciliation",
                .context = {},
            });
        }
        const auto durable = [persistence_service, result]() -> core::Result<void> {
            if (!persistence_service) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "Trackbench closed during source reconciliation",
                    .context = {},
                });
            }
            auto relocated =
                persistence_service->relocateLocalSourceAndWait(persistence::LocalSourceRelocation{
                    .operation_id = result.journal_id,
                    .source_reference = result.source_raw_path,
                    .target_reference = result.target_raw_path,
                    .previous_revision = result.source_revision,
                    .published_revision = result.target_revision,
                });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        };
        if (player_service != nullptr) {
            auto relocated = player_service->commit_source_relocation_and_wait(
                audio::LocalAuditionSourceRelocation{
                    .source_raw_path = result.source_raw_path,
                    .target_raw_path = result.target_raw_path,
                    .source_revision = result.source_revision,
                    .target_revision = result.target_revision,
                },
                durable);
            if (!relocated) {
                return std::unexpected(std::move(relocated.error()));
            }
        } else if (auto relocated = durable(); !relocated) {
            return std::unexpected(std::move(relocated.error()));
        }
        outcome->relocated_sources.push_back(result);
        return {};
    };

    if (kind == MetadataOperationJobKind::startup) {
        auto recovered = operations::recover_metadata_operations(metadata_journal,
                                                                 metadata_dependent, cancellation);
        if (!recovered) {
            remember_error(std::move(recovered.error()));
        } else {
            outcome->recovery = std::move(*recovered);
            auto maintained = operations::maintain_metadata_backups(
                metadata_journal,
                operations::MetadataBackupRetentionPolicy{
                    .maximum_age_seconds = metadata_retention_days * 24 * 60 * 60,
                    .maximum_entries = metadata_retention_entries,
                    .maximum_total_bytes = metadata_retention_bytes,
                },
                static_cast<std::int64_t>(std::time(nullptr)), cancellation);
            if (!maintained) {
                remember_error(std::move(maintained.error()));
            } else {
                outcome->maintenance = std::move(*maintained);
            }
        }
    } else if (kind == MetadataOperationJobKind::reload ||
               kind == MetadataOperationJobKind::file_undo) {
        // Loading the terminal/incomplete snapshots below is the whole job.
    } else if (!journal_id) {
        outcome->error = core::Error{.code = core::ErrorCode::invalid_argument,
                                     .message = "Metadata operation identity is missing",
                                     .context = {}};
    } else if (kind == MetadataOperationJobKind::undo) {
        auto undone = operations::undo_flac_metadata_operation(*journal_id, metadata_journal,
                                                               metadata_dependent, cancellation);
        if (!undone) {
            remember_error(std::move(undone.error()));
        }
    } else {
        auto released =
            operations::release_metadata_backup(*journal_id, metadata_journal, cancellation);
        if (!released) {
            remember_error(std::move(released.error()));
        }
    }

    if (kind == MetadataOperationJobKind::startup) {
        auto same = operations::recover_same_filesystem_publications(file_journal, file_dependent,
                                                                     cancellation);
        if (!same) {
            remember_error(std::move(same.error()));
        } else {
            outcome->file_recovery.insert(outcome->file_recovery.end(),
                                          std::make_move_iterator(same->begin()),
                                          std::make_move_iterator(same->end()));
        }
        auto cross = operations::recover_cross_filesystem_publications(file_journal, file_dependent,
                                                                       cancellation);
        if (!cross) {
            remember_error(std::move(cross.error()));
        } else {
            outcome->file_recovery.insert(outcome->file_recovery.end(),
                                          std::make_move_iterator(cross->begin()),
                                          std::make_move_iterator(cross->end()));
        }
    } else if (kind == MetadataOperationJobKind::file_undo) {
        if (!journal_id) {
            remember_error(core::Error{.code = core::ErrorCode::invalid_argument,
                                       .message = "File publication identity is missing",
                                       .context = {}});
        } else {
            auto undone = operations::undo_same_filesystem_publication(
                *journal_id, file_journal, file_dependent, cancellation);
            if (!undone) {
                remember_error(std::move(undone.error()));
            }
        }
    }

    auto backups = metadata_journal.load_backups();
    auto incomplete = metadata_journal.load_incomplete();
    auto file_publications = file_journal.load_recent();
    if (!backups && !outcome->error) {
        outcome->error = std::move(backups.error());
    }
    if (!incomplete && !outcome->error) {
        outcome->error = std::move(incomplete.error());
    }
    if (!file_publications && !outcome->error) {
        outcome->error = std::move(file_publications.error());
    }
    if (backups) {
        outcome->backups = std::move(*backups);
    }
    if (incomplete) {
        for (auto& record : *incomplete) {
            if (record.state == operations::MetadataOperationJournalState::needs_reconciliation) {
                outcome->reconciliation.push_back(std::move(record));
            }
        }
    }
    if (file_publications) {
        outcome->file_publications = std::move(*file_publications);
    }
    return outcome;
}

[[nodiscard]] QString backup_state_text(const operations::MetadataOperationBackupState state) {
    using State = operations::MetadataOperationBackupState;
    switch (state) {
    case State::retained:
        return QStringLiteral("Undo available");
    case State::undoing:
        return QStringLiteral("Undo interrupted");
    case State::undone:
        return QStringLiteral("Undone");
    case State::released:
        return QStringLiteral("Backup released");
    case State::needs_reconciliation:
        return QStringLiteral("Needs attention");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] QString
file_publication_state_text(const operations::FilePublicationJournalRecord& record,
                            const bool has_completed_reversal, const bool has_blocking_reversal) {
    using State = operations::FilePublicationJournalState;
    switch (record.state) {
    case State::planned:
        return record.reverses_journal_id ? QStringLiteral("Undo planned")
                                          : QStringLiteral("Planned");
    case State::target_prepared:
        return QStringLiteral("Target prepared");
    case State::target_published:
        return QStringLiteral("Target published");
    case State::dependent_state_committed:
        return QStringLiteral("References updated");
    case State::source_removed:
        return QStringLiteral("Source removed");
    case State::complete:
        if (record.reverses_journal_id) {
            return QStringLiteral("Undo complete");
        }
        if (has_completed_reversal) {
            return QStringLiteral("Undone");
        }
        if (has_blocking_reversal) {
            return QStringLiteral("Undo interrupted");
        }
        return record.publication == operations::OutputPathPublicationKind::same_filesystem_rename
                   ? QStringLiteral("Undo available")
                   : QStringLiteral("Complete");
    case State::rolled_back:
        return record.reverses_journal_id ? QStringLiteral("Undo rolled back")
                                          : QStringLiteral("Rolled back");
    case State::needs_reconciliation:
        return QStringLiteral("Needs attention");
    }
    return QStringLiteral("Unknown");
}

[[nodiscard]] std::vector<MetadataHistoryRow>
metadata_history_rows(const MetadataOperationJobOutcome& snapshot) {
    std::vector<MetadataHistoryRow> rows;
    rows.reserve(snapshot.backups.size() + snapshot.reconciliation.size() +
                 snapshot.file_publications.size());
    std::unordered_set<std::string> seen;
    seen.reserve(snapshot.backups.size());
    for (const auto& backup : snapshot.backups) {
        const auto& record = backup.operation;
        seen.insert(record.id.to_string());
        auto detail = QStringLiteral("%1 field%2 · %3 undo bytes · journal %4")
                          .arg(record.changes.size())
                          .arg(record.changes.size() == 1U ? QString{} : QStringLiteral("s"))
                          .arg(record.expected_revision.size)
                          .arg(QString::fromStdString(record.id.to_string()));
        if (backup.failure) {
            detail += QStringLiteral(" · %1").arg(displayText(backup.failure->message));
        }
        rows.push_back(MetadataHistoryRow{
            .journal_id = record.id,
            .state = backup_state_text(backup.state),
            .source = QString::fromStdString(core::escape_raw_path(record.source_raw_path)),
            .detail = std::move(detail),
            .completed = QDateTime::fromSecsSinceEpoch(backup.completed_at_unix_seconds)
                             .toLocalTime()
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            .file_publication = false,
            .can_undo = backup.state == operations::MetadataOperationBackupState::retained,
            .can_release = backup.state == operations::MetadataOperationBackupState::retained,
        });
    }
    for (const auto& record : snapshot.reconciliation) {
        if (seen.contains(record.id.to_string())) {
            continue;
        }
        auto detail =
            QStringLiteral("Journal %1").arg(QString::fromStdString(record.id.to_string()));
        if (record.failure) {
            detail += QStringLiteral(" · %1").arg(displayText(record.failure->message));
        }
        rows.push_back(MetadataHistoryRow{
            .journal_id = record.id,
            .state = QStringLiteral("Needs attention"),
            .source = QString::fromStdString(core::escape_raw_path(record.source_raw_path)),
            .detail = std::move(detail),
            .completed = QStringLiteral("Interrupted"),
            .file_publication = false,
            .can_undo = false,
            .can_release = false,
        });
    }
    std::unordered_set<std::string> reversal_blocks;
    std::unordered_set<std::string> completed_reversals;
    reversal_blocks.reserve(snapshot.file_publications.size());
    completed_reversals.reserve(snapshot.file_publications.size());
    for (const auto& record : snapshot.file_publications) {
        if (record.reverses_journal_id &&
            record.state != operations::FilePublicationJournalState::rolled_back) {
            reversal_blocks.insert(record.reverses_journal_id->to_string());
            if (record.state == operations::FilePublicationJournalState::complete) {
                completed_reversals.insert(record.reverses_journal_id->to_string());
            }
        }
    }
    for (const auto& record : snapshot.file_publications) {
        const auto has_blocking_reversal = reversal_blocks.contains(record.id.to_string());
        const auto has_completed_reversal = completed_reversals.contains(record.id.to_string());
        auto detail =
            QStringLiteral("File publication · %1 · target %2 · journal %3")
                .arg(record.publication ==
                             operations::OutputPathPublicationKind::same_filesystem_rename
                         ? QStringLiteral("same-filesystem rename")
                         : QStringLiteral("verified cross-filesystem copy"))
                .arg(QString::fromStdString(core::escape_raw_path(record.target_raw_path)))
                .arg(QString::fromStdString(record.id.to_string()));
        if (record.reverses_journal_id) {
            detail += QStringLiteral(" · reverses %1")
                          .arg(QString::fromStdString(record.reverses_journal_id->to_string()));
        }
        if (record.failure) {
            detail += QStringLiteral(" · %1").arg(displayText(record.failure->message));
        }
        rows.push_back(MetadataHistoryRow{
            .journal_id = record.id,
            .state =
                file_publication_state_text(record, has_completed_reversal, has_blocking_reversal),
            .source = QString::fromStdString(core::escape_raw_path(record.source_raw_path)),
            .detail = std::move(detail),
            .completed = record.state == operations::FilePublicationJournalState::complete
                             ? QStringLiteral("Recorded")
                             : QStringLiteral("Interrupted"),
            .file_publication = true,
            .can_undo = record.publication ==
                            operations::OutputPathPublicationKind::same_filesystem_rename &&
                        record.state == operations::FilePublicationJournalState::complete &&
                        !record.reverses_journal_id && !has_blocking_reversal,
            .can_release = false,
        });
    }
    return rows;
}

} // namespace

void BenchMainWindow::showMetadataProperties() {
    auto* tab = currentListTab();
    if (tab == nullptr || tab->view->selectionModel() == nullptr) {
        return;
    }
    auto selected = tab->view->selectionModel()->selectedRows();
    std::ranges::sort(selected, {}, &QModelIndex::row);
    if (selected.empty()) {
        return;
    }
    std::vector<QPersistentModelIndex> selected_rows;
    selected_rows.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto& index : selected) {
        selected_rows.emplace_back(index);
    }
    const auto selected_row_count = selected_rows.size();
    const QPointer model{tab->model};
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    auto* properties = new MetadataPropertiesDialog(
        selected_row_count,
        [model, selected_rows = std::move(selected_rows)](
            const std::size_t selected_index) -> std::optional<MetadataPropertiesSource> {
            if (model == nullptr || selected_index >= selected_rows.size() ||
                !selected_rows[selected_index].isValid()) {
                return std::nullopt;
            }
            const auto row_index = selected_rows[selected_index].row();
            if (row_index < 0 || row_index >= static_cast<int>(model->rows().size())) {
                return std::nullopt;
            }
            const auto& row = model->rows()[static_cast<std::size_t>(row_index)];
            auto label = model->index(row_index, local_title_column).data().toString();
            if (!row.artist.empty()) {
                label = QStringLiteral("%1 — %2").arg(displayText(row.artist), label);
            }
            return MetadataPropertiesSource{
                .source =
                    metadata::StagedMetadataSource{
                        .raw_path = row.raw_path,
                        .source_revision = row.source_revision,
                        .baseline = row.metadata,
                    },
                .track_label = std::move(label),
            };
        },
        std::span{default_metadata_fields},
        [this, database_path, persistence_service] {
            auto documents = collectDocuments();
            auto view_layouts = collectTrackViewLayouts();
            return MetadataWritePlanApplier{
                [database_path, persistence_service, documents = std::move(documents),
                 view_layouts = std::move(view_layouts)](
                    const metadata::MetadataWritePlan& plan,
                    const operations::MetadataApplyProgressCallback& progress,
                    const core::CancellationToken& cancellation) mutable
                    -> core::Result<operations::MetadataApplyResult> {
                    if (!persistence_service) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::cancelled,
                            .message = "Trackbench closed during metadata Apply",
                            .context = {},
                        });
                    }
                    const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                        std::move(documents), std::move(view_layouts));
                    if (!persistence_error.isEmpty()) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::database,
                            .message = utf8Bytes(persistence_error),
                            .context = {},
                        });
                    }
                    auto opened = persistence::SqliteMetadataOperationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    const auto dependent =
                        [persistence_service](
                            const operations::MetadataCommitResult& result) -> core::Result<void> {
                        if (!persistence_service) {
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "Trackbench closed during metadata Apply",
                                .context = {},
                            });
                        }
                        auto refreshed = persistence_service->refreshLocalMetadataAndWait(
                            metadata_refresh(result));
                        return refreshed ? core::Result<void>{}
                                         : std::unexpected(std::move(refreshed.error()));
                    };
                    return operations::apply_metadata_write_plan(
                        plan,
                        [&journal, &dependent](const metadata::MetadataWritePlanSource& source,
                                               const core::CancellationToken& source_cancellation) {
                            return operations::commit_flac_metadata_source(
                                source, journal, dependent, source_cancellation);
                        },
                        progress, cancellation,
                        operations::MetadataApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [this](const operations::MetadataApplyResult& result) {
            auto committed = false;
            for (const auto& source : result.sources) {
                if (!source.commit) {
                    continue;
                }
                applyCommittedMetadata(*source.commit);
                committed = true;
            }
            if (committed) {
                schedulePersist();
            }
            if (!result.sources.empty()) {
                requestMetadataOperationHistoryReload();
            }
        },
        MetadataTransformationStore{
            .load =
                [persistence_service](MetadataTransformationStore::LoadCompletion completion) {
                    if (!persistence_service) {
                        completion({}, QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->loadMetadataTransformationChains(std::move(completion));
                },
            .save =
                [persistence_service](persistence::SavedMetadataTransformationChain chain,
                                      MetadataTransformationStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->saveMetadataTransformationChain(std::move(chain),
                                                                         std::move(completion));
                },
            .remove =
                [persistence_service](core::StableId id,
                                      MetadataTransformationStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->removeMetadataTransformationChain(id,
                                                                           std::move(completion));
                },
        },
        OutputProfileStore{
            .load =
                [persistence_service](OutputProfileStore::LoadCompletion completion) {
                    if (!persistence_service) {
                        completion({}, {}, QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->loadOutputProfiles(std::move(completion));
                },
            .save_layout =
                [persistence_service](persistence::SavedOutputLayoutProfile profile,
                                      OutputProfileStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->saveOutputLayoutProfile(std::move(profile),
                                                                 std::move(completion));
                },
            .remove_layout =
                [persistence_service](core::StableId id,
                                      OutputProfileStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->removeOutputLayoutProfile(id, std::move(completion));
                },
            .save_destination =
                [persistence_service](persistence::SavedDestinationProfile profile,
                                      OutputProfileStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->saveDestinationProfile(std::move(profile),
                                                                std::move(completion));
                },
            .remove_destination =
                [persistence_service](core::StableId id,
                                      OutputProfileStore::Completion completion) {
                    if (!persistence_service) {
                        completion(QStringLiteral("Trackbench persistence is unavailable"));
                        return;
                    }
                    persistence_service->removeDestinationProfile(id, std::move(completion));
                },
        },
        [this, database_path, persistence_service] {
            auto documents = collectDocuments();
            auto view_layouts = collectTrackViewLayouts();
            const auto recovery_was_running = metadata_operation_running_;
            return FilePublicationPlanApplier{
                [this, database_path, persistence_service, documents = std::move(documents),
                 view_layouts = std::move(view_layouts), recovery_was_running](
                    const operations::OutputPathPreflight& preflight,
                    const operations::FilePublicationApplyProgressCallback& progress,
                    const core::CancellationToken& cancellation) mutable
                    -> core::Result<operations::FilePublicationApplyResult> {
                    if (recovery_was_running) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::conflict,
                            .message = "Startup operation recovery is still running; preview "
                                       "again after it finishes",
                            .context = {},
                        });
                    }
                    if (!persistence_service) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::cancelled,
                            .message = "Trackbench closed during file publication",
                            .context = {},
                        });
                    }
                    const auto persistence_error = persistence_service->saveWorkspaceAndWait(
                        std::move(documents), std::move(view_layouts));
                    if (!persistence_error.isEmpty()) {
                        return std::unexpected(core::Error{
                            .code = core::ErrorCode::database,
                            .message = utf8Bytes(persistence_error),
                            .context = {},
                        });
                    }
                    auto opened = persistence::SqliteFilePublicationJournal::open(database_path);
                    if (!opened) {
                        return std::unexpected(std::move(opened.error()));
                    }
                    auto journal = std::move(*opened);
                    const auto dependent =
                        [this,
                         persistence_service](const operations::FilePublicationCommitResult& result)
                        -> core::Result<void> {
                        if (!persistence_service) {
                            return std::unexpected(core::Error{
                                .code = core::ErrorCode::cancelled,
                                .message = "Trackbench closed during source reconciliation",
                                .context = {},
                            });
                        }
                        const auto durable = [persistence_service, result]() -> core::Result<void> {
                            if (!persistence_service) {
                                return std::unexpected(core::Error{
                                    .code = core::ErrorCode::cancelled,
                                    .message = "Trackbench closed during source reconciliation",
                                    .context = {},
                                });
                            }
                            auto relocated = persistence_service->relocateLocalSourceAndWait(
                                persistence::LocalSourceRelocation{
                                    .operation_id = result.journal_id,
                                    .source_reference = result.source_raw_path,
                                    .target_reference = result.target_raw_path,
                                    .previous_revision = result.source_revision,
                                    .published_revision = result.target_revision,
                                });
                            return relocated ? core::Result<void>{}
                                             : std::unexpected(std::move(relocated.error()));
                        };
                        if (player_ == nullptr) {
                            return durable();
                        }
                        auto relocated = player_->commit_source_relocation_and_wait(
                            audio::LocalAuditionSourceRelocation{
                                .source_raw_path = result.source_raw_path,
                                .target_raw_path = result.target_raw_path,
                                .source_revision = result.source_revision,
                                .target_revision = result.target_revision,
                            },
                            durable);
                        return relocated ? core::Result<void>{}
                                         : std::unexpected(std::move(relocated.error()));
                    };
                    return operations::apply_file_publications(
                        preflight, journal, dependent, progress, cancellation,
                        operations::FilePublicationApplyOptions{.maximum_parallelism = 2U});
                }};
        },
        [this](const operations::FilePublicationApplyResult& result) {
            auto committed = false;
            for (const auto& source : result.sources) {
                if (!source.commit) {
                    continue;
                }
                applyCommittedRelocation(*source.commit);
                committed = true;
            }
            if (committed) {
                schedulePersist();
                requestMetadataOperationHistoryReload();
            }
        },
        tabs_);
    properties->setWindowFlags(Qt::Widget);
    properties->setProperty("bench-special-tab", QStringLiteral("metadata-properties"));
    const auto tab_title =
        QStringLiteral("Tags · %1 %2")
            .arg(selected_row_count)
            .arg(selected_row_count == 1U ? QStringLiteral("track") : QStringLiteral("tracks"));
    const auto properties_index = tabs_->addTab(properties, tab_title);
    tabs_->setTabToolTip(
        properties_index,
        QStringLiteral("Temporary tagging workspace · close after applying or discarding"));
    connect(properties, &QObject::destroyed, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (list_tabs_.empty()) {
                addListTab(
                    persistence::ListDocument{
                        .id = core::StableId::random(),
                        .kind = persistence::ListKind::scratch,
                        .name = "Scratch",
                        .pinned = false,
                        .dirty = false,
                        .items = {},
                    },
                    true);
            }
            refreshTabActions();
            refreshTrackViewActions();
            refreshSelectionStatus();
        });
    });
    tabs_->setCurrentIndex(properties_index);
    properties->show();
}

void BenchMainWindow::startMetadataOperationRecovery() {
    if (metadata_recovery_started_ || metadata_operation_running_ || persistence_ == nullptr ||
        database_path_.empty()) {
        return;
    }
    metadata_recovery_started_ = true;
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(false);
    }
    setProperty("trackbench-metadata-operation-running", true);
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(
        QtConcurrent::run([database_path, persistence_service, player = player_, cancellation] {
            return run_metadata_operation_job(database_path, persistence_service, player,
                                              MetadataOperationJobKind::startup, std::nullopt,
                                              cancellation);
        }));
}

void BenchMainWindow::requestMetadataOperationHistoryReload() {
    metadata_history_reload_pending_ = true;
    if (metadata_operation_running_ || persistence_ == nullptr || database_path_.empty()) {
        return;
    }
    metadata_history_reload_pending_ = false;
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(false);
    }
    setProperty("trackbench-metadata-operation-running", true);
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(
        QtConcurrent::run([database_path, persistence_service, player = player_, cancellation] {
            return run_metadata_operation_job(database_path, persistence_service, player,
                                              MetadataOperationJobKind::reload, std::nullopt,
                                              cancellation);
        }));
}

void BenchMainWindow::startMetadataUndo(const core::StableId& journal_id) {
    if (metadata_operation_running_ || persistence_ == nullptr || journal_id.is_nil()) {
        return;
    }
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(false);
    }
    setProperty("trackbench-metadata-operation-running", true);
    statusBar()->showMessage(QStringLiteral("Restoring retained metadata backup…"));
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(QtConcurrent::run(
        [database_path, persistence_service, player = player_, cancellation, journal_id] {
            return run_metadata_operation_job(database_path, persistence_service, player,
                                              MetadataOperationJobKind::undo, journal_id,
                                              cancellation);
        }));
}

void BenchMainWindow::startFilePublicationUndo(const core::StableId& journal_id) {
    if (metadata_operation_running_ || persistence_ == nullptr || journal_id.is_nil()) {
        return;
    }
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(false);
    }
    setProperty("trackbench-metadata-operation-running", true);
    statusBar()->showMessage(QStringLiteral("Moving the file back…"));
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(QtConcurrent::run(
        [database_path, persistence_service, player = player_, cancellation, journal_id] {
            return run_metadata_operation_job(database_path, persistence_service, player,
                                              MetadataOperationJobKind::file_undo, journal_id,
                                              cancellation);
        }));
}

void BenchMainWindow::startMetadataBackupRelease(const core::StableId& journal_id) {
    if (metadata_operation_running_ || journal_id.is_nil()) {
        return;
    }
    metadata_operation_running_ = true;
    metadata_operation_cancellation_ = core::CancellationSource{};
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(false);
    }
    setProperty("trackbench-metadata-operation-running", true);
    statusBar()->showMessage(QStringLiteral("Releasing retained metadata backup…"));
    const QPointer persistence_service{persistence_};
    const auto database_path = database_path_;
    const auto cancellation = metadata_operation_cancellation_.token();
    metadata_operation_watcher_.setFuture(QtConcurrent::run(
        [database_path, persistence_service, player = player_, cancellation, journal_id] {
            return run_metadata_operation_job(database_path, persistence_service, player,
                                              MetadataOperationJobKind::release, journal_id,
                                              cancellation);
        }));
}

void BenchMainWindow::applyCommittedMetadata(const operations::MetadataCommitResult& result) {
    for (auto& tab : list_tabs_) {
        auto applied = tab->model->applyCommittedMetadata(result.source_raw_path, result.document,
                                                          result.published_revision);
        if (!applied) {
            statusBar()->showMessage(QStringLiteral("Metadata view refresh needs attention: %1")
                                         .arg(displayText(applied.error().message)),
                                     8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
}

void BenchMainWindow::applyCommittedRelocation(
    const operations::FilePublicationCommitResult& result) {
    for (auto& tab : list_tabs_) {
        auto applied =
            tab->model->applyCommittedRelocation(result.source_raw_path, result.target_raw_path,
                                                 result.source_revision, result.target_revision);
        if (!applied) {
            statusBar()->showMessage(QStringLiteral("File-path view refresh needs attention: %1")
                                         .arg(displayText(applied.error().message)),
                                     8'000);
            continue;
        }
        if (*applied > 0U) {
            syncArtwork(*tab);
        }
    }
    if (playback_source_.raw_path == result.source_raw_path) {
        playback_source_.raw_path = result.target_raw_path;
    }
    if (last_requested_next_ && last_requested_next_->raw_path == result.source_raw_path) {
        last_requested_next_->raw_path = result.target_raw_path;
    }
}

void BenchMainWindow::finishMetadataOperationJob() {
    metadata_operation_running_ = false;
    setProperty("trackbench-metadata-operation-running", false);
    auto snapshot = metadata_operation_watcher_.result();
    if (!snapshot) {
        if (metadata_history_action_ != nullptr) {
            metadata_history_action_->setEnabled(!isMpdContext());
        }
        statusBar()->showMessage(QStringLiteral("Metadata operation returned no result"), 8'000);
        return;
    }
    for (const auto& refreshed : snapshot->refreshed_sources) {
        applyCommittedMetadata(refreshed);
    }
    for (const auto& relocated : snapshot->relocated_sources) {
        applyCommittedRelocation(relocated);
    }
    if (!snapshot->refreshed_sources.empty() || !snapshot->relocated_sources.empty()) {
        schedulePersist();
    }
    metadata_operation_snapshot_ = std::move(snapshot);
    const auto backup_attention =
        std::ranges::count(metadata_operation_snapshot_->backups,
                           operations::MetadataOperationBackupState::needs_reconciliation,
                           &operations::MetadataOperationBackupRecord::state);
    const auto file_attention =
        std::ranges::count(metadata_operation_snapshot_->file_publications,
                           operations::FilePublicationJournalState::needs_reconciliation,
                           &operations::FilePublicationJournalRecord::state);
    const auto metadata_attention = metadata_operation_snapshot_->reconciliation.size() +
                                    static_cast<std::size_t>(backup_attention);
    const auto attention = metadata_attention + static_cast<std::size_t>(file_attention);
    const auto retained = std::ranges::count(metadata_operation_snapshot_->backups,
                                             operations::MetadataOperationBackupState::retained,
                                             &operations::MetadataOperationBackupRecord::state);
    setProperty("trackbench-metadata-retained-backups", static_cast<qulonglong>(retained));
    setProperty("trackbench-metadata-reconciliation-count",
                static_cast<qulonglong>(metadata_attention));
    setProperty("trackbench-file-reconciliation-count", static_cast<qulonglong>(file_attention));
    if (metadata_history_action_ != nullptr) {
        metadata_history_action_->setEnabled(!isMpdContext());
    }
    if (metadata_history_dialog_) {
        auto* dialog = metadata_history_dialog_.data();
        metadata_history_dialog_ = nullptr;
        dialog->close();
    }

    if (metadata_operation_snapshot_->error) {
        statusBar()->showMessage(
            QStringLiteral("Preparation operation failed: %1")
                .arg(displayText(metadata_operation_snapshot_->error->message)),
            10'000);
    } else if (attention > 0U) {
        statusBar()->showMessage(QStringLiteral("%1 preparation operation%2 need attention")
                                     .arg(attention)
                                     .arg(attention == 1U ? QString{} : QStringLiteral("s")),
                                 10'000);
    } else if (metadata_operation_snapshot_->kind == MetadataOperationJobKind::undo) {
        statusBar()->showMessage(QStringLiteral("Metadata change undone"), 5'000);
    } else if (metadata_operation_snapshot_->kind == MetadataOperationJobKind::release) {
        statusBar()->showMessage(QStringLiteral("Metadata undo backup released"), 5'000);
    } else if (metadata_operation_snapshot_->kind == MetadataOperationJobKind::file_undo) {
        statusBar()->showMessage(QStringLiteral("File move undone"), 5'000);
    } else if (!metadata_operation_snapshot_->recovery.empty() ||
               !metadata_operation_snapshot_->file_recovery.empty()) {
        const auto metadata_recovered =
            std::ranges::count_if(metadata_operation_snapshot_->recovery, [](const auto& result) {
                return result.outcome != operations::MetadataRecoveryOutcome::needs_reconciliation;
            });
        const auto file_recovered = std::ranges::count_if(
            metadata_operation_snapshot_->file_recovery, [](const auto& result) {
                return result.outcome !=
                       operations::FilePublicationRecoveryOutcome::needs_reconciliation;
            });
        const auto recovered = metadata_recovered + file_recovered;
        statusBar()->showMessage(QStringLiteral("Recovered %1 interrupted preparation operation%2")
                                     .arg(recovered)
                                     .arg(recovered == 1 ? QString{} : QStringLiteral("s")),
                                 5'000);
    }
    if (attention > 0U || metadata_operation_snapshot_->kind == MetadataOperationJobKind::undo ||
        metadata_operation_snapshot_->kind == MetadataOperationJobKind::release ||
        metadata_operation_snapshot_->kind == MetadataOperationJobKind::file_undo) {
        showMetadataOperationHistory();
    }
    if (metadata_history_reload_pending_) {
        QTimer::singleShot(0, this, &BenchMainWindow::requestMetadataOperationHistoryReload);
    }
}

void BenchMainWindow::showMetadataOperationHistory() {
    if (metadata_history_dialog_) {
        metadata_history_dialog_->show();
        metadata_history_dialog_->raise();
        metadata_history_dialog_->activateWindow();
        return;
    }
    if (!metadata_operation_snapshot_) {
        statusBar()->showMessage(QStringLiteral("Preparation operation history is still loading"),
                                 5'000);
        return;
    }

    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("bench-metadata-operation-history"));
    dialog->setWindowTitle(QStringLiteral("Preparation operations"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(900, 420);
    metadata_history_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* policy = new QLabel(
        QStringLiteral("Metadata undo backups are kept for up to %1 days; only the newest tag "
                       "change per file is retained, within %2 operations and %3 GiB. Completed "
                       "same-filesystem file moves can be moved back; cross-filesystem moves "
                       "remain visible but are not currently undoable.")
            .arg(metadata_retention_days)
            .arg(metadata_retention_entries)
            .arg(metadata_retention_bytes / (1024U * 1024U * 1024U)),
        dialog);
    policy->setObjectName(QStringLiteral("bench-metadata-retention-policy"));
    policy->setWordWrap(true);
    layout->addWidget(policy);

    auto* table = new QTableView(dialog);
    table->setObjectName(QStringLiteral("bench-metadata-operation-table"));
    auto* model = new MetadataOperationHistoryModel(
        metadata_history_rows(*metadata_operation_snapshot_), table);
    table->setModel(model);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(table, 1);

    if (model->rowCount() == 0) {
        auto* empty =
            new QLabel(QStringLiteral("No preparation operations have been recorded."), dialog);
        empty->setObjectName(QStringLiteral("bench-metadata-operation-empty"));
        layout->insertWidget(1, empty);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->setObjectName(QStringLiteral("bench-metadata-operation-buttons"));
    auto* undo = buttons->addButton(QStringLiteral("Undo change"), QDialogButtonBox::ActionRole);
    undo->setObjectName(QStringLiteral("bench-metadata-operation-undo"));
    auto* release =
        buttons->addButton(QStringLiteral("Delete backup"), QDialogButtonBox::ActionRole);
    release->setObjectName(QStringLiteral("bench-metadata-operation-release"));
    undo->setEnabled(false);
    release->setEnabled(false);
    const auto refresh_buttons = [table, model, undo, release] {
        const auto* row = model->row(table->currentIndex().row());
        undo->setEnabled(row != nullptr && row->can_undo);
        release->setEnabled(row != nullptr && row->can_release);
    };
    connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged, dialog,
            [refresh_buttons](const QModelIndex&, const QModelIndex&) { refresh_buttons(); });
    connect(undo, &QPushButton::clicked, dialog, [this, dialog, table, model] {
        const auto* row = model->row(table->currentIndex().row());
        if (row == nullptr || !row->can_undo) {
            return;
        }
        const auto id = row->journal_id;
        const auto source = row->source;
        const auto file_publication = row->file_publication;
        QMessageBox confirmation{
            QMessageBox::Question,
            file_publication ? QStringLiteral("Undo file move")
                             : QStringLiteral("Undo metadata change"),
            file_publication
                ? QStringLiteral("Move this file back to its original path? The destination must "
                                 "still be unoccupied.\n\n%1")
                      .arg(source)
                : QStringLiteral("Restore the exact tags from before this operation?\n\n%1")
                      .arg(source),
            QMessageBox::Yes | QMessageBox::Cancel, dialog};
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
        metadata_history_dialog_ = nullptr;
        dialog->close();
        if (file_publication) {
            startFilePublicationUndo(id);
        } else {
            startMetadataUndo(id);
        }
    });
    connect(release, &QPushButton::clicked, dialog, [this, dialog, table, model] {
        const auto* row = model->row(table->currentIndex().row());
        if (row == nullptr || !row->can_release) {
            return;
        }
        const auto id = row->journal_id;
        const auto source = row->source;
        QMessageBox confirmation{
            QMessageBox::Question, QStringLiteral("Delete metadata backup"),
            QStringLiteral("Delete the retained undo backup? This metadata change can no "
                           "longer be undone.\n\n%1")
                .arg(source),
            QMessageBox::Yes | QMessageBox::Cancel, dialog};
        confirmation.setOption(QMessageBox::Option::DontUseNativeDialog);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        if (confirmation.exec() != QMessageBox::Yes) {
            return;
        }
        metadata_history_dialog_ = nullptr;
        dialog->close();
        startMetadataBackupRelease(id);
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    if (model->rowCount() > 0) {
        table->selectRow(0);
        refresh_buttons();
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace trackknife::bench
