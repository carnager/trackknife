// SPDX-License-Identifier: GPL-3.0-only

#include "bench/preparation_feedback_dialog.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace trackknife::bench {

QDialog* createPreparationFeedbackDialog(const QString& window_title, const QString& summary,
                                         const std::vector<PreparationFeedbackRow>& rows,
                                         QWidget* parent) {
    auto* dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("bench-preparation-feedback"));
    dialog->setWindowTitle(window_title);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(720, rows.empty() ? 140 : 340);

    auto* layout = new QVBoxLayout(dialog);
    auto* summary_label = new QLabel(summary, dialog);
    summary_label->setObjectName(QStringLiteral("bench-preparation-feedback-summary"));
    summary_label->setWordWrap(true);
    layout->addWidget(summary_label);

    if (!rows.empty()) {
        auto* table = new QTreeWidget(dialog);
        table->setObjectName(QStringLiteral("bench-preparation-feedback-table"));
        table->setAccessibleName(QStringLiteral("Files that need attention"));
        table->setColumnCount(2);
        table->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Problem")});
        table->setRootIsDecorated(false);
        table->setAlternatingRowColors(true);
        table->setUniformRowHeights(true);
        table->setTextElideMode(Qt::ElideMiddle);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        for (const auto& row : rows) {
            auto* item = new QTreeWidgetItem(table, {row.file, row.detail});
            item->setToolTip(0, row.file);
            item->setToolTip(1, row.detail);
        }
        layout->addWidget(table, 1);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->setObjectName(QStringLiteral("bench-preparation-feedback-buttons"));
    if (auto* close = buttons->button(QDialogButtonBox::Close)) {
        close->setObjectName(QStringLiteral("bench-preparation-feedback-close"));
    }
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    return dialog;
}

} // namespace trackknife::bench
