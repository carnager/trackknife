// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_scalar_delegate.hpp"

#include "bench/metadata_grid_model.hpp"

#include <QAbstractItemModel>
#include <QLineEdit>
#include <QModelIndex>
#include <QStyledItemDelegate>

namespace trackknife::bench {
namespace {

class MetadataScalarDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    [[nodiscard]] QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                                        const QModelIndex& index) const override {
        if (index.column() <= 0) {
            return nullptr;
        }
        auto* editor = new QLineEdit(parent);
        editor->setFrame(false);
        return editor;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line == nullptr) {
            return;
        }
        const auto values = index.data(metadata_cell_values_role).toStringList();
        if (values.size() == 1) {
            line->setText(values.front());
            line->selectAll();
        } else {
            line->clear();
            line->setPlaceholderText(
                values.empty()
                    ? QStringLiteral("Type a value")
                    : QStringLiteral("Type to replace %1 exact values").arg(values.size()));
        }
        line->setModified(false);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model,
                      const QModelIndex& index) const override {
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line != nullptr && line->isModified()) {
            model->setData(index, line->text(), Qt::EditRole);
        }
    }
};

} // namespace

QAbstractItemDelegate* createMetadataScalarDelegate(QObject* parent) {
    return new MetadataScalarDelegate(parent);
}

} // namespace trackknife::bench
