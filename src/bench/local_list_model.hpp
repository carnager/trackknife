// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractTableModel>

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
    std::optional<std::int64_t> duration_ms;
    // True once a probe ran or persisted metadata was restored; unprobed rows
    // fall back to their file name and are queued for enrichment.
    bool probed{false};

    friend bool operator==(const LocalTrackRow&, const LocalTrackRow&) = default;
};

// Table model over one Trackbench working list. Rows hold raw path bytes;
// presentation uses the lossless escaped form from the core.
class LocalListModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    enum Column {
        title_column = 0,
        artist_column = 1,
        album_column = 2,
        duration_column = 3,
        path_column = 4,
        column_count = 5,
    };
    enum Role { raw_path_role = Qt::UserRole + 1 };

    explicit LocalListModel(QObject* parent = nullptr);

    void replaceRows(std::vector<LocalTrackRow> rows);
    void appendPaths(std::vector<std::string> raw_paths, int insertion_row = -1);
    void removeRowIndexes(std::vector<int> rows);
    // Applies probe metadata to the row holding raw_path (hint first, then
    // search); returns false when the row no longer exists.
    bool applyMetadata(const std::string& raw_path, int hint_row, LocalTrackRow metadata);
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

  private:
    std::vector<LocalTrackRow> rows_;
};

} // namespace trackknife::bench
