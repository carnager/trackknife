// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QAbstractTableModel>

#include <string>
#include <vector>

namespace trackknife::bench {

// Table model over one Trackbench working list. Rows hold raw Linux path
// bytes; presentation uses the lossless escaped form from the core.
class LocalListModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    enum Column { title_column = 0, path_column = 1, column_count = 2 };
    enum Role { raw_path_role = Qt::UserRole + 1 };

    explicit LocalListModel(QObject* parent = nullptr);

    void replacePaths(std::vector<std::string> raw_paths);
    void appendPaths(std::vector<std::string> raw_paths, int insertion_row = -1);
    void removeRowIndexes(std::vector<int> rows);
    [[nodiscard]] const std::vector<std::string>& rawPaths() const noexcept { return raw_paths_; }
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
    std::vector<std::string> raw_paths_;
};

} // namespace trackknife::bench
