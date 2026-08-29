// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"

#include <QAbstractTableModel>

#include <optional>
#include <string>
#include <vector>

namespace trackknife::quick {

class MpdBrowserModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr int column_count = 3;

    enum ExtraRole {
        UriRole = Qt::UserRole + 1,
        KindRole,
        ContainerRole,
    };
    enum class EntryKind { directory, track, playlist };

    explicit MpdBrowserModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] std::optional<EntryKind> kindAt(int row) const;
    [[nodiscard]] std::optional<std::string> uriAt(int row) const;

    void replaceEntries(std::vector<mpd::DatabaseEntry> entries);

  private:
    std::vector<mpd::DatabaseEntry> entries_;
};

} // namespace trackknife::quick
