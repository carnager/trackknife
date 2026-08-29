// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QObject>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QThread;

namespace trackknife::ui {

struct PersistedWorkspace {
    std::vector<persistence::ListDocument> lists;
    std::vector<persistence::ConnectionProfile> profiles;
    std::vector<persistence::TrackViewPreset> view_presets;
};

class ListPersistenceService final : public QObject {
    Q_OBJECT

  public:
    using WorkspaceCallback = std::function<void(PersistedWorkspace, QString)>;
    using CompletionCallback = std::function<void(QString)>;

    explicit ListPersistenceService(std::filesystem::path database_path, QObject* parent = nullptr);
    ~ListPersistenceService() override;

    ListPersistenceService(const ListPersistenceService&) = delete;
    ListPersistenceService& operator=(const ListPersistenceService&) = delete;

    void initialize(WorkspaceCallback callback);
    void saveWorkspace(std::vector<persistence::ListDocument> lists,
                       std::vector<persistence::TrackViewPreset> presets,
                       CompletionCallback callback = {});
    void saveProfiles(std::vector<persistence::ConnectionProfile> profiles,
                      CompletionCallback callback = {});

    // Window shutdown is the only blocking persistence boundary. Database work
    // still runs on the service thread and the call guarantees durable edits.
    [[nodiscard]] QString saveWorkspaceAndWait(std::vector<persistence::ListDocument> lists,
                                               std::vector<persistence::TrackViewPreset> presets);

  private:
    struct State;
    QThread* thread_{nullptr};
    QObject* worker_{nullptr};
    std::shared_ptr<State> state_;
};

} // namespace trackknife::ui
