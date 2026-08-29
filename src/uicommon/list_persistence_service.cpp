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

} // namespace trackknife::ui
