// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "quick/mpd_search_result_model.hpp"

#include <QPersistentModelIndex>
#include <QSet>
#include <QStandardItemModel>

#include <deque>
#include <functional>
#include <memory>

namespace trackknife::bench {

// Tree presentation over the bounded search projection. Album expansion and
// selection resolution use complete release lookups, never matching tracks alone.
class MpdLibrarySearchModel final : public QStandardItemModel {
    Q_OBJECT
  public:
    enum Role {
        KindRole = Qt::UserRole + 100,
        SourceRowRole,
        ArtistRole,
        UrisRole,
        LoadedRole,
        MoreRole
    };
    using Completion = std::function<void(QStringList, QString)>;
    explicit MpdLibrarySearchModel(quick::MpdSearchResultModel* source, QObject* parent = nullptr);
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    void setMore(bool more);
    void loadAlbum(const QModelIndex& index);
    void resolve(QModelIndexList indexes, Completion completion);
    static bool actionable(const QModelIndex& index);
    void acceptAlbum(quint64 token, const std::vector<mpd::Track>& tracks, const QString& error);

  signals:
    void albumRequested(quint64 token, const mpd::AlbumFilter& album);
    void rebuilt();
    void problem(const QString& message);

  private:
    struct Pending {
        quint64 token;
        QPersistentModelIndex index;
        std::vector<std::function<void(QString)>> completions;
    };
    struct Resolution {
        QList<QPersistentModelIndex> indexes;
        qsizetype position{0};
        QStringList uris;
        QSet<QString> seen;
        Completion completion;
    };
    void rebuild();
    void ensureAlbum(const QModelIndex& index, std::function<void(QString)> completion);
    void pump();
    void resolveNext(const std::shared_ptr<Resolution>& resolution);
    quick::MpdSearchResultModel* source_;
    std::deque<Pending> pending_;
    quint64 next_token_{0};
    bool loading_{false};
};
} // namespace trackknife::bench
