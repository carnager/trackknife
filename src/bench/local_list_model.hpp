// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QImage>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// One row of a Trackbench working list: raw Linux path bytes plus the
// display metadata enriched by the background probe or restored from the
// persisted document.
struct LocalTrackRow {
    std::string raw_path;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_artist;
    std::string date;
    std::string track_number;
    std::optional<std::int64_t> duration_ms;
    // True once a probe ran or persisted metadata was restored; unprobed rows
    // fall back to their file name and are queued for enrichment.
    bool probed{false};

    friend bool operator==(const LocalTrackRow&, const LocalTrackRow&) = default;
};

// Table model over one Trackbench working list, implementing the shared
// album-grouped presentation contract (uicommon/track_row_roles.hpp) so the
// client's QueueItemDelegate/QueueTableView render it unchanged. Rows hold
// raw path bytes; presentation uses the lossless escaped form from the core.
class LocalListModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    explicit LocalListModel(QObject* parent = nullptr);

    void replaceRows(std::vector<LocalTrackRow> rows);
    void appendPaths(std::vector<std::string> raw_paths, int insertion_row = -1);
    void appendRows(std::vector<LocalTrackRow> rows, int insertion_row = -1);
    void removeRowIndexes(std::vector<int> rows);
    // Moves the given rows (ascending, deduplicated) as one block to
    // insertion_row, preserving their relative order.
    void reorderRows(std::vector<int> rows, int insertion_row);
    // Applies probe metadata to the row holding raw_path (hint first, then
    // search); returns false when the row no longer exists.
    bool applyMetadata(const std::string& raw_path, int hint_row, LocalTrackRow metadata);
    // Marks the playing occurrence rendered by the shared delegate; an empty
    // path clears it.
    void setCurrentPath(std::string raw_path, int hint_row);
    // The delegate's album-grouping identity for a row; artwork is keyed and
    // painted per group.
    [[nodiscard]] QString groupKey(int row) const;
    [[nodiscard]] bool hasArtwork(const QString& key) const { return artwork_.contains(key); }
    void setArtwork(const QString& key, QImage image);
    [[nodiscard]] const std::vector<LocalTrackRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] std::string rawPath(int row) const;
    // Finds the row holding raw_path, preferring the hint row so duplicate
    // occurrences re-anchor to the same position after edits.
    [[nodiscard]] int rowOfPath(const std::string& raw_path, int hint_row) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;

  private:
    void refreshCurrentRow();
    void emitRowChanged(int row);

    std::vector<LocalTrackRow> rows_;
    QHash<QString, QImage> artwork_;
    std::string current_path_;
    int current_row_{-1};
};

} // namespace trackknife::bench
