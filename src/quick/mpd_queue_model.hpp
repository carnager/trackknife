// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/mpd/model.hpp"

#include <QAbstractTableModel>
#include <QHash>
#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::quick {

class MpdQueueModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    static constexpr int column_count = 6;

    enum ExtraRole {
        UriRole = Qt::UserRole + 1,
        QueueIdRole,
        QueuePositionRole,
        DurationMsRole,
        CurrentRole,
        AlbumArtistRole,
        PriorityRole,
        AlbumArtworkRole,
        AlbumArtworkUriRole,
    };

    explicit MpdQueueModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] std::optional<std::uint32_t> queueIdAt(int row) const;
    [[nodiscard]] std::optional<int> rowForQueueId(std::uint32_t song_id) const;
    [[nodiscard]] std::optional<std::string> uriAt(int row) const;
    [[nodiscard]] qint64 totalDurationMs() const noexcept;
    [[nodiscard]] std::vector<mpd::Track> tracksSnapshot() const { return tracks_; }

    void replaceTracks(std::vector<mpd::Track> tracks);
    void setCurrentSongId(std::optional<std::uint32_t> song_id);
    void setArtworkEnabled(bool enabled);

  public slots:
    void acceptArtwork(quint64 token, const QImage& image);

  signals:
    void artworkRequested(quint64 token, const QString& uri);

  private:
    struct AlbumArtwork {
        QString uri;
        QImage image;
        bool requested{false};
        quint64 token{0U};
    };

    void synchronizeArtwork();
    void requestNextArtwork();

    std::vector<mpd::Track> tracks_;
    std::optional<std::uint32_t> current_song_id_;
    QHash<QString, AlbumArtwork> album_artwork_;
    std::optional<quint64> active_artwork_token_;
    quint64 artwork_generation_{0U};
    bool artwork_enabled_{false};
};

} // namespace trackknife::quick
