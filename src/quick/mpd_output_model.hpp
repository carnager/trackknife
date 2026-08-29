// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"

#include <QAbstractListModel>

#include <vector>

namespace trackknife::quick {

class MpdOutputModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role {
        OutputIdRole = Qt::UserRole + 1,
        NameRole,
        EnabledRole,
        PrimaryRole,
        OnlineKnownRole,
        OnlineRole,
        DetailRole,
    };

    explicit MpdOutputModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replaceOutputs(std::vector<mpd::Output> outputs);

  private:
    std::vector<mpd::Output> outputs_;
};

} // namespace trackknife::quick
