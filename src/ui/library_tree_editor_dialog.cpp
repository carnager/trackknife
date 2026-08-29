// SPDX-License-Identifier: GPL-3.0-only

#include "ui/library_tree_editor_dialog.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace trackknife::ui {

LibraryTreeEditorDialog::LibraryTreeEditorDialog(LibraryTreeDefinition definition, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("library-tree-editor"));
    setWindowTitle(QStringLiteral("Configure server library tree"));
    resize(980, 460);

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("library-tree-name"));
    form->addRow(QStringLiteral("Preset name:"), name_);
    root_tag_ = new QComboBox(this);
    root_tag_->setObjectName(QStringLiteral("library-tree-root-tag"));
    root_tag_->setEditable(true);
    root_tag_->addItems({QStringLiteral("AlbumArtist"), QStringLiteral("Artist"),
                         QStringLiteral("Genre"), QStringLiteral("Composer")});
    form->addRow(QStringLiteral("MPD root tag:"), root_tag_);
    layout->addLayout(form);

    auto* explanation = new QLabel(
        QStringLiteral("Each row is one structural level. The last row creates track leaves; "
                       "earlier rows group tracks. Labels and sorting use tkfmt-1."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    levels_ = new QTableWidget(this);
    levels_->setObjectName(QStringLiteral("library-tree-levels"));
    levels_->setColumnCount(5);
    levels_->setHorizontalHeaderLabels(
        {QStringLiteral("Level"), QStringLiteral("Grouping expression"),
         QStringLiteral("Label expression"), QStringLiteral("Sort expression"),
         QStringLiteral("Omit if single")});
    levels_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    levels_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    levels_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    levels_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    levels_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    levels_->verticalHeader()->hide();
    levels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    levels_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(levels_, 1);

    auto* row_actions = new QHBoxLayout;
    auto* add = new QPushButton(QStringLiteral("Add level"), this);
    auto* remove = new QPushButton(QStringLiteral("Remove level"), this);
    auto* up = new QPushButton(QStringLiteral("Move up"), this);
    auto* down = new QPushButton(QStringLiteral("Move down"), this);
    row_actions->addWidget(add);
    row_actions->addWidget(remove);
    row_actions->addWidget(up);
    row_actions->addWidget(down);
    row_actions->addStretch(1);
    layout->addLayout(row_actions);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &LibraryTreeEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QAbstractButton::clicked, this,
            [this] { populate(defaultLibraryTreeDefinition()); });
    connect(add, &QPushButton::clicked, this, [this] {
        addLevel({.name = QStringLiteral("Group"),
                  .grouping_expression = QStringLiteral("%genre%"),
                  .label_expression = QStringLiteral("$if2(%genre%,Unknown)"),
                  .sort_expression = QStringLiteral("$if2(%genre%,Unknown)")});
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (levels_->rowCount() <= 2) {
            QMessageBox::information(
                this, windowTitle(),
                QStringLiteral("Keep at least one group and one track level."));
            return;
        }
        const auto row = levels_->currentRow();
        if (row >= 0) {
            levels_->removeRow(row);
        }
    });
    connect(up, &QPushButton::clicked, this, [this] { moveCurrentLevel(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveCurrentLevel(1); });

    populate(definition);
}

LibraryTreeDefinition LibraryTreeEditorDialog::definition() const {
    LibraryTreeDefinition result;
    result.name = name_->text().trimmed();
    result.root_tag = root_tag_->currentText().trimmed();
    result.levels.reserve(static_cast<std::size_t>(levels_->rowCount()));
    for (int row = 0; row < levels_->rowCount(); ++row) {
        const auto* omit = qobject_cast<QCheckBox*>(levels_->cellWidget(row, 4));
        result.levels.push_back({.name = levels_->item(row, 0)->text().trimmed(),
                                 .grouping_expression = levels_->item(row, 1)->text(),
                                 .label_expression = levels_->item(row, 2)->text(),
                                 .sort_expression = levels_->item(row, 3)->text(),
                                 .omit_when_single = omit != nullptr && omit->isChecked()});
    }
    return result;
}

void LibraryTreeEditorDialog::accept() {
    auto candidate = definition();
    if (candidate.name.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Give the tree preset a name."));
        return;
    }
    ServerLibraryTreeModel validator;
    const auto error = validator.setDefinition(candidate);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QDialog::accept();
}

void LibraryTreeEditorDialog::populate(const LibraryTreeDefinition& definition) {
    name_->setText(definition.name);
    root_tag_->setCurrentText(definition.root_tag);
    levels_->setRowCount(0);
    for (const auto& level : definition.levels) {
        addLevel(level);
    }
    if (levels_->rowCount() > 0) {
        levels_->selectRow(0);
    }
}

void LibraryTreeEditorDialog::addLevel(LibraryTreeLevelDefinition level, int row) {
    if (row < 0) {
        row = levels_->rowCount();
    }
    levels_->insertRow(row);
    levels_->setItem(row, 0, new QTableWidgetItem(level.name));
    levels_->setItem(row, 1, new QTableWidgetItem(level.grouping_expression));
    levels_->setItem(row, 2, new QTableWidgetItem(level.label_expression));
    levels_->setItem(row, 3, new QTableWidgetItem(level.sort_expression));
    auto* omit = new QCheckBox(levels_);
    omit->setChecked(level.omit_when_single);
    omit->setToolTip(QStringLiteral("Skip this branch when all tracks share one value"));
    levels_->setCellWidget(row, 4, omit);
    levels_->selectRow(row);
}

void LibraryTreeEditorDialog::moveCurrentLevel(const int offset) {
    const auto source = levels_->currentRow();
    const auto target = source + offset;
    if (source < 0 || target < 0 || target >= levels_->rowCount()) {
        return;
    }
    auto current = definition();
    std::swap(current.levels[static_cast<std::size_t>(source)],
              current.levels[static_cast<std::size_t>(target)]);
    populate(current);
    levels_->selectRow(target);
}

} // namespace trackknife::ui
