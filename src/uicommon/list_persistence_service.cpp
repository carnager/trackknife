// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/list_persistence_service.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QThread>

#include <optional>
#include <utility>

namespace trackknife::ui {
namespace {

[[nodiscard]] QString errorText(const core::Error& error) {
    return QString::fromStdString(error.message);
}

} // namespace

struct ListPersistenceService::State {
    std::filesystem::path database_path;
    std::optional<persistence::ListRepository> repository;
    QString initialization_error;
};

ListPersistenceService::ListPersistenceService(std::filesystem::path database_path, QObject* parent)
    : QObject(parent), thread_(new QThread(this)), worker_(new QObject),
      state_(std::make_shared<State>(State{.database_path = std::move(database_path),
                                           .repository = std::nullopt,
                                           .initialization_error = {}})) {
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->setObjectName(QStringLiteral("trackknife-persistence"));
    thread_->start();
}

ListPersistenceService::~ListPersistenceService() {
    if (thread_ == nullptr || worker_ == nullptr) {
        return;
    }
    QMetaObject::invokeMethod(
        worker_, [state = state_] { state->repository.reset(); }, Qt::BlockingQueuedConnection);
    thread_->quit();
    thread_->wait();
}

void ListPersistenceService::initialize(WorkspaceCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, callback = std::move(callback)]() mutable {
            PersistedWorkspace snapshot;
            auto opened = persistence::ListRepository::open(state->database_path);
            if (!opened) {
                state->initialization_error = errorText(opened.error());
            } else {
                state->repository.emplace(std::move(*opened));
                auto lists = state->repository->load_all();
                auto profiles = state->repository->load_profiles();
                auto presets = state->repository->load_view_presets();
                if (!lists) {
                    state->initialization_error = errorText(lists.error());
                } else if (!profiles) {
                    state->initialization_error = errorText(profiles.error());
                } else if (!presets) {
                    state->initialization_error = errorText(presets.error());
                } else {
                    snapshot.lists = std::move(*lists);
                    snapshot.profiles = std::move(*profiles);
                    snapshot.view_presets = std::move(*presets);
                }
            }
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [callback = std::move(callback), snapshot = std::move(snapshot),
                 error = state->initialization_error]() mutable {
                    callback(std::move(snapshot), std::move(error));
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::saveWorkspace(std::vector<persistence::ListDocument> lists,
                                           std::vector<persistence::TrackViewPreset> presets,
                                           CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, lists = std::move(lists), presets = std::move(presets),
         callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && state->repository) {
                if (auto stored = state->repository->replace_all(lists); !stored) {
                    error = errorText(stored.error());
                } else if (auto stored_presets = state->repository->replace_view_presets(presets);
                           !stored_presets) {
                    error = errorText(stored_presets.error());
                }
            }
            if (!callback || !self) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::saveProfiles(std::vector<persistence::ConnectionProfile> profiles,
                                          CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, profiles = std::move(profiles),
         callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && state->repository) {
                if (auto stored = state->repository->replace_profiles(profiles); !stored) {
                    error = errorText(stored.error());
                }
            }
            if (!callback || !self) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::loadMetadataTransformationChains(
    TransformationChainsCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, callback = std::move(callback)]() mutable {
            std::vector<persistence::SavedMetadataTransformationChain> chains;
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                auto loaded = state->repository->load_metadata_transformation_chains();
                if (!loaded) {
                    error = errorText(loaded.error());
                } else {
                    chains = std::move(*loaded);
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [callback = std::move(callback), chains = std::move(chains), error]() mutable {
                    callback(std::move(chains), error);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::saveMetadataTransformationChain(
    persistence::SavedMetadataTransformationChain chain, CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, chain = std::move(chain), callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto stored = state->repository->upsert_metadata_transformation_chain(chain);
                    !stored) {
                    error = errorText(stored.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::removeMetadataTransformationChain(core::StableId id,
                                                               CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, id, callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto removed = state->repository->remove_metadata_transformation_chain(id);
                    !removed) {
                    error = errorText(removed.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::loadOutputProfiles(OutputProfilesCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, callback = std::move(callback)]() mutable {
            std::vector<persistence::SavedOutputLayoutProfile> layouts;
            std::vector<persistence::SavedDestinationProfile> destinations;
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                auto loaded_layouts = state->repository->load_output_layout_profiles();
                if (!loaded_layouts) {
                    error = errorText(loaded_layouts.error());
                } else {
                    auto loaded_destinations = state->repository->load_destination_profiles();
                    if (!loaded_destinations) {
                        error = errorText(loaded_destinations.error());
                    } else {
                        layouts = std::move(*loaded_layouts);
                        destinations = std::move(*loaded_destinations);
                    }
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [callback = std::move(callback), layouts = std::move(layouts),
                 destinations = std::move(destinations),
                 error]() mutable { callback(std::move(layouts), std::move(destinations), error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::saveOutputLayoutProfile(persistence::SavedOutputLayoutProfile profile,
                                                     CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, profile = std::move(profile),
         callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto stored = state->repository->upsert_output_layout_profile(profile);
                    !stored) {
                    error = errorText(stored.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::removeOutputLayoutProfile(core::StableId id,
                                                       CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, id, callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto removed = state->repository->remove_output_layout_profile(id); !removed) {
                    error = errorText(removed.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::saveDestinationProfile(persistence::SavedDestinationProfile profile,
                                                    CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, profile = std::move(profile),
         callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto stored = state->repository->upsert_destination_profile(profile); !stored) {
                    error = errorText(stored.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void ListPersistenceService::removeDestinationProfile(core::StableId id,
                                                      CompletionCallback callback) {
    const QPointer self{this};
    QMetaObject::invokeMethod(
        worker_,
        [self, state = state_, id, callback = std::move(callback)]() mutable {
            QString error = state->initialization_error;
            if (error.isEmpty() && !state->repository) {
                error = QStringLiteral("List persistence is not initialized");
            } else if (error.isEmpty()) {
                if (auto removed = state->repository->remove_destination_profile(id); !removed) {
                    error = errorText(removed.error());
                }
            }
            if (!self || !callback) {
                return;
            }
            QMetaObject::invokeMethod(
                self, [callback = std::move(callback), error]() mutable { callback(error); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

QString
ListPersistenceService::saveWorkspaceAndWait(std::vector<persistence::ListDocument> lists,
                                             std::vector<persistence::TrackViewPreset> presets) {
    QString error;
    QMetaObject::invokeMethod(
        worker_,
        [state = state_, lists = std::move(lists), presets = std::move(presets), &error] {
            error = state->initialization_error;
            if (!error.isEmpty() || !state->repository) {
                return;
            }
            if (auto stored = state->repository->replace_all(lists); !stored) {
                error = errorText(stored.error());
            } else if (auto stored_presets = state->repository->replace_view_presets(presets);
                       !stored_presets) {
                error = errorText(stored_presets.error());
            }
        },
        Qt::BlockingQueuedConnection);
    return error;
}

core::Result<persistence::LocalMetadataRefreshResult>
ListPersistenceService::refreshLocalMetadataAndWait(persistence::LocalMetadataRefresh refresh) {
    if (QThread::currentThread() == thread()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Metadata refresh cannot block the UI thread",
            .context = {},
        });
    }
    if (QThread::currentThread() == thread_) {
        if (!state_->initialization_error.isEmpty() || !state_->repository) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state_->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state_->initialization_error.toStdString(),
                .context = {},
            });
        }
        return state_->repository->refresh_local_metadata(refresh);
    }
    std::optional<core::Result<persistence::LocalMetadataRefreshResult>> result;
    QMetaObject::invokeMethod(
        worker_,
        [state = state_, refresh = std::move(refresh), &result] {
            if (!state->initialization_error.isEmpty() || !state->repository) {
                result = std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = state->initialization_error.isEmpty()
                                   ? "List persistence is not initialized"
                                   : state->initialization_error.toStdString(),
                    .context = {},
                });
                return;
            }
            result = state->repository->refresh_local_metadata(refresh);
        },
        Qt::BlockingQueuedConnection);
    if (!result) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Metadata refresh did not return a persistence result",
            .context = {},
        });
    }
    return std::move(*result);
}

core::Result<persistence::LocalSourceRelocationResult>
ListPersistenceService::relocateLocalSourceAndWait(persistence::LocalSourceRelocation relocation) {
    if (QThread::currentThread() == thread()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Local-source relocation cannot block the UI thread",
            .context = {},
        });
    }
    if (QThread::currentThread() == thread_) {
        if (!state_->initialization_error.isEmpty() || !state_->repository) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state_->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state_->initialization_error.toStdString(),
                .context = {},
            });
        }
        return state_->repository->relocate_local_source(relocation);
    }
    std::optional<core::Result<persistence::LocalSourceRelocationResult>> result;
    QMetaObject::invokeMethod(
        worker_,
        [state = state_, relocation = std::move(relocation), &result] {
            if (!state->initialization_error.isEmpty() || !state->repository) {
                result = std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = state->initialization_error.isEmpty()
                                   ? "List persistence is not initialized"
                                   : state->initialization_error.toStdString(),
                    .context = {},
                });
                return;
            }
            result = state->repository->relocate_local_source(relocation);
        },
        Qt::BlockingQueuedConnection);
    if (!result) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Local-source relocation did not return a persistence result",
            .context = {},
        });
    }
    return std::move(*result);
}

} // namespace trackknife::ui
