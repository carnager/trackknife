// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_transformation_preview_model.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/metadata/transformation.hpp"

#include <QAbstractItemModel>
#include <QModelIndex>
#include <QVariant>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace trackknife::bench {
namespace {

class MetadataTransformationPreviewModel final : public QAbstractItemModel {
  public:
    MetadataTransformationPreviewModel(
        std::shared_ptr<const metadata::MetadataTransformationPreview> preview,
        QStringList track_labels, QObject* parent = nullptr)
        : QAbstractItemModel(parent), preview_(std::move(preview)),
          track_labels_(std::move(track_labels)) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        if (!parent.isValid()) {
            return static_cast<int>(std::min(
                preview_->cells.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
        }
        return parent.column() == 0 && isChangeIndex(parent) ? 1 : 0;
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() && !isChangeIndex(parent) ? 0 : 3;
    }

    [[nodiscard]] QModelIndex index(const int row, const int column,
                                    const QModelIndex& parent = {}) const override {
        if (row < 0 || column < 0 || column >= 3) {
            return {};
        }
        if (!parent.isValid()) {
            if (row >= rowCount()) {
                return {};
            }
            return createIndex(row, column, changeId(row));
        }
        if (parent.column() != 0 || !isChangeIndex(parent) || row != 0) {
            return {};
        }
        return createIndex(row, column, detailId(parent.row()));
    }

    [[nodiscard]] QModelIndex parent(const QModelIndex& child) const override {
        if (!child.isValid() || isChangeIndex(child)) {
            return {};
        }
        const auto row = detailParentRow(child);
        if (row < 0 || row >= rowCount()) {
            return {};
        }
        return createIndex(row, 0, changeId(row));
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid()) {
            return {};
        }
        const auto cell_row = isChangeIndex(index) ? index.row() : detailParentRow(index);
        if (cell_row < 0 || static_cast<std::size_t>(cell_row) >= preview_->cells.size()) {
            return {};
        }
        const auto& cell = preview_->cells[static_cast<std::size_t>(cell_row)];
        if (role == Qt::ToolTipRole) {
            return isChangeIndex(index)
                       ? (cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                              ? QStringLiteral("Exact native field · expand to see the affected "
                                               "file and producing step")
                              : QStringLiteral(
                                    "Semantic field · expand to see the affected file and "
                                    "producing step"))
                       : QStringLiteral("Step %1 produced the final value for %2")
                             .arg(cell.last_action_index + 1U)
                             .arg(display_utf8(cell.display_field));
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        if (!isChangeIndex(index)) {
            switch (index.column()) {
            case 0:
                return QStringLiteral("File");
            case 1:
                if (cell.item_index < static_cast<std::size_t>(track_labels_.size())) {
                    return track_labels_.at(static_cast<qsizetype>(cell.item_index));
                }
                return QStringLiteral("File %1").arg(cell.item_index + 1U);
            case 2:
                return QStringLiteral("Produced by step %1").arg(cell.last_action_index + 1U);
            default:
                return {};
            }
        }
        switch (index.column()) {
        case 0:
            return cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                       ? QStringLiteral("Exact native: %1").arg(display_utf8(cell.display_field))
                       : display_utf8(cell.display_field);
        case 1:
            return cell.before ? display_plan_values(*cell.before) : QStringLiteral("(missing)");
        case 2:
            return cell.after ? display_plan_values(*cell.after) : QStringLiteral("(removed)");
        default:
            return {};
        }
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        constexpr std::array<std::string_view, 3> headings{"Field", "Old", "New"};
        return section >= 0 && static_cast<std::size_t>(section) < headings.size()
                   ? QVariant{display_utf8(headings[static_cast<std::size_t>(section)])}
                   : QVariant{};
    }

  private:
    [[nodiscard]] static quintptr changeId(const int row) noexcept {
        return (static_cast<quintptr>(row) * 2U) + 1U;
    }

    [[nodiscard]] static quintptr detailId(const int parent_row) noexcept {
        return (static_cast<quintptr>(parent_row) * 2U) + 2U;
    }

    [[nodiscard]] static bool isChangeIndex(const QModelIndex& index) noexcept {
        return index.isValid() && (index.internalId() & 1U) != 0U;
    }

    [[nodiscard]] static int detailParentRow(const QModelIndex& index) noexcept {
        if (!index.isValid() || isChangeIndex(index) || index.internalId() < 2U) {
            return -1;
        }
        const auto row = (index.internalId() - 2U) / 2U;
        return row > static_cast<quintptr>(std::numeric_limits<int>::max()) ? -1
                                                                            : static_cast<int>(row);
    }

    std::shared_ptr<const metadata::MetadataTransformationPreview> preview_;
    QStringList track_labels_;
};

} // namespace

QAbstractItemModel* createMetadataTransformationPreviewModel(
    std::shared_ptr<const metadata::MetadataTransformationPreview> preview,
    QStringList track_labels, QObject* parent) {
    return new MetadataTransformationPreviewModel(std::move(preview), std::move(track_labels),
                                                  parent);
}

} // namespace trackknife::bench
