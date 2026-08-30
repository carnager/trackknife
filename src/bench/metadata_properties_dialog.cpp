// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_properties_dialog.hpp"

#include "bench/metadata_grid_model.hpp"
#include "trackknife/metadata/field_suggestions.hpp"

#include <QAbstractListModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace trackknife::bench {

struct MetadataApplyProgressState {
    mutable std::mutex mutex;
    std::vector<operations::MetadataApplySourceState> states;
    std::vector<std::optional<core::Error>> issues;
    std::size_t completed_sources{0U};
};

namespace {

[[nodiscard]] QString display_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] std::string encode_utf8(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}

[[nodiscard]] QString pluralized(const std::size_t count, const QString& singular,
                                 const QString& plural) {
    return count == 1U ? singular : plural;
}

[[nodiscard]] QString display_plan_values(const std::vector<std::string>& values) {
    constexpr auto maximum_visible_values = std::size_t{8U};
    constexpr auto maximum_visible_characters = qsizetype{512};
    QStringList visible;
    visible.reserve(static_cast<qsizetype>(std::min(values.size(), maximum_visible_values) +
                                           (values.size() > maximum_visible_values)));
    for (const auto& value : values | std::views::take(maximum_visible_values)) {
        auto display = value.empty() ? QStringLiteral("(empty value)") : display_utf8(value);
        if (display.size() > maximum_visible_characters) {
            display = display.left(maximum_visible_characters) + QChar{0x2026};
        }
        visible.push_back(std::move(display));
    }
    if (values.size() > maximum_visible_values) {
        visible.push_back(
            QStringLiteral("… +%1 values").arg(values.size() - maximum_visible_values));
    }
    return visible.join(QStringLiteral("  ·  "));
}

[[nodiscard]] bool has_conflict(const metadata::MetadataWritePlanSource& source) {
    return std::ranges::any_of(source.issues, [](const auto& issue) {
        return issue.blocking && issue.error.code == core::ErrorCode::conflict;
    });
}

[[nodiscard]] QString apply_state_text(const operations::MetadataApplySourceState state) {
    using State = operations::MetadataApplySourceState;
    switch (state) {
    case State::pending:
        return QStringLiteral("Waiting");
    case State::running:
        return QStringLiteral("Applying");
    case State::committed:
        return QStringLiteral("Applied");
    case State::failed:
        return QStringLiteral("Failed");
    case State::cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

} // namespace

class MetadataExactValueModel final : public QAbstractListModel {
  public:
    explicit MetadataExactValueModel(QStringList values, QObject* parent = nullptr)
        : QAbstractListModel(parent), values_(std::move(values)) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0
                                : static_cast<int>(std::min(
                                      static_cast<std::size_t>(values_.size()),
                                      static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= values_.size()) {
            return {};
        }
        const auto& value = values_[index.row()];
        if (role == Qt::DisplayRole) {
            return value.isEmpty() ? QVariant{QStringLiteral("(empty value)")} : QVariant{value};
        }
        if (role == Qt::EditRole) {
            return value;
        }
        if (role == Qt::ToolTipRole && value.isEmpty()) {
            return QStringLiteral("This is one explicit empty metadata value");
        }
        if (role == Qt::ForegroundRole && value.isEmpty()) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }

    bool setData(const QModelIndex& index, const QVariant& value, const int role) override {
        if (!index.isValid() || index.row() < 0 || index.row() >= values_.size() ||
            role != Qt::EditRole) {
            return false;
        }
        const auto replacement = value.toString();
        if (values_[index.row()] == replacement) {
            return false;
        }
        values_[index.row()] = replacement;
        emit dataChanged(index, index,
                         {Qt::DisplayRole, Qt::EditRole, Qt::ForegroundRole, Qt::ToolTipRole});
        return true;
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (role != Qt::DisplayRole) {
            return {};
        }
        return orientation == Qt::Horizontal ? QVariant{QStringLiteral("Exact value")}
                                             : QVariant{section + 1};
    }

    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override {
        return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable
                               : Qt::NoItemFlags;
    }

    [[nodiscard]] QModelIndex appendValue() {
        constexpr auto maximum_values = 16'384;
        if (values_.size() >= maximum_values) {
            return {};
        }
        const auto row = static_cast<int>(values_.size());
        beginInsertRows({}, row, row);
        values_.push_back(QString{});
        endInsertRows();
        return index(row, 0);
    }

    void removeIndexes(const QModelIndexList& indexes) {
        std::vector<int> rows;
        rows.reserve(static_cast<std::size_t>(indexes.size()));
        for (const auto& index : indexes) {
            if (index.isValid() && index.row() >= 0 && index.row() < values_.size()) {
                rows.push_back(index.row());
            }
        }
        std::sort(rows.begin(), rows.end(), std::greater<>{});
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        for (const auto row : rows) {
            beginRemoveRows({}, row, row);
            values_.removeAt(row);
            endRemoveRows();
        }
    }

    [[nodiscard]] QModelIndex moveUp(const QModelIndex& current) {
        if (!current.isValid() || current.row() <= 0 || current.row() >= values_.size()) {
            return current;
        }
        const auto row = current.row();
        beginMoveRows({}, row, row, {}, row - 1);
        values_.move(row, row - 1);
        endMoveRows();
        return index(row - 1, 0);
    }

    [[nodiscard]] QModelIndex moveDown(const QModelIndex& current) {
        if (!current.isValid() || current.row() < 0 || current.row() + 1 >= values_.size()) {
            return current;
        }
        const auto row = current.row();
        beginMoveRows({}, row, row, {}, row + 2);
        values_.move(row, row + 1);
        endMoveRows();
        return index(row + 1, 0);
    }

    [[nodiscard]] const QStringList& values() const noexcept { return values_; }

  private:
    QStringList values_;
};

class MetadataExactValueDialog final : public QDialog {
  public:
    MetadataExactValueDialog(const QString& heading, const QString& context, QStringList values,
                             QWidget* parent)
        : QDialog(parent) {
        setObjectName(QStringLiteral("bench-metadata-exact-values"));
        setWindowTitle(QStringLiteral("Edit exact metadata values"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(640, 420);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        auto* title = new QLabel(heading, this);
        title->setObjectName(QStringLiteral("bench-metadata-exact-values-heading"));
        layout->addWidget(title);
        auto* explanation = new QLabel(context, this);
        explanation->setObjectName(QStringLiteral("bench-metadata-exact-values-context"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        table_ = new QTableView(this);
        table_->setObjectName(QStringLiteral("bench-metadata-exact-values-table"));
        model_ = new MetadataExactValueModel(std::move(values), table_);
        table_->setModel(model_);
        table_->setAlternatingRowColors(true);
        table_->setShowGrid(false);
        table_->setWordWrap(false);
        table_->setTextElideMode(Qt::ElideRight);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                QAbstractItemView::EditKeyPressed |
                                QAbstractItemView::AnyKeyPressed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->verticalHeader()->setDefaultSectionSize(24);
        layout->addWidget(table_, 1);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        add_ = new QPushButton(QStringLiteral("Add value"), this);
        add_->setObjectName(QStringLiteral("bench-metadata-exact-values-add"));
        remove_ = new QPushButton(QStringLiteral("Remove value"), this);
        remove_->setObjectName(QStringLiteral("bench-metadata-exact-values-remove"));
        up_ = new QPushButton(QStringLiteral("Move up"), this);
        up_->setObjectName(QStringLiteral("bench-metadata-exact-values-up"));
        down_ = new QPushButton(QStringLiteral("Move down"), this);
        down_->setObjectName(QStringLiteral("bench-metadata-exact-values-down"));
        actions->addWidget(add_);
        actions->addWidget(remove_);
        actions->addWidget(up_);
        actions->addWidget(down_);
        actions->addStretch(1);
        layout->addLayout(actions);

        auto* note = new QLabel(
            QStringLiteral("Each row is one value. Empty rows are preserved; use Delete in "
                           "Properties to remove the field."),
            this);
        note->setWordWrap(true);
        layout->addWidget(note);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons_->setObjectName(QStringLiteral("bench-metadata-exact-values-buttons"));
        connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons_);

        connect(add_, &QPushButton::clicked, this, [this] {
            const auto added = model_->appendValue();
            if (added.isValid()) {
                table_->setCurrentIndex(added);
                table_->edit(added);
            }
            updateActions();
        });
        connect(remove_, &QPushButton::clicked, this, [this] {
            model_->removeIndexes(table_->selectionModel()->selectedRows());
            updateActions();
        });
        connect(up_, &QPushButton::clicked, this, [this] {
            table_->setCurrentIndex(model_->moveUp(table_->currentIndex()));
            updateActions();
        });
        connect(down_, &QPushButton::clicked, this, [this] {
            table_->setCurrentIndex(model_->moveDown(table_->currentIndex()));
            updateActions();
        });
        connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this] { updateActions(); });
        connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this] { updateActions(); });
        connect(model_, &QAbstractItemModel::rowsInserted, this, [this] { updateActions(); });
        connect(model_, &QAbstractItemModel::rowsRemoved, this, [this] { updateActions(); });
        if (model_->rowCount() > 0) {
            table_->setCurrentIndex(model_->index(0, 0));
        }
        updateActions();
    }

    [[nodiscard]] std::vector<std::string> values() const {
        std::vector<std::string> result;
        result.reserve(static_cast<std::size_t>(model_->values().size()));
        for (const auto& value : model_->values()) {
            const auto encoded = value.toUtf8();
            result.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
        }
        return result;
    }

  private:
    void updateActions() {
        const auto current = table_->currentIndex();
        const auto has_selection = !table_->selectionModel()->selectedRows().empty();
        add_->setEnabled(model_->rowCount() < 16'384);
        remove_->setEnabled(has_selection);
        up_->setEnabled(current.isValid() && current.row() > 0);
        down_->setEnabled(current.isValid() && current.row() + 1 < model_->rowCount());
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(model_->rowCount() > 0);
    }

    MetadataExactValueModel* model_{nullptr};
    QTableView* table_{nullptr};
    QPushButton* add_{nullptr};
    QPushButton* remove_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

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
                       ? QStringLiteral("Expand to see the affected file and producing step")
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
            return display_utf8(cell.display_field);
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

class MetadataTransformationDialog final : public QDialog {
  public:
    using StageCallback =
        std::function<bool(const metadata::MetadataTransformationPreview& preview)>;
    using PreviewResult = core::Result<metadata::MetadataTransformationPreview>;

    MetadataTransformationDialog(std::shared_ptr<const metadata::StagedMetadataSelection> selection,
                                 metadata::StagedMetadataPatchSet draft,
                                 std::vector<std::size_t> item_indexes, QStringList track_labels,
                                 StageCallback stage, MetadataTransformationStore store,
                                 QWidget* parent,
                                 std::optional<core::StableId> initially_selected = std::nullopt,
                                 const bool preview_initially_selected = false)
        : QDialog(parent), watcher_(this), selection_(std::move(selection)),
          draft_(std::move(draft)), item_indexes_(std::move(item_indexes)),
          track_labels_(std::move(track_labels)), stage_(std::move(stage)),
          store_(std::move(store)), initially_selected_(initially_selected),
          preview_initially_selected_(preview_initially_selected) {
        setObjectName(QStringLiteral("bench-metadata-transformation"));
        setWindowTitle(QStringLiteral("Metadata transformation"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(880, 650);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        auto* explanation = new QLabel(
            QStringLiteral("Build an ordered chain. Each step sees the result of every earlier "
                           "step; nothing enters the metadata draft until you preview and stage "
                           "the final changes."),
            this);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        auto* saved_form = new QFormLayout;
        saved_ = new QComboBox(this);
        saved_->setObjectName(QStringLiteral("bench-metadata-transformation-saved"));
        saved_->addItem(QStringLiteral("New unsaved chain"));
        saved_form->addRow(QStringLiteral("Saved chain:"), saved_);
        layout->addLayout(saved_form);
        auto* saved_row = new QHBoxLayout;
        save_ = new QPushButton(QStringLiteral("Save"), this);
        save_->setObjectName(QStringLiteral("bench-metadata-transformation-save"));
        save_as_ = new QPushButton(QStringLiteral("Save as new"), this);
        save_as_->setObjectName(QStringLiteral("bench-metadata-transformation-save-as-new"));
        delete_saved_ = new QPushButton(QStringLiteral("Delete saved"), this);
        delete_saved_->setObjectName(QStringLiteral("bench-metadata-transformation-delete"));
        saved_row->addWidget(save_);
        saved_row->addWidget(save_as_);
        saved_row->addWidget(delete_saved_);
        saved_row->addStretch(1);
        layout->addLayout(saved_row);
        catalog_status_ = new QLabel(this);
        catalog_status_->setObjectName(
            QStringLiteral("bench-metadata-transformation-catalog-status"));
        catalog_status_->setWordWrap(true);
        layout->addWidget(catalog_status_);

        auto* name_form = new QFormLayout;
        name_ = new QLineEdit(QStringLiteral("Ad hoc transformation"), this);
        name_->setObjectName(QStringLiteral("bench-metadata-transformation-name"));
        name_form->addRow(QStringLiteral("Chain name:"), name_);
        layout->addLayout(name_form);

        auto* step_form = new QFormLayout;
        kind_ = new QComboBox(this);
        kind_->setObjectName(QStringLiteral("bench-metadata-transformation-kind"));
        kind_->addItems(
            {QStringLiteral("Set one literal value"), QStringLiteral("Add one literal value"),
             QStringLiteral("Remove field"), QStringLiteral("Trim each value"),
             QStringLiteral("Lowercase each value"), QStringLiteral("Uppercase each value"),
             QStringLiteral("Capitalize first character"), QStringLiteral("Copy another field"),
             QStringLiteral("Split by exact separator"),
             QStringLiteral("Join with exact separator"), QStringLiteral("Format with tkfmt-1"),
             QStringLiteral("Remove exact matching values"),
             QStringLiteral("Replace exact matching values"),
             QStringLiteral("Number by selected-file order")});
        target_ = new QLineEdit(this);
        target_->setObjectName(QStringLiteral("bench-metadata-transformation-target"));
        target_->setPlaceholderText(QStringLiteral("For example: Title or ALBUM ARTIST"));
        target_field_candidates_.reserve(selection_->field_count());
        for (std::size_t index = 0U; index < selection_->field_count(); ++index) {
            const auto& field = selection_->field(index);
            if (field.present_item_count > 0U) {
                target_field_candidates_.push_back(metadata::MetadataFieldSuggestionCandidate{
                    .display_name = field.display_name,
                    .kind = metadata::MetadataFieldSuggestionKind::present,
                });
            }
        }
        target_completion_model_ = new QStringListModel(this);
        target_completion_model_->setObjectName(
            QStringLiteral("bench-metadata-transformation-target-completions"));
        target_completer_ = new QCompleter(target_completion_model_, this);
        target_completer_->setObjectName(
            QStringLiteral("bench-metadata-transformation-target-completer"));
        target_completer_->setCaseSensitivity(Qt::CaseInsensitive);
        target_completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
        target_completer_->setMaxVisibleItems(12);
        target_->setCompleter(target_completer_);
        target_completion_model_->setStringList(targetFieldSuggestions({}));
        input_label_ = new QLabel(QStringLiteral("Value:"), this);
        input_ = new QLineEdit(this);
        input_->setObjectName(QStringLiteral("bench-metadata-transformation-input"));
        replacement_label_ = new QLabel(QStringLiteral("Replacement:"), this);
        replacement_ = new QLineEdit(this);
        replacement_->setObjectName(QStringLiteral("bench-metadata-transformation-replacement"));
        number_start_label_ = new QLabel(QStringLiteral("Start at:"), this);
        number_start_ = new QSpinBox(this);
        number_start_->setObjectName(QStringLiteral("bench-metadata-transformation-number-start"));
        number_start_->setRange(1, 1'000'000'000);
        number_start_->setValue(1);
        number_padding_label_ = new QLabel(QStringLiteral("Minimum width:"), this);
        number_padding_ = new QSpinBox(this);
        number_padding_->setObjectName(
            QStringLiteral("bench-metadata-transformation-number-padding"));
        number_padding_->setRange(0, 32);
        number_padding_->setValue(0);
        step_form->addRow(QStringLiteral("New step:"), kind_);
        step_form->addRow(QStringLiteral("Target field:"), target_);
        step_form->addRow(input_label_, input_);
        step_form->addRow(replacement_label_, replacement_);
        step_form->addRow(number_start_label_, number_start_);
        step_form->addRow(number_padding_label_, number_padding_);
        layout->addLayout(step_form);

        auto* add_row = new QHBoxLayout;
        add_ = new QPushButton(QStringLiteral("Add step"), this);
        add_->setObjectName(QStringLiteral("bench-metadata-transformation-add"));
        add_row->addWidget(add_);
        add_row->addStretch(1);
        layout->addLayout(add_row);

        steps_ = new QListWidget(this);
        steps_->setObjectName(QStringLiteral("bench-metadata-transformation-steps"));
        steps_->setAlternatingRowColors(true);
        layout->addWidget(steps_, 1);

        auto* order_row = new QHBoxLayout;
        remove_ = new QPushButton(QStringLiteral("Remove step"), this);
        remove_->setObjectName(QStringLiteral("bench-metadata-transformation-remove"));
        up_ = new QPushButton(QStringLiteral("Move up"), this);
        up_->setObjectName(QStringLiteral("bench-metadata-transformation-up"));
        down_ = new QPushButton(QStringLiteral("Move down"), this);
        down_->setObjectName(QStringLiteral("bench-metadata-transformation-down"));
        order_row->addWidget(remove_);
        order_row->addWidget(up_);
        order_row->addWidget(down_);
        order_row->addStretch(1);
        preview_button_ = new QPushButton(QStringLiteral("Preview final changes"), this);
        preview_button_->setObjectName(QStringLiteral("bench-metadata-transformation-preview"));
        order_row->addWidget(preview_button_);
        layout->addLayout(order_row);

        summary_ = new QLabel(QStringLiteral("Add at least one step to preview."), this);
        summary_->setObjectName(QStringLiteral("bench-metadata-transformation-summary"));
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        table_ = new QTreeView(this);
        table_->setObjectName(QStringLiteral("bench-metadata-transformation-table"));
        table_->setAlternatingRowColors(true);
        table_->setWordWrap(false);
        table_->setTextElideMode(Qt::ElideRight);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setRootIsDecorated(true);
        table_->setItemsExpandable(true);
        table_->setExpandsOnDoubleClick(true);
        table_->setUniformRowHeights(true);
        table_->setIndentation(18);
        table_->header()->setSectionResizeMode(QHeaderView::Interactive);
        table_->header()->setStretchLastSection(true);
        table_->setColumnWidth(0, 190);
        table_->setColumnWidth(1, 280);
        layout->addWidget(table_, 2);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
        stage_button_ =
            buttons_->addButton(QStringLiteral("Stage changes"), QDialogButtonBox::AcceptRole);
        stage_button_->setObjectName(QStringLiteral("bench-metadata-transformation-stage"));
        stage_button_->setEnabled(false);
        layout->addWidget(buttons_);

        connect(kind_, &QComboBox::currentIndexChanged, this, [this] { updateInputForKind(); });
        connect(target_, &QLineEdit::textChanged, this, [this](const QString& text) {
            target_completion_model_->setStringList(targetFieldSuggestions(text));
            if (text.trimmed().isEmpty() || target_completion_model_->rowCount() == 0) {
                return;
            }
            QTimer::singleShot(0, this, [this] {
                if (isVisible() && target_->hasFocus()) {
                    target_completer_->complete();
                }
            });
        });
        connect(name_, &QLineEdit::textChanged, this, [this] {
            invalidatePreview();
            updateActions();
        });
        connect(saved_, &QComboBox::currentIndexChanged, this,
                [this](const int index) { selectSaved(index); });
        connect(save_, &QPushButton::clicked, this, [this] { saveCurrent(false); });
        connect(save_as_, &QPushButton::clicked, this, [this] { saveCurrent(true); });
        connect(delete_saved_, &QPushButton::clicked, this, [this] { deleteSaved(); });
        connect(add_, &QPushButton::clicked, this, [this] { addStep(); });
        connect(remove_, &QPushButton::clicked, this, [this] { removeStep(); });
        connect(up_, &QPushButton::clicked, this, [this] { moveStep(-1); });
        connect(down_, &QPushButton::clicked, this, [this] { moveStep(1); });
        connect(steps_, &QListWidget::currentRowChanged, this, [this] { updateActions(); });
        connect(preview_button_, &QPushButton::clicked, this, [this] { startPreview(); });
        connect(stage_button_, &QPushButton::clicked, this, [this] { stagePreview(); });
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(&watcher_, &QFutureWatcherBase::finished, this, [this] { finishPreview(); });
        updateInputForKind();
        updateActions();
        loadSaved();
    }

    ~MetadataTransformationDialog() override {
        cancellation_.request_cancellation();
        if (planning_) {
            watcher_.waitForFinished();
        }
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (!planning_) {
            QDialog::closeEvent(event);
            return;
        }
        close_requested_ = true;
        cancellation_.request_cancellation();
        summary_->setText(QStringLiteral("Cancelling transformation preview…"));
        event->ignore();
    }

  private:
    [[nodiscard]] QStringList targetFieldSuggestions(const QString& query) const {
        const auto encoded = query.toUtf8();
        const auto suggestions = metadata::suggest_metadata_field_names(
            std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())},
            target_field_candidates_);
        QStringList names;
        names.reserve(static_cast<qsizetype>(suggestions.size()));
        for (const auto& suggestion : suggestions) {
            names.push_back(display_utf8(suggestion.display_name));
        }
        return names;
    }

    [[nodiscard]] QString actionText(const metadata::MetadataTransformationAction& action,
                                     const std::size_t index) const {
        return std::visit(
            [index](const auto& typed) {
                using Action = std::decay_t<decltype(typed)>;
                const auto field = display_utf8(typed.target_field);
                if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction>) {
                    return QStringLiteral("%1. Set %2 to %3")
                        .arg(index + 1U)
                        .arg(field, display_plan_values(typed.values));
                } else if constexpr (std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                    return QStringLiteral("%1. Add %3 to %2")
                        .arg(index + 1U)
                        .arg(field, display_plan_values(typed.values));
                } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldAction>) {
                    return QStringLiteral("%1. Remove %2").arg(index + 1U).arg(field);
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataTransformValuesAction>) {
                    QString verb;
                    switch (typed.transform) {
                    case metadata::MetadataValueTransformKind::trim_ascii:
                        verb = QStringLiteral("Trim each value of");
                        break;
                    case metadata::MetadataValueTransformKind::lowercase:
                        verb = QStringLiteral("Lowercase each value of");
                        break;
                    case metadata::MetadataValueTransformKind::uppercase:
                        verb = QStringLiteral("Uppercase each value of");
                        break;
                    case metadata::MetadataValueTransformKind::capitalize_first:
                        verb = QStringLiteral("Capitalize first character of each value of");
                        break;
                    }
                    return QStringLiteral("%1. %2 %3").arg(index + 1U).arg(verb, field);
                } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                    return QStringLiteral("%1. Copy %3 to %2")
                        .arg(index + 1U)
                        .arg(field, display_utf8(typed.source_field));
                } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                    return QStringLiteral("%1. Split %2 by %3")
                        .arg(index + 1U)
                        .arg(field, display_utf8(typed.separator));
                } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                    const auto separator = typed.separator.empty()
                                               ? QStringLiteral("(empty separator)")
                                               : display_utf8(typed.separator);
                    return QStringLiteral("%1. Join %2 with %3")
                        .arg(index + 1U)
                        .arg(field, separator);
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataRemoveMatchingValuesAction>) {
                    const auto match = typed.match.empty() ? QStringLiteral("(empty value)")
                                                           : display_utf8(typed.match);
                    return QStringLiteral("%1. Remove values of %2 equal to %3")
                        .arg(index + 1U)
                        .arg(field, match);
                } else if constexpr (std::is_same_v<
                                         Action, metadata::MetadataReplaceMatchingValuesAction>) {
                    const auto match = typed.match.empty() ? QStringLiteral("(empty value)")
                                                           : display_utf8(typed.match);
                    return QStringLiteral("%1. Replace values of %2 equal to %3 with %4")
                        .arg(index + 1U)
                        .arg(field, match, display_plan_values(typed.replacement_values));
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataNumberSelectedItemsAction>) {
                    return typed.padding == 0U
                               ? QStringLiteral("%1. Number %2 from %3 by selected-file order")
                                     .arg(index + 1U)
                                     .arg(field)
                                     .arg(typed.start)
                               : QStringLiteral("%1. Number %2 from %3 by selected-file order "
                                                "(minimum width %4)")
                                     .arg(index + 1U)
                                     .arg(field)
                                     .arg(typed.start)
                                     .arg(typed.padding);
                } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                    return QStringLiteral("%1. Format %2 as %3")
                        .arg(index + 1U)
                        .arg(field, display_utf8(typed.source));
                }
                return QString{};
            },
            action);
    }

    [[nodiscard]] QString capitalizationNoChangeSummary() const {
        if (!preview_ || actions_.empty()) {
            return {};
        }
        for (const auto& action : actions_) {
            const auto* transform = std::get_if<metadata::MetadataTransformValuesAction>(&action);
            if (transform == nullptr ||
                transform->transform != metadata::MetadataValueTransformKind::capitalize_first) {
                return {};
            }
        }

        const auto present = preview_->unchanged_present_cell_count;
        const auto missing = preview_->unchanged_missing_cell_count;
        QString target;
        if (actions_.size() == 1U) {
            target = display_utf8(
                std::get<metadata::MetadataTransformValuesAction>(actions_.front()).target_field);
        }
        if (present == 0U) {
            return target.isEmpty()
                       ? QStringLiteral("No changes: none of the selected files contains the "
                                        "targeted fields; missing fields are skipped.")
                       : QStringLiteral("No changes: none of the selected files contains %1; "
                                        "missing fields are skipped.")
                             .arg(target);
        }

        auto message = target.isEmpty()
                           ? QStringLiteral("No changes: every existing targeted value already "
                                            "starts with its uppercase form.")
                           : QStringLiteral("No changes: every existing %1 value already starts "
                                            "with its uppercase form.")
                                 .arg(target);
        if (missing > 0U) {
            const auto verb = missing == 1U ? QStringLiteral("was") : QStringLiteral("were");
            message += QStringLiteral(" %1 targeted %2 %3 missing and %4 skipped.")
                           .arg(missing)
                           .arg(missing == 1U ? QStringLiteral("field") : QStringLiteral("fields"))
                           .arg(verb, verb);
        }
        return message;
    }

    void rebuildSteps(const int selected_row) {
        steps_->clear();
        for (std::size_t index = 0U; index < actions_.size(); ++index) {
            steps_->addItem(actionText(actions_[index], index));
        }
        if (!actions_.empty()) {
            steps_->setCurrentRow(
                std::clamp(selected_row, 0, static_cast<int>(actions_.size()) - 1));
        }
        updateActions();
    }

    void repopulateSaved(const std::optional<core::StableId>& selected = std::nullopt) {
        std::ranges::sort(catalog_, [](const auto& left, const auto& right) {
            if (left.chain.name != right.chain.name) {
                return left.chain.name < right.chain.name;
            }
            return left.id.to_string() < right.id.to_string();
        });
        const QSignalBlocker blocker{saved_};
        saved_->clear();
        saved_->addItem(QStringLiteral("New unsaved chain"));
        auto selected_index = 0;
        for (std::size_t index = 0U; index < catalog_.size(); ++index) {
            const auto& entry = catalog_[index];
            saved_->addItem(display_utf8(entry.chain.name),
                            QString::fromStdString(entry.id.to_string()));
            if (selected && entry.id == *selected) {
                selected_index = static_cast<int>(index) + 1;
            }
        }
        saved_->setCurrentIndex(selected_index);
        selected_saved_ = selected_index > 0 ? selected : std::nullopt;
        updateActions();
    }

    void loadSaved() {
        if (!store_.load) {
            catalog_status_->setText(
                QStringLiteral("Saved chains are unavailable in this session."));
            updateActions();
            return;
        }
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Loading saved chains…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        store_.load([self](std::vector<persistence::SavedMetadataTransformationChain> chains,
                           QString error) mutable {
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!error.isEmpty()) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not load saved chains · %1").arg(error));
                self->updateActions();
                return;
            }
            self->catalog_ = std::move(chains);
            self->repopulateSaved(self->initially_selected_);
            if (self->initially_selected_) {
                self->selectSaved(self->saved_->currentIndex());
                if (self->preview_initially_selected_ && !self->actions_.empty()) {
                    QTimer::singleShot(0, self, [self] {
                        if (self) {
                            self->startPreview();
                        }
                    });
                }
            }
            self->catalog_status_->setText(QStringLiteral("%1 saved %2 available")
                                               .arg(self->catalog_.size())
                                               .arg(self->catalog_.size() == 1U
                                                        ? QStringLiteral("chain")
                                                        : QStringLiteral("chains")));
        });
    }

    void selectSaved(const int index) {
        if (catalog_busy_) {
            return;
        }
        if (index <= 0 || static_cast<std::size_t>(index) > catalog_.size()) {
            selected_saved_.reset();
            name_->setText(QStringLiteral("Ad hoc transformation"));
            actions_.clear();
            invalidatePreview();
            rebuildSteps(-1);
            catalog_status_->setText(QStringLiteral("Editing a new unsaved chain"));
            return;
        }
        const auto& selected = catalog_[static_cast<std::size_t>(index) - 1U];
        selected_saved_ = selected.id;
        name_->setText(display_utf8(selected.chain.name));
        actions_ = selected.chain.actions;
        invalidatePreview();
        rebuildSteps(0);
        catalog_status_->setText(
            QStringLiteral("Loaded saved chain · %1").arg(display_utf8(selected.chain.name)));
    }

    [[nodiscard]] metadata::MetadataTransformationChain currentChain() const {
        return metadata::MetadataTransformationChain{
            .schema_version = 1U,
            .name = encode_utf8(name_->text()),
            .actions = actions_,
        };
    }

    void saveCurrent(const bool as_new) {
        if (catalog_busy_ || !store_.save || actions_.empty()) {
            return;
        }
        if (name_->text().trimmed().isEmpty()) {
            catalog_status_->setText(QStringLiteral("Enter a non-empty chain name before saving."));
            name_->setFocus(Qt::OtherFocusReason);
            return;
        }
        auto chain = currentChain();
        if (const auto valid = metadata::validate_metadata_transformation_chain(chain); !valid) {
            catalog_status_->setText(
                QStringLiteral("Cannot save chain · %1").arg(display_utf8(valid.error().message)));
            return;
        }
        persistence::SavedMetadataTransformationChain saved_chain{
            .id = !as_new && selected_saved_ ? *selected_saved_ : core::StableId::random(),
            .chain = std::move(chain),
            .automatic = false,
        };
        if (!as_new && selected_saved_) {
            const auto existing = std::ranges::find(
                catalog_, *selected_saved_, &persistence::SavedMetadataTransformationChain::id);
            if (existing != catalog_.end()) {
                saved_chain.automatic = existing->automatic;
            }
        }
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Saving chain…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        auto retained_chain = saved_chain;
        store_.save(std::move(saved_chain), [self, saved_chain = std::move(retained_chain)](
                                                QString error) mutable {
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!error.isEmpty()) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not save chain · %1").arg(error));
                self->updateActions();
                return;
            }
            const auto found = std::ranges::find(
                self->catalog_, saved_chain.id, &persistence::SavedMetadataTransformationChain::id);
            if (found == self->catalog_.end()) {
                self->catalog_.push_back(saved_chain);
            } else {
                *found = saved_chain;
            }
            self->repopulateSaved(saved_chain.id);
            self->catalog_status_->setText(
                QStringLiteral("Saved chain · %1").arg(display_utf8(saved_chain.chain.name)));
        });
    }

    void deleteSaved() {
        if (catalog_busy_ || !store_.remove || !selected_saved_) {
            return;
        }
        const auto id = *selected_saved_;
        catalog_busy_ = true;
        catalog_status_->setText(QStringLiteral("Deleting saved chain…"));
        updateActions();
        const QPointer<MetadataTransformationDialog> self{this};
        store_.remove(id, [self, id](QString error) {
            if (!self) {
                return;
            }
            self->catalog_busy_ = false;
            if (!error.isEmpty()) {
                self->catalog_status_->setText(
                    QStringLiteral("Could not delete saved chain · %1").arg(error));
                self->updateActions();
                return;
            }
            std::erase_if(self->catalog_, [id](const auto& entry) { return entry.id == id; });
            self->selected_saved_.reset();
            self->repopulateSaved();
            self->name_->setText(QStringLiteral("Ad hoc transformation"));
            self->actions_.clear();
            self->invalidatePreview();
            self->rebuildSteps(-1);
            self->catalog_status_->setText(QStringLiteral("Saved chain deleted"));
        });
    }

    void updateInputForKind() {
        const auto kind = kind_->currentIndex();
        const auto has_input = kind == 0 || kind == 1 || (kind >= 7 && kind <= 12);
        input_label_->setVisible(has_input);
        input_->setVisible(has_input);
        const auto has_replacement = kind == 12;
        replacement_label_->setVisible(has_replacement);
        replacement_->setVisible(has_replacement);
        const auto has_numbering = kind == 13;
        number_start_label_->setVisible(has_numbering);
        number_start_->setVisible(has_numbering);
        number_padding_label_->setVisible(has_numbering);
        number_padding_->setVisible(has_numbering);
        if (kind == 7) {
            input_label_->setText(QStringLiteral("Source field:"));
            input_->setPlaceholderText(QStringLiteral("For example: Artist"));
        } else if (kind == 8 || kind == 9) {
            input_label_->setText(QStringLiteral("Separator:"));
            input_->setPlaceholderText(kind == 8 ? QStringLiteral("Required exact separator")
                                                 : QStringLiteral("May be empty"));
        } else if (kind == 10) {
            input_label_->setText(QStringLiteral("Expression:"));
            input_->setPlaceholderText(QStringLiteral("For example: %artist% — %title%"));
        } else if (kind == 11 || kind == 12) {
            input_label_->setText(QStringLiteral("Exact value:"));
            input_->setPlaceholderText(QStringLiteral("Case-sensitive; may be empty"));
        } else {
            input_label_->setText(QStringLiteral("Value:"));
            input_->setPlaceholderText(QString{});
        }
    }

    void addStep() {
        const auto field = target_->text().trimmed();
        if (field.isEmpty()) {
            summary_->setText(QStringLiteral("Enter a target field before adding the step."));
            target_->setFocus(Qt::OtherFocusReason);
            return;
        }
        const auto target = encode_utf8(field);
        switch (kind_->currentIndex()) {
        case 0:
            actions_.push_back(metadata::MetadataSetValuesAction{
                .target_field = target, .values = {encode_utf8(input_->text())}});
            break;
        case 1:
            actions_.push_back(metadata::MetadataAddValuesAction{
                .target_field = target, .values = {encode_utf8(input_->text())}});
            break;
        case 2:
            actions_.push_back(metadata::MetadataRemoveFieldAction{.target_field = target});
            break;
        case 3:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::trim_ascii});
            break;
        case 4:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::lowercase});
            break;
        case 5:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::uppercase});
            break;
        case 6:
            actions_.push_back(metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = metadata::MetadataValueTransformKind::capitalize_first});
            break;
        case 7: {
            const auto source = input_->text().trimmed();
            if (source.isEmpty()) {
                summary_->setText(QStringLiteral("Enter a source field for the copy step."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            actions_.push_back(metadata::MetadataCopyFieldAction{
                .target_field = target, .source_field = encode_utf8(source)});
            break;
        }
        case 8:
            if (input_->text().isEmpty()) {
                summary_->setText(QStringLiteral("Split requires a non-empty exact separator."));
                input_->setFocus(Qt::OtherFocusReason);
                return;
            }
            actions_.push_back(metadata::MetadataSplitValuesAction{
                .target_field = target, .separator = encode_utf8(input_->text())});
            break;
        case 9:
            actions_.push_back(metadata::MetadataJoinValuesAction{
                .target_field = target, .separator = encode_utf8(input_->text())});
            break;
        case 10:
            actions_.push_back(metadata::MetadataFormatValueAction{
                .target_field = target, .dialect = {}, .source = encode_utf8(input_->text())});
            break;
        case 11:
            actions_.push_back(metadata::MetadataRemoveMatchingValuesAction{
                .target_field = target, .match = encode_utf8(input_->text())});
            break;
        case 12:
            actions_.push_back(metadata::MetadataReplaceMatchingValuesAction{
                .target_field = target,
                .match = encode_utf8(input_->text()),
                .replacement_values = {encode_utf8(replacement_->text())},
            });
            break;
        case 13:
            actions_.push_back(metadata::MetadataNumberSelectedItemsAction{
                .target_field = target,
                .start = static_cast<std::uint32_t>(number_start_->value()),
                .padding = static_cast<std::uint32_t>(number_padding_->value()),
            });
            break;
        default:
            return;
        }
        invalidatePreview();
        rebuildSteps(static_cast<int>(actions_.size()) - 1);
        target_->clear();
        input_->clear();
        replacement_->clear();
        target_->setFocus(Qt::OtherFocusReason);
    }

    void removeStep() {
        const auto row = steps_->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= actions_.size()) {
            return;
        }
        actions_.erase(actions_.begin() + row);
        invalidatePreview();
        rebuildSteps(std::min(row, static_cast<int>(actions_.size()) - 1));
    }

    void moveStep(const int offset) {
        const auto row = steps_->currentRow();
        const auto destination = row + offset;
        if (row < 0 || destination < 0 ||
            static_cast<std::size_t>(destination) >= actions_.size()) {
            return;
        }
        std::swap(actions_[static_cast<std::size_t>(row)],
                  actions_[static_cast<std::size_t>(destination)]);
        invalidatePreview();
        rebuildSteps(destination);
    }

    void invalidatePreview() {
        preview_.reset();
        if (auto* model = table_->model()) {
            table_->setModel(nullptr);
            model->deleteLater();
        }
        stage_button_->setEnabled(false);
        if (!planning_) {
            summary_->setText(actions_.empty()
                                  ? QStringLiteral("Add at least one step to preview.")
                                  : QStringLiteral("Preview required after chain changes."));
        }
    }

    void updateActions() {
        const auto row = steps_->currentRow();
        const auto valid = row >= 0 && static_cast<std::size_t>(row) < actions_.size();
        const auto editing_enabled = !planning_ && !catalog_busy_;
        saved_->setEnabled(editing_enabled && static_cast<bool>(store_.load));
        save_->setEnabled(editing_enabled && static_cast<bool>(store_.save) && !actions_.empty() &&
                          !name_->text().trimmed().isEmpty());
        save_as_->setEnabled(editing_enabled && static_cast<bool>(store_.save) &&
                             !actions_.empty() && !name_->text().trimmed().isEmpty());
        delete_saved_->setEnabled(editing_enabled && static_cast<bool>(store_.remove) &&
                                  selected_saved_.has_value());
        name_->setEnabled(editing_enabled);
        kind_->setEnabled(editing_enabled);
        target_->setEnabled(editing_enabled);
        input_->setEnabled(editing_enabled);
        replacement_->setEnabled(editing_enabled);
        number_start_->setEnabled(editing_enabled);
        number_padding_->setEnabled(editing_enabled);
        add_->setEnabled(editing_enabled && actions_.size() < 256U);
        steps_->setEnabled(editing_enabled);
        remove_->setEnabled(editing_enabled && valid);
        up_->setEnabled(editing_enabled && valid && row > 0);
        down_->setEnabled(editing_enabled && valid && row + 1 < steps_->count());
        preview_button_->setEnabled(editing_enabled && !actions_.empty());
        stage_button_->setEnabled(editing_enabled && preview_ != nullptr &&
                                  !preview_->cells.empty());
    }

    void startPreview() {
        if (planning_ || actions_.empty()) {
            return;
        }
        invalidatePreview();
        cancellation_.request_cancellation();
        cancellation_ = core::CancellationSource{};
        auto chain = currentChain();
        const auto selection = selection_;
        const auto draft = draft_;
        const auto items = item_indexes_;
        const auto cancellation = cancellation_.token();
        planning_ = true;
        summary_->setText(QStringLiteral("Evaluating the chain against the current draft…"));
        updateActions();
        watcher_.setFuture(QtConcurrent::run(
            [selection, draft, items, chain = std::move(chain), cancellation]() mutable {
                return std::make_shared<PreviewResult>(metadata::plan_metadata_transformation(
                    *selection, draft, items, std::move(chain), cancellation));
            }));
    }

    void finishPreview() {
        planning_ = false;
        if (close_requested_) {
            close();
            return;
        }
        const auto result = watcher_.result();
        if (!result || !*result) {
            const auto message = result ? display_utf8(result->error().message)
                                        : QStringLiteral("The preview task returned no result");
            summary_->setText(QStringLiteral("Transformation preview failed · %1").arg(message));
            updateActions();
            return;
        }
        preview_ =
            std::make_shared<const metadata::MetadataTransformationPreview>(std::move(**result));
        auto* old_model = table_->model();
        table_->setModel(new MetadataTransformationPreviewModel(preview_, track_labels_, table_));
        if (old_model != nullptr) {
            old_model->deleteLater();
        }
        const auto capitalization_summary = capitalizationNoChangeSummary();
        summary_->setText(
            preview_->cells.empty()
                ? (capitalization_summary.isEmpty()
                       ? QStringLiteral("The chain produces no changes for the selected files.")
                       : capitalization_summary)
                : QStringLiteral("%1 final cell %2 across %3 selected %4 · review before staging")
                      .arg(preview_->cells.size())
                      .arg(preview_->cells.size() == 1U ? QStringLiteral("change")
                                                        : QStringLiteral("changes"))
                      .arg(preview_->changed_item_count)
                      .arg(preview_->changed_item_count == 1U ? QStringLiteral("file")
                                                              : QStringLiteral("files")));
        updateActions();
    }

    void stagePreview() {
        if (!preview_ || preview_->cells.empty() || !stage_) {
            return;
        }
        if (!stage_(*preview_)) {
            invalidatePreview();
            summary_->setText(QStringLiteral(
                "The preview is stale or could not fit in the draft. Preview the chain again."));
            return;
        }
        accept();
    }

    QFutureWatcher<std::shared_ptr<PreviewResult>> watcher_;
    std::shared_ptr<const metadata::StagedMetadataSelection> selection_;
    metadata::StagedMetadataPatchSet draft_;
    std::vector<std::size_t> item_indexes_;
    QStringList track_labels_;
    StageCallback stage_;
    MetadataTransformationStore store_;
    std::optional<core::StableId> initially_selected_;
    bool preview_initially_selected_{false};
    std::vector<persistence::SavedMetadataTransformationChain> catalog_;
    std::optional<core::StableId> selected_saved_;
    std::vector<metadata::MetadataTransformationAction> actions_;
    std::shared_ptr<const metadata::MetadataTransformationPreview> preview_;
    core::CancellationSource cancellation_;
    QComboBox* saved_{nullptr};
    QPushButton* save_{nullptr};
    QPushButton* save_as_{nullptr};
    QPushButton* delete_saved_{nullptr};
    QLabel* catalog_status_{nullptr};
    QLineEdit* name_{nullptr};
    QComboBox* kind_{nullptr};
    QLineEdit* target_{nullptr};
    std::vector<metadata::MetadataFieldSuggestionCandidate> target_field_candidates_;
    QStringListModel* target_completion_model_{nullptr};
    QCompleter* target_completer_{nullptr};
    QLabel* input_label_{nullptr};
    QLineEdit* input_{nullptr};
    QLabel* replacement_label_{nullptr};
    QLineEdit* replacement_{nullptr};
    QLabel* number_start_label_{nullptr};
    QSpinBox* number_start_{nullptr};
    QLabel* number_padding_label_{nullptr};
    QSpinBox* number_padding_{nullptr};
    QPushButton* add_{nullptr};
    QListWidget* steps_{nullptr};
    QPushButton* remove_{nullptr};
    QPushButton* up_{nullptr};
    QPushButton* down_{nullptr};
    QPushButton* preview_button_{nullptr};
    QLabel* summary_{nullptr};
    QTreeView* table_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* stage_button_{nullptr};
    bool planning_{false};
    bool catalog_busy_{false};
    bool close_requested_{false};
};

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

class MetadataWritePlanModel final : public QAbstractTableModel {
  public:
    explicit MetadataWritePlanModel(std::shared_ptr<const metadata::MetadataWritePlan> plan,
                                    QObject* parent = nullptr)
        : QAbstractTableModel(parent), plan_(std::move(plan)) {
        rows_.reserve(plan_->patch_count);
        for (std::size_t source_index = 0U; source_index < plan_->sources.size(); ++source_index) {
            const auto& source = plan_->sources[source_index];
            for (std::size_t change_index = 0U; change_index < source.changes.size();
                 ++change_index) {
                const auto& change = source.changes[change_index];
                for (std::size_t intent_index = 0U; intent_index < change.intents.size();
                     ++intent_index) {
                    rows_.push_back(Row{.source_index = source_index,
                                        .change_index = change_index,
                                        .intent_index = intent_index});
                }
            }
        }
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid()
                   ? 0
                   : static_cast<int>(std::min(
                         rows_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 6;
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= columnCount()) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        const auto& source = plan_->sources[row.source_index];
        const auto& change = source.changes[row.change_index];
        const auto& intent = change.intents[row.intent_index];
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case 0:
                return source.ready() ? QStringLiteral("Ready")
                                      : (has_conflict(source) ? QStringLiteral("Conflict")
                                                              : QStringLiteral("Blocked"));
            case 1:
                return QString::fromStdString(core::escape_raw_path(source.raw_path));
            case 2:
                return display_utf8(change.display_name);
            case 3:
                return change.original_present ? display_plan_values(change.original_values)
                                               : QStringLiteral("—");
            case 4:
                return intent.kind == metadata::StagedMetadataPatchKind::remove_field
                           ? QStringLiteral("(remove)")
                           : display_plan_values(intent.values);
            case 5:
                return QStringLiteral("File row %1 · %2 source %3")
                    .arg(intent.item_index + 1U)
                    .arg(source.occurrence_indexes.size())
                    .arg(source.occurrence_indexes.size() == 1U ? QStringLiteral("reference")
                                                                : QStringLiteral("references"));
            default:
                return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            auto details = QStringLiteral("Fresh adapter: %1\nStaged file row: %2")
                               .arg(source.adapter_name.empty() ? QStringLiteral("unavailable")
                                                                : display_utf8(source.adapter_name))
                               .arg(intent.item_index + 1U);
            if (change.conflicting_intents) {
                details += QStringLiteral("\nThis field has %1 incompatible logical intents.")
                               .arg(change.intents.size());
            }
            for (const auto& issue : source.issues) {
                if (issue.field_index && *issue.field_index != change.field_index) {
                    continue;
                }
                details += QStringLiteral("\n%1: %2")
                               .arg(display_utf8(
                                        metadata::metadata_write_plan_issue_kind_name(issue.kind)),
                                    display_utf8(issue.error.message));
            }
            return details;
        }
        if (role == Qt::TextAlignmentRole && index.column() == 0) {
            return Qt::AlignCenter;
        }
        if (role == Qt::ForegroundRole && !source.ready()) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        return {};
    }

    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (role != Qt::DisplayRole) {
            return {};
        }
        if (orientation == Qt::Vertical) {
            return section + 1;
        }
        switch (section) {
        case 0:
            return QStringLiteral("Status");
        case 1:
            return QStringLiteral("Physical source");
        case 2:
            return QStringLiteral("Field");
        case 3:
            return QStringLiteral("Fresh original");
        case 4:
            return QStringLiteral("Planned result");
        case 5:
            return QStringLiteral("Logical scope");
        default:
            return {};
        }
    }

  private:
    struct Row {
        std::size_t source_index{0U};
        std::size_t change_index{0U};
        std::size_t intent_index{0U};
    };

    std::shared_ptr<const metadata::MetadataWritePlan> plan_;
    std::vector<Row> rows_;
};

class MetadataWritePlanDialog final : public QDialog {
  public:
    using ApplyCallback = std::function<void(std::shared_ptr<const metadata::MetadataWritePlan>)>;

    explicit MetadataWritePlanDialog(std::shared_ptr<const metadata::MetadataWritePlan> plan,
                                     ApplyCallback apply, QWidget* parent)
        : QDialog(parent) {
        setObjectName(QStringLiteral("bench-metadata-write-plan"));
        setWindowTitle(QStringLiteral("Metadata write plan"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(1'100, 520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        auto* summary = new QLabel(
            QStringLiteral("%1 staged %2 · %3 physical %4 · %5 ready · %6 blocking %7")
                .arg(plan->patch_count)
                .arg(plan->patch_count == 1U ? QStringLiteral("change") : QStringLiteral("changes"))
                .arg(plan->sources.size())
                .arg(plan->sources.size() == 1U ? QStringLiteral("source")
                                                : QStringLiteral("sources"))
                .arg(plan->ready_source_count())
                .arg(plan->blocking_issue_count())
                .arg(plan->blocking_issue_count() == 1U ? QStringLiteral("issue")
                                                        : QStringLiteral("issues")),
            this);
        summary->setObjectName(QStringLiteral("bench-metadata-write-plan-summary"));
        layout->addWidget(summary);

        auto* explanation = new QLabel(
            plan->ready()
                ? QStringLiteral("Every source was freshly revalidated. This immutable preview "
                                 "is ready for explicit Apply.")
                : QStringLiteral("Writing is blocked. Hover a row for revision, logical-source, "
                                 "adapter, and preservation details. No files can be changed."),
            this);
        explanation->setObjectName(QStringLiteral("bench-metadata-write-plan-explanation"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        auto* table = new QTableView(this);
        table->setObjectName(QStringLiteral("bench-metadata-write-plan-table"));
        table->setAccessibleName(QStringLiteral("Revalidated metadata write-plan changes"));
        table->setModel(new MetadataWritePlanModel(plan, table));
        table->setAlternatingRowColors(true);
        table->setShowGrid(false);
        table->setWordWrap(false);
        table->setTextElideMode(Qt::ElideMiddle);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(24);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        table->setColumnWidth(0, 90);
        table->setColumnWidth(2, 150);
        table->setColumnWidth(5, 190);
        layout->addWidget(table, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->setObjectName(QStringLiteral("bench-metadata-write-plan-buttons"));
        if (plan->ready() && apply) {
            auto* apply_button = buttons->addButton(QDialogButtonBox::Apply);
            apply_button->setObjectName(QStringLiteral("bench-metadata-write-plan-apply"));
            apply_button->setToolTip(
                QStringLiteral("Commit every ready physical source in this immutable plan"));
            connect(apply_button, &QPushButton::clicked, this,
                    [plan = std::move(plan), apply = std::move(apply)] { apply(plan); });
        }
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        layout->addWidget(buttons);
    }
};

class MetadataApplySourceModel final : public QAbstractTableModel {
  public:
    explicit MetadataApplySourceModel(const metadata::MetadataWritePlan& plan,
                                      QObject* parent = nullptr)
        : QAbstractTableModel(parent) {
        rows_.reserve(plan.sources.size());
        for (const auto& source : plan.sources) {
            rows_.push_back(
                Row{.state = operations::MetadataApplySourceState::pending,
                    .source = QString::fromStdString(core::escape_raw_path(source.raw_path)),
                    .detail = QStringLiteral("Waiting for a mutation worker")});
        }
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 3;
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == Qt::ToolTipRole) {
            return row.detail;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case 0:
            return apply_state_text(row.state);
        case 1:
            return row.source;
        case 2:
            return row.detail;
        default:
            return {};
        }
    }
    [[nodiscard]] QVariant headerData(const int section, const Qt::Orientation orientation,
                                      const int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        static constexpr std::array labels{"Status", "Physical source", "Details"};
        return section >= 0 && section < static_cast<int>(labels.size())
                   ? QString::fromLatin1(labels[static_cast<std::size_t>(section)])
                   : QVariant{};
    }

    void update(const std::vector<operations::MetadataApplySourceState>& states,
                const std::vector<std::optional<core::Error>>& issues) {
        const auto count = std::min({rows_.size(), states.size(), issues.size()});
        for (std::size_t index = 0U; index < count; ++index) {
            rows_[index].state = states[index];
            rows_[index].detail =
                issues[index] ? display_utf8(issues[index]->message)
                              : (states[index] == operations::MetadataApplySourceState::running
                                     ? QStringLiteral("Preparing, verifying, and publishing")
                                 : states[index] == operations::MetadataApplySourceState::committed
                                     ? QStringLiteral("Published and refreshed every occurrence")
                                 : states[index] == operations::MetadataApplySourceState::pending
                                     ? QStringLiteral("Waiting for a mutation worker")
                                     : QString{});
        }
        if (count > 0U) {
            emit dataChanged(index(0, 0), index(static_cast<int>(count - 1U), columnCount() - 1));
        }
    }

  private:
    struct Row {
        operations::MetadataApplySourceState state{operations::MetadataApplySourceState::pending};
        QString source;
        QString detail;
    };
    std::vector<Row> rows_;
};

class MetadataApplyDialog final : public QDialog {
  public:
    using CancelCallback = std::function<void()>;

    explicit MetadataApplyDialog(const metadata::MetadataWritePlan& plan, CancelCallback cancel,
                                 QWidget* parent)
        : QDialog(parent), cancel_(std::move(cancel)) {
        setObjectName(QStringLiteral("bench-metadata-apply"));
        setWindowTitle(QStringLiteral("Apply metadata changes"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(900, 420);
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(QStringLiteral("Applying 0 of %1 physical %2…")
                                  .arg(plan.sources.size())
                                  .arg(plan.sources.size() == 1U ? QStringLiteral("source")
                                                                 : QStringLiteral("sources")),
                              this);
        summary_->setObjectName(QStringLiteral("bench-metadata-apply-summary"));
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        progress_ = new QProgressBar(this);
        progress_->setObjectName(QStringLiteral("bench-metadata-apply-progress"));
        progress_->setRange(0, static_cast<int>(plan.sources.size()));
        progress_->setValue(0);
        layout->addWidget(progress_);
        auto* table = new QTableView(this);
        table->setObjectName(QStringLiteral("bench-metadata-apply-table"));
        model_ = new MetadataApplySourceModel(plan, table);
        table->setModel(model_);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setWordWrap(false);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        buttons_ = new QDialogButtonBox(this);
        buttons_->setObjectName(QStringLiteral("bench-metadata-apply-buttons"));
        cancel_button_ =
            buttons_->addButton(QStringLiteral("Cancel"), QDialogButtonBox::RejectRole);
        cancel_button_->setObjectName(QStringLiteral("bench-metadata-apply-cancel"));
        connect(cancel_button_, &QPushButton::clicked, this, [this] { requestCancellation(); });
        layout->addWidget(buttons_);
    }

    void update(const MetadataApplyProgressState& progress) {
        std::vector<operations::MetadataApplySourceState> states;
        std::vector<std::optional<core::Error>> issues;
        std::size_t completed = 0U;
        {
            std::scoped_lock lock{progress.mutex};
            states = progress.states;
            issues = progress.issues;
            completed = progress.completed_sources;
        }
        model_->update(states, issues);
        progress_->setValue(static_cast<int>(completed));
        summary_->setText(
            QStringLiteral("Applying %1 of %2 physical %3%4")
                .arg(completed)
                .arg(states.size())
                .arg(states.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources"))
                .arg(cancellation_requested_ ? QStringLiteral(" · cancelling…")
                                             : QStringLiteral("…")));
    }

    void finish(const core::Result<operations::MetadataApplyResult>& result) {
        running_ = false;
        cancel_button_->setVisible(false);
        auto* close = buttons_->addButton(QDialogButtonBox::Close);
        close->setObjectName(QStringLiteral("bench-metadata-apply-close"));
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        if (!result) {
            summary_->setText(QStringLiteral("Metadata Apply could not start · %1")
                                  .arg(display_utf8(result.error().message)));
            return;
        }
        std::vector<operations::MetadataApplySourceState> states;
        std::vector<std::optional<core::Error>> issues;
        states.reserve(result->sources.size());
        issues.reserve(result->sources.size());
        for (const auto& source : result->sources) {
            states.push_back(source.state);
            issues.push_back(source.issue);
        }
        model_->update(states, issues);
        progress_->setValue(static_cast<int>(result->sources.size()));
        summary_->setText(
            QStringLiteral("%1 applied · %2 failed · %3 cancelled%4")
                .arg(result->committed_source_count())
                .arg(result->failed_source_count())
                .arg(result->cancelled_source_count())
                .arg(result->committed_source_count() == result->sources.size()
                         ? QStringLiteral(" · complete")
                         : QStringLiteral(" · close and re-preview before retrying")));
    }

  protected:
    void closeEvent(QCloseEvent* event) override {
        if (!running_) {
            QDialog::closeEvent(event);
            return;
        }
        requestCancellation();
        event->ignore();
    }

  private:
    void requestCancellation() {
        if (cancellation_requested_) {
            return;
        }
        cancellation_requested_ = true;
        cancel_button_->setEnabled(false);
        summary_->setText(QStringLiteral("Cancelling after in-flight sources become safe…"));
        if (cancel_) {
            cancel_();
        }
    }

    CancelCallback cancel_;
    MetadataApplySourceModel* model_{nullptr};
    QLabel* summary_{nullptr};
    QProgressBar* progress_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* cancel_button_{nullptr};
    bool running_{true};
    bool cancellation_requested_{false};
};

MetadataPropertiesDialog::MetadataPropertiesDialog(
    const std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
    const std::span<const std::string_view> preferred_fields,
    MetadataWritePlanApplierFactory plan_applier_factory, MetadataApplyObserver apply_observer,
    MetadataTransformationStore transformation_store, OutputProfileStore output_profile_store,
    QWidget* parent)
    : QDialog(parent), selection_watcher_(this), write_plan_watcher_(this), apply_watcher_(this),
      source_reader_(std::move(source_reader)),
      plan_applier_factory_(std::move(plan_applier_factory)),
      apply_observer_(std::move(apply_observer)),
      transformation_store_(std::move(transformation_store)),
      output_profile_store_(std::move(output_profile_store)),
      requested_item_count_(requested_item_count) {
    setObjectName(QStringLiteral("bench-metadata-properties"));
    setWindowTitle(QStringLiteral("Track properties"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1'020, 620);

    root_layout_ = new QVBoxLayout(this);
    root_layout_->setContentsMargins(10, 8, 10, 8);
    root_layout_->setSpacing(6);

    summary_ = new QLabel(QStringLiteral("%1 %2 · preparing")
                              .arg(requested_item_count_)
                              .arg(pluralized(requested_item_count_, QStringLiteral("track"),
                                              QStringLiteral("tracks"))),
                          this);
    summary_->setObjectName(QStringLiteral("bench-metadata-summary"));
    root_layout_->addWidget(summary_);

    read_only_ =
        new QLabel(QStringLiteral("Read-only metadata preview · preparing selection"), this);
    read_only_->setObjectName(QStringLiteral("bench-metadata-read-only"));
    read_only_->setAccessibleName(QStringLiteral("Metadata write capability"));
    read_only_->setTextFormat(Qt::PlainText);
    root_layout_->addWidget(read_only_);

    auto* side_tabs = new QTabWidget(this);
    side_tabs->setObjectName(QStringLiteral("bench-metadata-side-panel"));
    side_tabs->setMinimumWidth(270);
    side_tabs->setMaximumWidth(390);
    transformation_panel_ = side_tabs;
    transformation_panel_->hide();

    auto* transformation_group = new QWidget(side_tabs);
    transformation_group->setObjectName(QStringLiteral("bench-metadata-transformation-panel"));
    auto* transformation_layout = new QVBoxLayout(transformation_group);
    transformation_layout->setContentsMargins(8, 8, 8, 8);
    transformation_layout->setSpacing(6);
    auto* transformation_help = new QLabel(
        QStringLiteral("Checked scripts are included whenever this tag draft is previewed and "
                       "applied. The checked state is saved."),
        transformation_group);
    transformation_help->setObjectName(QStringLiteral("bench-metadata-transformation-help"));
    transformation_help->setWordWrap(true);
    transformation_layout->addWidget(transformation_help);
    transformation_list_ = new QListWidget(transformation_group);
    transformation_list_->setObjectName(QStringLiteral("bench-metadata-transformation-list"));
    transformation_list_->setAccessibleName(QStringLiteral("Saved tagging scripts"));
    transformation_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    transformation_list_->setAlternatingRowColors(true);
    transformation_list_->setEnabled(false);
    transformation_layout->addWidget(transformation_list_, 1);
    transformation_status_ =
        new QLabel(QStringLiteral("Loading saved scripts…"), transformation_group);
    transformation_status_->setObjectName(QStringLiteral("bench-metadata-transformation-status"));
    transformation_status_->setWordWrap(true);
    transformation_layout->addWidget(transformation_status_);
    transform_button_ =
        new QPushButton(QStringLiteral("Open script editor…"), transformation_group);
    transform_button_->setObjectName(QStringLiteral("bench-metadata-transform"));
    transform_button_->setToolTip(
        QStringLiteral("Open the selected saved script, or create a new one"));
    transform_button_->setEnabled(false);
    transformation_layout->addWidget(transform_button_);
    side_tabs->addTab(transformation_group, QStringLiteral("Scripts"));

    auto* operations_panel = new QWidget(side_tabs);
    operations_panel->setObjectName(QStringLiteral("bench-preparation-operations-panel"));
    auto* operations_layout = new QVBoxLayout(operations_panel);
    operations_layout->setContentsMargins(8, 8, 8, 8);
    operations_layout->setSpacing(6);
    auto* operations_help = new QLabel(
        QStringLiteral("Choose what one reviewed preparation will do. Rename and Move become "
                       "selectable with their combined preview and undo path."),
        operations_panel);
    operations_help->setObjectName(QStringLiteral("bench-preparation-operations-help"));
    operations_help->setWordWrap(true);
    operations_layout->addWidget(operations_help);
    save_tags_check_ = new QCheckBox(QStringLiteral("Save tags"), operations_panel);
    save_tags_check_->setObjectName(QStringLiteral("bench-preparation-save-tags"));
    save_tags_check_->setChecked(true);
    operations_layout->addWidget(save_tags_check_);
    rename_files_check_ = new QCheckBox(QStringLiteral("Rename files"), operations_panel);
    rename_files_check_->setObjectName(QStringLiteral("bench-preparation-rename-files"));
    rename_files_check_->setEnabled(false);
    rename_files_check_->setToolTip(
        QStringLiteral("The combined metadata/file review is not wired yet"));
    operations_layout->addWidget(rename_files_check_);
    move_files_check_ = new QCheckBox(QStringLiteral("Move files"), operations_panel);
    move_files_check_->setObjectName(QStringLiteral("bench-preparation-move-files"));
    move_files_check_->setEnabled(false);
    move_files_check_->setToolTip(
        QStringLiteral("The combined metadata/file review is not wired yet"));
    operations_layout->addWidget(move_files_check_);
    replaygain_check_ = new QCheckBox(QStringLiteral("ReplayGain"), operations_panel);
    replaygain_check_->setObjectName(QStringLiteral("bench-preparation-replaygain"));
    replaygain_check_->setEnabled(false);
    replaygain_check_->setToolTip(QStringLiteral("ReplayGain analysis is planned for M7"));
    operations_layout->addWidget(replaygain_check_);

    auto* layout_group = new QGroupBox(QStringLiteral("Naming layout"), operations_panel);
    layout_group->setObjectName(QStringLiteral("bench-output-layout-editor"));
    auto* layout_form = new QFormLayout(layout_group);
    layout_form->setContentsMargins(6, 8, 6, 6);
    output_layout_combo_ = new QComboBox(layout_group);
    output_layout_combo_->setObjectName(QStringLiteral("bench-output-layout-profile"));
    output_layout_combo_->setAccessibleName(QStringLiteral("Saved naming layout"));
    layout_form->addRow(QStringLiteral("Saved:"), output_layout_combo_);
    output_layout_name_ = new QLineEdit(layout_group);
    output_layout_name_->setObjectName(QStringLiteral("bench-output-layout-name"));
    output_layout_name_->setPlaceholderText(QStringLiteral("For example: Album folders"));
    layout_form->addRow(QStringLiteral("Name:"), output_layout_name_);
    output_directory_expression_ = new QLineEdit(layout_group);
    output_directory_expression_->setObjectName(
        QStringLiteral("bench-output-layout-directory-expression"));
    output_directory_expression_->setPlaceholderText(
        QStringLiteral("For example: %album artist%/%album%"));
    layout_form->addRow(QStringLiteral("Folders:"), output_directory_expression_);
    output_basename_expression_ = new QLineEdit(layout_group);
    output_basename_expression_->setObjectName(
        QStringLiteral("bench-output-layout-basename-expression"));
    output_basename_expression_->setPlaceholderText(
        QStringLiteral("For example: %tracknumber% - %title%"));
    layout_form->addRow(QStringLiteral("Filename:"), output_basename_expression_);
    auto* layout_buttons = new QHBoxLayout;
    output_layout_new_button_ = new QPushButton(QStringLiteral("New"), layout_group);
    output_layout_new_button_->setObjectName(QStringLiteral("bench-output-layout-new"));
    output_layout_save_button_ = new QPushButton(QStringLiteral("Save"), layout_group);
    output_layout_save_button_->setObjectName(QStringLiteral("bench-output-layout-save"));
    output_layout_remove_button_ = new QPushButton(QStringLiteral("Remove"), layout_group);
    output_layout_remove_button_->setObjectName(QStringLiteral("bench-output-layout-remove"));
    layout_buttons->addWidget(output_layout_new_button_);
    layout_buttons->addWidget(output_layout_save_button_);
    layout_buttons->addWidget(output_layout_remove_button_);
    layout_form->addRow(layout_buttons);
    operations_layout->addWidget(layout_group);

    auto* destination_group = new QGroupBox(QStringLiteral("Move destination"), operations_panel);
    destination_group->setObjectName(QStringLiteral("bench-destination-editor"));
    auto* destination_form = new QFormLayout(destination_group);
    destination_form->setContentsMargins(6, 8, 6, 6);
    destination_combo_ = new QComboBox(destination_group);
    destination_combo_->setObjectName(QStringLiteral("bench-destination-profile"));
    destination_combo_->setAccessibleName(QStringLiteral("Saved move destination"));
    destination_form->addRow(QStringLiteral("Saved:"), destination_combo_);
    destination_name_ = new QLineEdit(destination_group);
    destination_name_->setObjectName(QStringLiteral("bench-destination-name"));
    destination_name_->setPlaceholderText(QStringLiteral("For example: Music library"));
    destination_form->addRow(QStringLiteral("Name:"), destination_name_);
    auto* root_row = new QHBoxLayout;
    destination_root_ = new QLineEdit(destination_group);
    destination_root_->setObjectName(QStringLiteral("bench-destination-root"));
    destination_root_->setPlaceholderText(QStringLiteral("Choose an absolute folder"));
    destination_browse_button_ = new QPushButton(QStringLiteral("Browse…"), destination_group);
    destination_browse_button_->setObjectName(QStringLiteral("bench-destination-browse"));
    root_row->addWidget(destination_root_, 1);
    root_row->addWidget(destination_browse_button_);
    destination_form->addRow(QStringLiteral("Root:"), root_row);
    auto* destination_buttons = new QHBoxLayout;
    destination_new_button_ = new QPushButton(QStringLiteral("New"), destination_group);
    destination_new_button_->setObjectName(QStringLiteral("bench-destination-new"));
    destination_save_button_ = new QPushButton(QStringLiteral("Save"), destination_group);
    destination_save_button_->setObjectName(QStringLiteral("bench-destination-save"));
    destination_remove_button_ = new QPushButton(QStringLiteral("Remove"), destination_group);
    destination_remove_button_->setObjectName(QStringLiteral("bench-destination-remove"));
    destination_buttons->addWidget(destination_new_button_);
    destination_buttons->addWidget(destination_save_button_);
    destination_buttons->addWidget(destination_remove_button_);
    destination_form->addRow(destination_buttons);
    operations_layout->addWidget(destination_group);
    output_profile_status_ =
        new QLabel(QStringLiteral("Loading output profiles…"), operations_panel);
    output_profile_status_->setObjectName(QStringLiteral("bench-output-profile-status"));
    output_profile_status_->setWordWrap(true);
    operations_layout->addWidget(output_profile_status_);
    operations_layout->addStretch(1);
    side_tabs->addTab(operations_panel, QStringLiteral("Operations"));

    loading_ = new QLabel(QStringLiteral("Preparing metadata grid…"), this);
    loading_->setObjectName(QStringLiteral("bench-metadata-loading"));
    loading_->setAlignment(Qt::AlignCenter);
    root_layout_->addWidget(loading_, 1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons_->setObjectName(QStringLiteral("bench-metadata-buttons"));
    undo_button_ = buttons_->addButton(QStringLiteral("Undo"), QDialogButtonBox::ActionRole);
    undo_button_->setObjectName(QStringLiteral("bench-metadata-undo"));
    undo_button_->setEnabled(false);
    redo_button_ = buttons_->addButton(QStringLiteral("Redo"), QDialogButtonBox::ActionRole);
    redo_button_->setObjectName(QStringLiteral("bench-metadata-redo"));
    redo_button_->setEnabled(false);
    discard_button_ =
        buttons_->addButton(QStringLiteral("Discard draft"), QDialogButtonBox::ResetRole);
    discard_button_->setObjectName(QStringLiteral("bench-metadata-discard"));
    discard_button_->setEnabled(false);
    add_field_button_ =
        buttons_->addButton(QStringLiteral("Add field…"), QDialogButtonBox::ActionRole);
    add_field_button_->setObjectName(QStringLiteral("bench-metadata-add-field"));
    add_field_button_->setToolTip(QStringLiteral("Add an arbitrary metadata field (Insert)"));
    add_field_button_->setEnabled(false);
    remove_field_button_ =
        buttons_->addButton(QStringLiteral("Remove field"), QDialogButtonBox::ActionRole);
    remove_field_button_->setObjectName(QStringLiteral("bench-metadata-remove-field"));
    remove_field_button_->setToolTip(
        QStringLiteral("Remove the selected fields from the selected files (Delete)"));
    remove_field_button_->setEnabled(false);
    edit_values_button_ =
        buttons_->addButton(QStringLiteral("Edit values…"), QDialogButtonBox::ActionRole);
    edit_values_button_->setObjectName(QStringLiteral("bench-metadata-edit-values"));
    edit_values_button_->setToolTip(
        QStringLiteral("Edit the exact ordered value list (Ctrl+Enter)"));
    edit_values_button_->setEnabled(false);
    preview_plan_button_ =
        buttons_->addButton(QStringLiteral("Preview write plan…"), QDialogButtonBox::ActionRole);
    preview_plan_button_->setObjectName(QStringLiteral("bench-metadata-preview-write-plan"));
    preview_plan_button_->setToolTip(
        QStringLiteral("Freshly revalidate every staged physical source and preview conflicts"));
    preview_plan_button_->setEnabled(false);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(undo_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->undo());
        }
    });
    connect(redo_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->redo());
        }
    });
    connect(discard_button_, &QPushButton::clicked, this, [this] {
        if (grid_model_ != nullptr) {
            static_cast<void>(grid_model_->discardAll());
        }
    });
    connect(add_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::promptAddField);
    connect(remove_field_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeSelectedFields);
    connect(edit_values_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::editCurrentValues);
    connect(transform_button_, &QPushButton::clicked, this, [this] {
        std::optional<core::StableId> selected;
        if (const auto* item = transformation_list_->currentItem()) {
            if (const auto parsed =
                    core::StableId::parse(item->data(Qt::UserRole).toString().toStdString())) {
                selected = *parsed;
            }
        }
        promptTransformation(selected);
    });
    connect(transformation_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                const auto selected =
                    core::StableId::parse(item->data(Qt::UserRole).toString().toStdString());
                if (selected) {
                    promptTransformation(*selected);
                }
            });
    connect(transformation_list_, &QListWidget::itemSelectionChanged, this,
            &MetadataPropertiesDialog::updateTransformationButton);
    connect(transformation_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        const auto id = core::StableId::parse(item->data(Qt::UserRole).toString().toStdString());
        if (id) {
            toggleAutomaticTransformation(*id, item->checkState() == Qt::Checked);
        }
    });
    connect(output_layout_combo_, &QComboBox::currentIndexChanged, this,
            &MetadataPropertiesDialog::selectOutputLayout);
    connect(destination_combo_, &QComboBox::currentIndexChanged, this,
            &MetadataPropertiesDialog::selectDestination);
    connect(output_layout_new_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::newOutputLayout);
    connect(destination_new_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::newDestination);
    connect(output_layout_save_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::saveOutputLayout);
    connect(destination_save_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::saveDestination);
    connect(output_layout_remove_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeOutputLayout);
    connect(destination_remove_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::removeDestination);
    for (auto* editor : {output_layout_name_, output_directory_expression_,
                         output_basename_expression_, destination_name_, destination_root_}) {
        connect(editor, &QLineEdit::textChanged, this,
                &MetadataPropertiesDialog::updateOutputProfileButtons);
    }
    connect(destination_root_, &QLineEdit::textEdited, this, [this](const QString& text) {
        const auto encoded = QFile::encodeName(text);
        destination_root_raw_path_.assign(encoded.constData(),
                                          static_cast<std::size_t>(encoded.size()));
    });
    connect(destination_browse_button_, &QPushButton::clicked, this, [this] {
        const auto initial = destination_root_raw_path_.empty()
                                 ? QString{}
                                 : QFile::decodeName(QByteArray{
                                       destination_root_raw_path_.data(),
                                       static_cast<qsizetype>(destination_root_raw_path_.size())});
        const auto selected = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose move destination"), initial,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (selected.isEmpty()) {
            return;
        }
        const auto encoded = QFile::encodeName(selected);
        destination_root_raw_path_.assign(encoded.constData(),
                                          static_cast<std::size_t>(encoded.size()));
        destination_root_->setText(
            QString::fromStdString(core::escape_raw_path(destination_root_raw_path_)));
        updateOutputProfileButtons();
    });
    connect(save_tags_check_, &QCheckBox::toggled, this, [this](const bool enabled) {
        invalidateWritePlan();
        if (!enabled) {
            read_only_->setText(QStringLiteral(
                "Tag edits remain in memory but will not be written while Save tags is off"));
        } else if (grid_model_ != nullptr) {
            updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
        }
    });
    connect(preview_plan_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::previewWritePlan);
    connect(&write_plan_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishWritePlan);
    connect(&apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishApply);
    apply_progress_timer_ = new QTimer(this);
    apply_progress_timer_->setInterval(50);
    connect(apply_progress_timer_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::updateApplyProgress);
    root_layout_->addWidget(buttons_);
    loadTransformationCatalog();
    loadOutputProfiles();

    const metadata::StagedMetadataSelectionLimits limits;
    if (requested_item_count_ > limits.items) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(
            QStringLiteral("The selection exceeds the %1-track limit").arg(limits.items));
        source_reader_ = {};
        return;
    }
    sources_.reserve(requested_item_count_);
    track_labels_.reserve(static_cast<qsizetype>(std::min(
        requested_item_count_, static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))));
    preferred_fields_.reserve(preferred_fields.size());
    for (const auto field : preferred_fields) {
        preferred_fields_.emplace_back(field);
    }
    connect(&selection_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishSelection);
    QTimer::singleShot(0, this, &MetadataPropertiesDialog::captureSources);
}

MetadataPropertiesDialog::~MetadataPropertiesDialog() {
    write_plan_cancellation_.request_cancellation();
    apply_cancellation_.request_cancellation();
    if (write_plan_running_) {
        write_plan_watcher_.waitForFinished();
    }
    if (apply_running_) {
        apply_watcher_.waitForFinished();
    }
}

void MetadataPropertiesDialog::captureSources() {
    constexpr auto capture_budget_ms = 4;
    if (capture_index_ >= requested_item_count_) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(QStringLiteral("No tracks were selected"));
        return;
    }
    QElapsedTimer timer;
    timer.start();
    do {
        if (auto snapshot = source_reader_(capture_index_)) {
            sources_.push_back(std::move(snapshot->source));
            track_labels_.push_back(std::move(snapshot->track_label));
        }
        ++capture_index_;
    } while (capture_index_ < requested_item_count_ && timer.elapsed() < capture_budget_ms);

    if (capture_index_ < requested_item_count_) {
        loading_->setText(QStringLiteral("Preparing metadata grid… %1/%2")
                              .arg(capture_index_)
                              .arg(requested_item_count_));
        QTimer::singleShot(0, this, &MetadataPropertiesDialog::captureSources);
        return;
    }
    source_reader_ = {};
    if (sources_.empty()) {
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(QStringLiteral("The selected tracks are no longer available"));
        return;
    }
    startSelection();
}

void MetadataPropertiesDialog::startSelection() {
    selection_watcher_.setFuture(QtConcurrent::run(
        [sources = std::move(sources_), preferred = std::move(preferred_fields_)]() mutable {
            std::vector<std::string_view> preferred_views;
            preferred_views.reserve(preferred.size());
            for (const auto& field : preferred) {
                preferred_views.emplace_back(field);
            }
            return std::make_shared<SelectionResult>(
                metadata::StagedMetadataSelection::create(std::move(sources), preferred_views));
        }));
}

void MetadataPropertiesDialog::finishSelection() {
    const auto result = selection_watcher_.result();
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The selection task returned no result");
        summary_->setText(QStringLiteral("Properties unavailable"));
        read_only_->setText(QStringLiteral("Read-only metadata preview"));
        loading_->setText(message);
        return;
    }
    buildGrid(std::move(**result));
}

void MetadataPropertiesDialog::buildGrid(metadata::StagedMetadataSelection selection) {
    const auto item_count = selection.item_count();
    const auto source_count = selection.distinct_source_count();
    const auto field_count = selection.field_count();
    const auto revision_count = selection.item_revision_count();
    loaded_item_count_ = item_count;
    selected_item_count_ = item_count;
    loaded_source_count_ = source_count;
    loaded_field_count_ = field_count;
    selection_summary_ =
        QStringLiteral("%1 of %2 files selected · %3 %4 · %5 fields")
            .arg(item_count)
            .arg(item_count)
            .arg(source_count)
            .arg(pluralized(source_count, QStringLiteral("source"), QStringLiteral("sources")))
            .arg(field_count);
    revision_summary_ = revision_count == item_count
                            ? QStringLiteral("source revisions captured")
                            : QStringLiteral("%1 rows have no captured source revision")
                                  .arg(item_count - revision_count);
    updateDraftState(0, false, false);

    auto* content_splitter = new QSplitter(Qt::Horizontal, this);
    content_splitter->setObjectName(QStringLiteral("bench-metadata-content-splitter"));
    content_splitter->setChildrenCollapsible(false);
    auto* splitter = new QSplitter(Qt::Vertical, content_splitter);
    splitter->setObjectName(QStringLiteral("bench-metadata-splitter"));
    splitter->setChildrenCollapsible(false);

    file_list_ = new QTableView(splitter);
    file_list_->setObjectName(QStringLiteral("bench-metadata-files"));
    file_list_->setAccessibleName(QStringLiteral("Files included in metadata edit"));
    grid_model_ = new MetadataGridModel(std::move(selection), std::move(track_labels_), file_list_);
    file_list_->setModel(grid_model_);
    file_list_->setAlternatingRowColors(true);
    file_list_->setShowGrid(false);
    file_list_->setWordWrap(false);
    file_list_->setTextElideMode(Qt::ElideMiddle);
    file_list_->setSelectionBehavior(QAbstractItemView::SelectRows);
    file_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    file_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    file_list_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    file_list_->verticalHeader()->hide();
    file_list_->verticalHeader()->setDefaultSectionSize(24);
    file_list_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (auto column = 1; column < grid_model_->columnCount(); ++column) {
        file_list_->hideColumn(column);
    }
    connect(grid_model_, &QAbstractItemModel::columnsInserted, file_list_,
            [this](const QModelIndex& parent, const int first, const int last) {
                if (parent.isValid()) {
                    return;
                }
                for (auto column = first; column <= last; ++column) {
                    file_list_->hideColumn(column);
                }
            });

    fields_ = new QTableView(splitter);
    fields_->setObjectName(QStringLiteral("bench-metadata-fields"));
    fields_->setAccessibleName(QStringLiteral("Metadata fields with original and draft values"));
    aggregate_model_ = new MetadataAggregateModel(grid_model_, fields_);
    fields_->setModel(aggregate_model_);
    fields_->setAlternatingRowColors(true);
    fields_->setShowGrid(false);
    fields_->setWordWrap(false);
    fields_->setTextElideMode(Qt::ElideRight);
    fields_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fields_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fields_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                             QAbstractItemView::AnyKeyPressed);
    fields_->setItemDelegate(new MetadataScalarDelegate(fields_));
    fields_->installEventFilter(this);
    fields_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->verticalHeader()->hide();
    fields_->verticalHeader()->setDefaultSectionSize(24);
    fields_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    fields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    fields_->setColumnWidth(0, 190);

    splitter->addWidget(file_list_);
    splitter->addWidget(fields_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({340, 220});
    content_splitter->addWidget(splitter);
    content_splitter->addWidget(transformation_panel_);
    transformation_panel_->show();
    content_splitter->setStretchFactor(0, 1);
    content_splitter->setStretchFactor(1, 0);
    content_splitter->setSizes({760, 260});
    root_layout_->insertWidget(root_layout_->indexOf(buttons_), content_splitter, 1);

    selection_debounce_ = new QTimer(this);
    selection_debounce_->setSingleShot(true);
    selection_debounce_->setInterval(40);
    connect(selection_debounce_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::updateSelectionProjection);
    connect(file_list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { scheduleSelectionProjection(); });
    connect(fields_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
        updateFieldButtons();
        updateEditValuesButton();
    });
    connect(fields_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { updateFieldButtons(); });
    connect(grid_model_, &MetadataGridModel::draftStateChanged, this,
            [this](const int patch_count, const bool can_undo, const bool can_redo) {
                invalidateWritePlan();
                updateDraftState(patch_count, can_undo, can_redo);
            });
    connect(grid_model_, &MetadataGridModel::editRejected, this, [this](const QString& message) {
        read_only_->setText(QStringLiteral("Draft edit rejected · %1").arg(message));
    });
    connect(aggregate_model_, &MetadataAggregateModel::editRejected, this,
            [this](const QString& message) {
                read_only_->setText(QStringLiteral("Draft edit rejected · %1").arg(message));
            });
    connect(aggregate_model_, &MetadataAggregateModel::selectionProjectionChanged, this,
            [this](const bool ready, const int selected_count) {
                if (ready) {
                    updateDraftState(draft_count_, undo_button_->isEnabled(),
                                     redo_button_->isEnabled());
                } else {
                    read_only_->setText(QStringLiteral("Preparing metadata for %1 selected %2…")
                                            .arg(selected_count)
                                            .arg(selected_count == 1 ? QStringLiteral("file")
                                                                     : QStringLiteral("files")));
                }
                updateFieldButtons();
                updateEditValuesButton();
                updateTransformationButton();
            });
    if (grid_model_->rowCount() > 0) {
        file_list_->selectionModel()->setCurrentIndex(grid_model_->index(0, 0),
                                                      QItemSelectionModel::NoUpdate);
        file_list_->selectAll();
        updateSelectionProjection();
    }
    if (aggregate_model_->rowCount() > 0) {
        fields_->setCurrentIndex(aggregate_model_->index(0, 2));
    }
    updateEditValuesButton();
    updateFieldButtons();
    updateTransformationButton();

    root_layout_->removeWidget(loading_);
    loading_->deleteLater();
    loading_ = nullptr;
}

void MetadataPropertiesDialog::scheduleSelectionProjection() {
    if (selection_debounce_ != nullptr) {
        selection_debounce_->start();
    }
}

void MetadataPropertiesDialog::updateSelectionProjection() {
    if (file_list_ == nullptr || aggregate_model_ == nullptr ||
        file_list_->selectionModel() == nullptr) {
        return;
    }

    auto selected_items = selectedItemIndexes();
    selected_item_count_ = selected_items.size();
    selection_summary_ = QStringLiteral("%1 of %2 files selected · %3 %4 · %5 fields")
                             .arg(selected_item_count_)
                             .arg(loaded_item_count_)
                             .arg(loaded_source_count_)
                             .arg(pluralized(loaded_source_count_, QStringLiteral("source"),
                                             QStringLiteral("sources")))
                             .arg(loaded_field_count_);
    updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
    aggregate_model_->setSelectedItems(std::move(selected_items));
    updateTransformationButton();
}

void MetadataPropertiesDialog::updateDraftState(const int patch_count, const bool can_undo,
                                                const bool can_redo) {
    draft_count_ = patch_count;
    summary_->setText(patch_count == 0 ? selection_summary_
                                       : QStringLiteral("%1 · %2 staged %3")
                                             .arg(selection_summary_)
                                             .arg(patch_count)
                                             .arg(patch_count == 1 ? QStringLiteral("change")
                                                                   : QStringLiteral("changes")));
    read_only_->setText(
        save_tags_check_ != nullptr && !save_tags_check_->isChecked()
            ? QStringLiteral("In-memory draft only · Save tags is off, so metadata is used only "
                             "as preparation context · %1")
                  .arg(revision_summary_)
            : QStringLiteral("In-memory draft only · file writing is not enabled until a fresh "
                             "ready plan is explicitly applied · %1")
                  .arg(revision_summary_));
    undo_button_->setEnabled(can_undo);
    redo_button_->setEnabled(can_redo);
    discard_button_->setEnabled(patch_count > 0);
    updateFieldButtons();
    updateTransformationButton();
    updateWritePlanButton();
}

void MetadataPropertiesDialog::updateFieldButtons() {
    if (add_field_button_ == nullptr || remove_field_button_ == nullptr) {
        return;
    }
    const auto selection_ready = aggregate_model_ != nullptr && aggregate_model_->summaryReady() &&
                                 aggregate_model_->selectedItemCount() > 0U;
    add_field_button_->setEnabled(selection_ready && field_name_dialog_ == nullptr);
    const auto has_fields = fields_ != nullptr && fields_->selectionModel() != nullptr &&
                            !fields_->selectionModel()->selectedRows(0).empty();
    remove_field_button_->setEnabled(selection_ready && has_fields);
}

void MetadataPropertiesDialog::updateEditValuesButton() {
    if (edit_values_button_ == nullptr) {
        return;
    }
    const auto enabled = exact_values_dialog_ == nullptr && aggregate_model_ != nullptr &&
                         aggregate_model_->summaryReady() &&
                         aggregate_model_->selectedItemCount() > 0U && fields_ != nullptr &&
                         fields_->currentIndex().isValid();
    edit_values_button_->setEnabled(enabled);
}

void MetadataPropertiesDialog::updateTransformationButton() {
    if (transform_button_ == nullptr) {
        return;
    }
    const auto enabled = transformation_dialog_ == nullptr && grid_model_ != nullptr &&
                         aggregate_model_ != nullptr && aggregate_model_->summaryReady() &&
                         aggregate_model_->selectedItemCount() > 0U &&
                         exact_values_dialog_ == nullptr && field_name_dialog_ == nullptr &&
                         !write_plan_running_ && !apply_running_ && write_plan_dialog_ == nullptr &&
                         apply_dialog_ == nullptr;
    transform_button_->setEnabled(enabled);
    if (transformation_list_ != nullptr) {
        transformation_list_->setEnabled(!transformation_catalog_loading_ &&
                                         !transformation_catalog_.empty() && enabled);
        transform_button_->setText(transformation_list_->currentItem() == nullptr
                                       ? QStringLiteral("Open script editor…")
                                       : QStringLiteral("Edit selected script…"));
    }
}

void MetadataPropertiesDialog::loadTransformationCatalog(
    const std::optional<core::StableId> selected) {
    if (!transformation_store_.load) {
        transformation_catalog_.clear();
        transformation_catalog_loading_ = false;
        rebuildTransformationCatalogControls();
        return;
    }
    transformation_catalog_loading_ = true;
    updateTransformationButton();
    const QPointer<MetadataPropertiesDialog> self{this};
    transformation_store_.load(
        [self, selected](std::vector<persistence::SavedMetadataTransformationChain> chains,
                         QString error) mutable {
            if (!self) {
                return;
            }
            self->transformation_catalog_loading_ = false;
            if (!error.isEmpty()) {
                self->transformation_catalog_.clear();
                self->rebuildTransformationCatalogControls();
                self->read_only_->setText(
                    QStringLiteral("Could not load saved transformations · %1").arg(error));
                return;
            }
            self->transformation_catalog_ = std::move(chains);
            self->rebuildTransformationCatalogControls(selected);
        });
}

void MetadataPropertiesDialog::rebuildTransformationCatalogControls(
    const std::optional<core::StableId> selected) {
    std::ranges::sort(transformation_catalog_, [](const auto& left, const auto& right) {
        if (left.chain.name != right.chain.name) {
            return left.chain.name < right.chain.name;
        }
        return left.id.to_string() < right.id.to_string();
    });
    const QSignalBlocker blocker{transformation_list_};
    transformation_list_->clear();
    QListWidgetItem* selected_item = nullptr;
    std::size_t automatic_count = 0U;
    for (const auto& entry : transformation_catalog_) {
        const auto name = display_utf8(entry.chain.name);
        const auto id = QString::fromStdString(entry.id.to_string());
        auto* item = new QListWidgetItem(name, transformation_list_);
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(entry.automatic ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(entry.automatic ? QStringLiteral("Included in every tagging write plan")
                                         : QStringLiteral("Not included automatically"));
        if (selected && entry.id == *selected) {
            selected_item = item;
        }
        automatic_count += entry.automatic ? 1U : 0U;
    }
    if (selected_item != nullptr) {
        transformation_list_->setCurrentItem(selected_item);
    } else if (transformation_list_->count() > 0) {
        transformation_list_->setCurrentRow(0);
    }
    transformation_status_->setText(
        transformation_catalog_.empty()
            ? QStringLiteral("No saved scripts yet. Open the editor to create one.")
            : QStringLiteral("%1 of %2 checked · run in the order shown")
                  .arg(automatic_count)
                  .arg(transformation_catalog_.size()));
    updateTransformationButton();
    updateWritePlanButton();
}

void MetadataPropertiesDialog::toggleAutomaticTransformation(const core::StableId id,
                                                             const bool enabled) {
    if (transformation_catalog_loading_ || !transformation_store_.save) {
        rebuildTransformationCatalogControls(id);
        return;
    }
    const auto found = std::ranges::find(transformation_catalog_, id,
                                         &persistence::SavedMetadataTransformationChain::id);
    if (found == transformation_catalog_.end() || found->automatic == enabled) {
        return;
    }
    auto updated = *found;
    updated.automatic = enabled;
    auto retained_update = updated;
    transformation_catalog_loading_ = true;
    updateTransformationButton();
    const QPointer<MetadataPropertiesDialog> self{this};
    transformation_store_.save(std::move(updated), [self, updated = std::move(retained_update)](
                                                       QString error) mutable {
        if (!self) {
            return;
        }
        self->transformation_catalog_loading_ = false;
        if (!error.isEmpty()) {
            self->rebuildTransformationCatalogControls(updated.id);
            self->read_only_->setText(
                QStringLiteral("Could not update automatic transformation · %1").arg(error));
            return;
        }
        const auto retained = std::ranges::find(self->transformation_catalog_, updated.id,
                                                &persistence::SavedMetadataTransformationChain::id);
        if (retained != self->transformation_catalog_.end()) {
            *retained = updated;
        }
        self->rebuildTransformationCatalogControls(updated.id);
        self->read_only_->setText(
            QStringLiteral("%1 will %2run before every write preview; Apply still requires "
                           "explicit confirmation.")
                .arg(display_utf8(updated.chain.name),
                     updated.automatic ? QString{} : QStringLiteral("not ")));
    });
}

void MetadataPropertiesDialog::loadOutputProfiles() {
    output_profiles_loading_ = true;
    updateOutputProfileButtons();
    if (!output_profile_store_.load) {
        output_profiles_loading_ = false;
        output_profile_status_->setText(
            QStringLiteral("Output-profile persistence is unavailable"));
        rebuildOutputProfileControls();
        return;
    }
    const QPointer self{this};
    output_profile_store_.load(
        [self](std::vector<persistence::SavedOutputLayoutProfile> layouts,
               std::vector<persistence::SavedDestinationProfile> destinations,
               QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profiles_loading_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not load output profiles · %1").arg(error));
                self->rebuildOutputProfileControls();
                return;
            }
            self->output_layout_catalog_ = std::move(layouts);
            self->destination_catalog_ = std::move(destinations);
            self->rebuildOutputProfileControls(self->editing_output_layout_id_,
                                               self->editing_destination_id_);
            self->output_profile_status_->setText(
                QStringLiteral("%1 naming %2 · %3 move %4")
                    .arg(self->output_layout_catalog_.size())
                    .arg(self->output_layout_catalog_.size() == 1U ? QStringLiteral("layout")
                                                                   : QStringLiteral("layouts"))
                    .arg(self->destination_catalog_.size())
                    .arg(self->destination_catalog_.size() == 1U ? QStringLiteral("destination")
                                                                 : QStringLiteral("destinations")));
        });
}

void MetadataPropertiesDialog::rebuildOutputProfileControls(
    const std::optional<core::StableId> selected_layout,
    const std::optional<core::StableId> selected_destination) {
    const QSignalBlocker layout_blocker{output_layout_combo_};
    const QSignalBlocker destination_blocker{destination_combo_};
    output_layout_combo_->clear();
    for (const auto& saved : output_layout_catalog_) {
        output_layout_combo_->addItem(display_utf8(saved.profile.name),
                                      QString::fromStdString(saved.id.to_string()));
    }
    destination_combo_->clear();
    for (const auto& saved : destination_catalog_) {
        destination_combo_->addItem(display_utf8(saved.profile.name),
                                    QString::fromStdString(saved.id.to_string()));
    }
    const auto select_id = [](QComboBox* combo, const std::optional<core::StableId>& id) {
        if (!id) {
            return combo->count() > 0 ? 0 : -1;
        }
        return combo->findData(QString::fromStdString(id->to_string()));
    };
    const auto layout_index = select_id(output_layout_combo_, selected_layout);
    const auto destination_index = select_id(destination_combo_, selected_destination);
    output_layout_combo_->setCurrentIndex(layout_index);
    destination_combo_->setCurrentIndex(destination_index);
    selectOutputLayout(layout_index);
    selectDestination(destination_index);
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::selectOutputLayout(const int index) {
    if (index < 0 || index >= output_layout_combo_->count()) {
        editing_output_layout_id_.reset();
        output_layout_name_->clear();
        output_directory_expression_->clear();
        output_basename_expression_->clear();
        updateOutputProfileButtons();
        return;
    }
    const auto id =
        core::StableId::parse(output_layout_combo_->itemData(index).toString().toStdString());
    const auto found = id ? std::ranges::find(output_layout_catalog_, *id,
                                              &persistence::SavedOutputLayoutProfile::id)
                          : output_layout_catalog_.end();
    if (found == output_layout_catalog_.end()) {
        newOutputLayout();
        return;
    }
    editing_output_layout_id_ = found->id;
    output_layout_name_->setText(display_utf8(found->profile.name));
    output_directory_expression_->setText(
        display_utf8(found->profile.relative_directory_expression));
    output_basename_expression_->setText(display_utf8(found->profile.basename_expression));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::selectDestination(const int index) {
    if (index < 0 || index >= destination_combo_->count()) {
        editing_destination_id_.reset();
        destination_root_raw_path_.clear();
        destination_name_->clear();
        destination_root_->clear();
        updateOutputProfileButtons();
        return;
    }
    const auto id =
        core::StableId::parse(destination_combo_->itemData(index).toString().toStdString());
    const auto found =
        id ? std::ranges::find(destination_catalog_, *id, &persistence::SavedDestinationProfile::id)
           : destination_catalog_.end();
    if (found == destination_catalog_.end()) {
        newDestination();
        return;
    }
    editing_destination_id_ = found->id;
    destination_root_raw_path_ = found->profile.root_raw_path;
    destination_name_->setText(display_utf8(found->profile.name));
    destination_root_->setText(
        QString::fromStdString(core::escape_raw_path(destination_root_raw_path_)));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::newOutputLayout() {
    editing_output_layout_id_.reset();
    output_layout_combo_->setCurrentIndex(-1);
    output_layout_name_->clear();
    output_directory_expression_->clear();
    output_basename_expression_->clear();
    output_layout_name_->setFocus();
    output_profile_status_->setText(
        QStringLiteral("Define a reusable relative folder and filename convention"));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::newDestination() {
    editing_destination_id_.reset();
    destination_root_raw_path_.clear();
    destination_combo_->setCurrentIndex(-1);
    destination_name_->clear();
    destination_root_->clear();
    destination_name_->setFocus();
    output_profile_status_->setText(QStringLiteral("Define a named absolute root for Move files"));
    updateOutputProfileButtons();
}

void MetadataPropertiesDialog::saveOutputLayout() {
    if (!output_profile_store_.save_layout || output_profile_mutation_running_) {
        return;
    }
    persistence::SavedOutputLayoutProfile saved{
        .id = editing_output_layout_id_.value_or(core::StableId::random()),
        .profile =
            operations::OutputLayoutProfile{
                .schema_version = 1U,
                .name = encode_utf8(output_layout_name_->text()),
                .dialect = {},
                .relative_directory_expression = encode_utf8(output_directory_expression_->text()),
                .basename_expression = encode_utf8(output_basename_expression_->text()),
                .sanitization_policy = {"linux", 1U},
            },
    };
    if (auto valid = operations::validate_output_layout_profile(saved.profile); !valid) {
        output_profile_status_->setText(QStringLiteral("Naming layout is not valid · %1")
                                            .arg(display_utf8(valid.error().message)));
        return;
    }
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    auto retained_saved = saved;
    output_profile_store_.save_layout(
        std::move(saved), [self, saved = std::move(retained_saved)](QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profile_mutation_running_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not save naming layout · %1").arg(error));
                self->updateOutputProfileButtons();
                return;
            }
            const auto found = std::ranges::find(self->output_layout_catalog_, saved.id,
                                                 &persistence::SavedOutputLayoutProfile::id);
            if (found == self->output_layout_catalog_.end()) {
                self->output_layout_catalog_.push_back(saved);
            } else {
                *found = saved;
            }
            std::ranges::sort(self->output_layout_catalog_, {},
                              [](const auto& profile) { return profile.profile.name; });
            self->editing_output_layout_id_ = saved.id;
            self->rebuildOutputProfileControls(saved.id, self->editing_destination_id_);
            self->output_profile_status_->setText(QStringLiteral("Naming layout saved"));
        });
}

void MetadataPropertiesDialog::saveDestination() {
    if (!output_profile_store_.save_destination || output_profile_mutation_running_) {
        return;
    }
    persistence::SavedDestinationProfile saved{
        .id = editing_destination_id_.value_or(core::StableId::random()),
        .profile =
            operations::DestinationProfile{
                .schema_version = 1U,
                .name = encode_utf8(destination_name_->text()),
                .root_raw_path = destination_root_raw_path_,
                .containment_policy = {"lexical-beneath-root", 1U},
            },
    };
    if (auto valid = operations::validate_destination_profile(saved.profile); !valid) {
        output_profile_status_->setText(QStringLiteral("Move destination is not valid · %1")
                                            .arg(display_utf8(valid.error().message)));
        return;
    }
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    auto retained_saved = saved;
    output_profile_store_.save_destination(
        std::move(saved), [self, saved = std::move(retained_saved)](QString error) mutable {
            if (!self) {
                return;
            }
            self->output_profile_mutation_running_ = false;
            if (!error.isEmpty()) {
                self->output_profile_status_->setText(
                    QStringLiteral("Could not save move destination · %1").arg(error));
                self->updateOutputProfileButtons();
                return;
            }
            const auto found = std::ranges::find(self->destination_catalog_, saved.id,
                                                 &persistence::SavedDestinationProfile::id);
            if (found == self->destination_catalog_.end()) {
                self->destination_catalog_.push_back(saved);
            } else {
                *found = saved;
            }
            std::ranges::sort(self->destination_catalog_, {},
                              [](const auto& profile) { return profile.profile.name; });
            self->editing_destination_id_ = saved.id;
            self->rebuildOutputProfileControls(self->editing_output_layout_id_, saved.id);
            self->output_profile_status_->setText(QStringLiteral("Move destination saved"));
        });
}

void MetadataPropertiesDialog::removeOutputLayout() {
    if (!editing_output_layout_id_ || !output_profile_store_.remove_layout ||
        output_profile_mutation_running_) {
        return;
    }
    const auto id = *editing_output_layout_id_;
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    output_profile_store_.remove_layout(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->output_profile_mutation_running_ = false;
        if (!error.isEmpty()) {
            self->output_profile_status_->setText(
                QStringLiteral("Could not remove naming layout · %1").arg(error));
            self->updateOutputProfileButtons();
            return;
        }
        std::erase_if(self->output_layout_catalog_,
                      [id](const auto& saved) { return saved.id == id; });
        self->editing_output_layout_id_.reset();
        self->rebuildOutputProfileControls({}, self->editing_destination_id_);
        self->output_profile_status_->setText(QStringLiteral("Naming layout removed"));
    });
}

void MetadataPropertiesDialog::removeDestination() {
    if (!editing_destination_id_ || !output_profile_store_.remove_destination ||
        output_profile_mutation_running_) {
        return;
    }
    const auto id = *editing_destination_id_;
    output_profile_mutation_running_ = true;
    updateOutputProfileButtons();
    const QPointer self{this};
    output_profile_store_.remove_destination(id, [self, id](QString error) {
        if (!self) {
            return;
        }
        self->output_profile_mutation_running_ = false;
        if (!error.isEmpty()) {
            self->output_profile_status_->setText(
                QStringLiteral("Could not remove move destination · %1").arg(error));
            self->updateOutputProfileButtons();
            return;
        }
        std::erase_if(self->destination_catalog_,
                      [id](const auto& saved) { return saved.id == id; });
        self->editing_destination_id_.reset();
        self->rebuildOutputProfileControls(self->editing_output_layout_id_, {});
        self->output_profile_status_->setText(QStringLiteral("Move destination removed"));
    });
}

void MetadataPropertiesDialog::updateOutputProfileButtons() {
    if (output_layout_save_button_ == nullptr) {
        return;
    }
    const auto available = !output_profiles_loading_ && !output_profile_mutation_running_;
    output_layout_combo_->setEnabled(available && !output_layout_catalog_.empty());
    destination_combo_->setEnabled(available && !destination_catalog_.empty());
    output_layout_name_->setEnabled(available);
    output_directory_expression_->setEnabled(available);
    output_basename_expression_->setEnabled(available);
    destination_name_->setEnabled(available);
    destination_root_->setEnabled(available);
    destination_browse_button_->setEnabled(available && output_profile_store_.save_destination);
    output_layout_new_button_->setEnabled(available && output_profile_store_.save_layout);
    destination_new_button_->setEnabled(available && output_profile_store_.save_destination);
    output_layout_save_button_->setEnabled(available && output_profile_store_.save_layout &&
                                           !output_layout_name_->text().isEmpty() &&
                                           !output_basename_expression_->text().isEmpty());
    destination_save_button_->setEnabled(available && output_profile_store_.save_destination &&
                                         !destination_name_->text().isEmpty() &&
                                         !destination_root_raw_path_.empty());
    output_layout_remove_button_->setEnabled(available && output_profile_store_.remove_layout &&
                                             editing_output_layout_id_.has_value());
    destination_remove_button_->setEnabled(available && output_profile_store_.remove_destination &&
                                           editing_destination_id_.has_value());
}

void MetadataPropertiesDialog::updateWritePlanButton() {
    if (preview_plan_button_ == nullptr) {
        return;
    }
    preview_plan_button_->setEnabled(
        grid_model_ != nullptr && draft_count_ > 0 && save_tags_check_->isChecked() &&
        !transformation_catalog_loading_ && !write_plan_running_ && !apply_running_ &&
        write_plan_dialog_ == nullptr && apply_dialog_ == nullptr);
    updateTransformationButton();
}

void MetadataPropertiesDialog::invalidateWritePlan() {
    ++write_plan_generation_;
    write_plan_cancellation_.request_cancellation();
    write_plan_cancellation_ = core::CancellationSource{};
    if (write_plan_dialog_ != nullptr) {
        write_plan_dialog_->close();
    }
    updateWritePlanButton();
}

void MetadataPropertiesDialog::previewWritePlan() {
    if (grid_model_ == nullptr || grid_model_->patches().empty() || write_plan_running_ ||
        !save_tags_check_->isChecked()) {
        return;
    }
    metadata::MetadataTransformationChain combined{
        .schema_version = 1U,
        .name = "Automatic saved transformations",
        .actions = {},
    };
    const metadata::MetadataTransformationLimits limits;
    for (const auto& saved : transformation_catalog_) {
        if (!saved.automatic) {
            continue;
        }
        if (saved.chain.actions.size() > limits.actions - combined.actions.size()) {
            read_only_->setText(
                QStringLiteral(
                    "Automatic transformations exceed the %1-step combined limit; disable or "
                    "shorten a chain before previewing the write plan.")
                    .arg(limits.actions));
            return;
        }
        combined.actions.insert(combined.actions.end(), saved.chain.actions.begin(),
                                saved.chain.actions.end());
    }

    auto draft = grid_model_->patches();
    const auto patches = draft.patches();
    std::vector<std::size_t> items;
    items.reserve(patches.size());
    for (const auto& patch : patches) {
        if (items.empty() || items.back() != patch.item_index) {
            items.push_back(patch.item_index);
        }
    }
    if (items.empty()) {
        return;
    }

    ++write_plan_generation_;
    write_plan_job_generation_ = write_plan_generation_;
    write_plan_cancellation_.request_cancellation();
    write_plan_cancellation_ = core::CancellationSource{};
    const auto selection = grid_model_->sharedSelection();
    const auto cancellation = write_plan_cancellation_.token();
    const auto automatic_action_count = combined.actions.size();
    write_plan_running_ = true;
    updateWritePlanButton();
    read_only_->setText(automatic_action_count == 0U
                            ? QStringLiteral("Revalidating every staged physical source for an "
                                             "immutable preview…")
                            : QStringLiteral("Evaluating %1 checked automatic %2 in a temporary "
                                             "draft, then revalidating every staged source…")
                                  .arg(automatic_action_count)
                                  .arg(automatic_action_count == 1U ? QStringLiteral("step")
                                                                    : QStringLiteral("steps")));
    write_plan_watcher_.setFuture(
        QtConcurrent::run([selection, draft = std::move(draft), items = std::move(items),
                           combined = std::move(combined), cancellation]() mutable {
            if (!combined.actions.empty()) {
                auto preview = metadata::plan_metadata_transformation(
                    *selection, draft, items, std::move(combined), cancellation);
                if (!preview) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(preview.error())));
                }

                auto effective_selection = *selection;
                for (const auto& cell : preview->cells) {
                    auto field_index = effective_selection.field_index(cell.canonical_field);
                    if (!field_index) {
                        auto inserted = effective_selection.ensure_missing_field(
                            cell.canonical_field, cell.display_field);
                        if (!inserted) {
                            return std::make_shared<WritePlanResult>(
                                std::unexpected(std::move(inserted.error())));
                        }
                        field_index = *inserted;
                    }
                    auto staged = cell.after
                                      ? draft.replace_values(effective_selection, cell.item_index,
                                                             *field_index, *cell.after)
                                      : draft.remove_field(effective_selection, cell.item_index,
                                                           *field_index);
                    if (!staged) {
                        return std::make_shared<WritePlanResult>(
                            std::unexpected(std::move(staged.error())));
                    }
                }
                return std::make_shared<WritePlanResult>(metadata::revalidate_metadata_write_plan(
                    effective_selection, draft, cancellation));
            }
            return std::make_shared<WritePlanResult>(
                metadata::revalidate_metadata_write_plan(*selection, draft, cancellation));
        }));
}

void MetadataPropertiesDialog::finishWritePlan() {
    const auto generation = write_plan_job_generation_;
    const auto result = write_plan_watcher_.result();
    write_plan_running_ = false;
    updateWritePlanButton();
    if (generation != write_plan_generation_) {
        return;
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The write-plan task returned no result");
        read_only_->setText(QStringLiteral("Write-plan preview failed · %1").arg(message));
        return;
    }

    auto plan = std::make_shared<const metadata::MetadataWritePlan>(std::move(**result));
    const auto source_count = plan->sources.size();
    const auto blocker_count = plan->blocking_issue_count();
    auto* dialog = new MetadataWritePlanDialog(
        plan,
        [this](std::shared_ptr<const metadata::MetadataWritePlan> ready_plan) {
            startApply(std::move(ready_plan));
        },
        this);
    write_plan_dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (write_plan_dialog_ == dialog) {
            write_plan_dialog_ = nullptr;
            updateWritePlanButton();
        }
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    read_only_->setText(
        QStringLiteral("Fresh write-plan preview · %1 physical %2 · %3 blocking %4 · %5")
            .arg(source_count)
            .arg(source_count == 1U ? QStringLiteral("source") : QStringLiteral("sources"))
            .arg(blocker_count)
            .arg(blocker_count == 1U ? QStringLiteral("issue") : QStringLiteral("issues"))
            .arg(plan->ready() ? QStringLiteral("explicit Apply is available")
                               : QStringLiteral("writing is blocked")));
}

void MetadataPropertiesDialog::startApply(std::shared_ptr<const metadata::MetadataWritePlan> plan) {
    if (!plan || !plan->ready() || !plan_applier_factory_ || apply_running_ ||
        apply_dialog_ != nullptr) {
        return;
    }
    auto applier = plan_applier_factory_();
    if (!applier) {
        read_only_->setText(QStringLiteral("Metadata Apply is unavailable"));
        return;
    }
    if (write_plan_dialog_ != nullptr) {
        write_plan_dialog_->close();
    }
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    apply_progress_state_ = std::make_shared<MetadataApplyProgressState>();
    apply_progress_state_->states.assign(plan->sources.size(),
                                         operations::MetadataApplySourceState::pending);
    apply_progress_state_->issues.resize(plan->sources.size());
    apply_running_ = true;
    apply_committed_ = false;
    updateWritePlanButton();
    read_only_->setText(QStringLiteral("Applying the immutable write plan on bounded workers…"));

    auto* dialog = new MetadataApplyDialog(
        *plan, [this] { apply_cancellation_.request_cancellation(); }, this);
    apply_dialog_ = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (apply_dialog_ == dialog) {
            apply_dialog_ = nullptr;
        }
        if (apply_committed_) {
            QTimer::singleShot(0, this, &QDialog::close);
        } else {
            updateWritePlanButton();
        }
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    apply_progress_timer_->start();

    const auto cancellation = apply_cancellation_.token();
    const auto progress_state = apply_progress_state_;
    apply_watcher_.setFuture(
        QtConcurrent::run([plan = std::move(plan), applier = std::move(applier), progress_state,
                           cancellation]() mutable {
            const operations::MetadataApplyProgressCallback progress =
                [progress_state](const operations::MetadataApplyProgress& update) {
                    std::scoped_lock lock{progress_state->mutex};
                    if (update.source_index >= progress_state->states.size()) {
                        return;
                    }
                    progress_state->states[update.source_index] = update.state;
                    progress_state->issues[update.source_index] = update.issue;
                    progress_state->completed_sources = update.completed_sources;
                };
            return std::make_shared<core::Result<operations::MetadataApplyResult>>(
                applier(*plan, progress, cancellation));
        }));
}

void MetadataPropertiesDialog::updateApplyProgress() {
    if (apply_dialog_ == nullptr || !apply_progress_state_) {
        return;
    }
    static_cast<MetadataApplyDialog*>(apply_dialog_.data())->update(*apply_progress_state_);
}

void MetadataPropertiesDialog::finishApply() {
    apply_running_ = false;
    apply_progress_timer_->stop();
    updateApplyProgress();
    const auto result = apply_watcher_.result();
    if (result && *result) {
        apply_committed_ = (**result).committed_source_count() > 0U;
        if (apply_observer_) {
            apply_observer_(**result);
        }
    }
    if (apply_dialog_ != nullptr) {
        auto* dialog = static_cast<MetadataApplyDialog*>(apply_dialog_.data());
        if (result) {
            dialog->finish(*result);
        } else {
            dialog->finish(std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "The metadata Apply task returned no result",
                .context = {},
            }));
        }
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        read_only_->setText(QStringLiteral("Metadata Apply failed · %1").arg(message));
    } else if (apply_committed_) {
        read_only_->setText(
            QStringLiteral("Metadata Apply committed %1 physical %2 · close results to refresh "
                           "Properties")
                .arg((**result).committed_source_count())
                .arg((**result).committed_source_count() == 1U ? QStringLiteral("source")
                                                               : QStringLiteral("sources")));
    } else {
        read_only_->setText(
            QStringLiteral("No sources were committed · preview again before retrying"));
        ++write_plan_generation_;
    }
    updateWritePlanButton();
}

QStringList MetadataPropertiesDialog::metadataFieldNameSuggestions(const QString& query) const {
    using metadata::MetadataFieldSuggestionCandidate;
    using metadata::MetadataFieldSuggestionKind;

    std::vector<MetadataFieldSuggestionCandidate> candidates;
    const auto catalog = metadata::metadata_field_suggestion_catalog();
    const auto present_count = grid_model_ == nullptr ? 0U : grid_model_->selection().field_count();
    candidates.reserve(present_count + recent_field_names_.size() + catalog.size());
    if (grid_model_ != nullptr) {
        const auto& selection = grid_model_->selection();
        for (std::size_t index = 0U; index < selection.field_count(); ++index) {
            const auto& field = selection.field(index);
            if (field.present_item_count > 0U) {
                candidates.push_back(MetadataFieldSuggestionCandidate{
                    .display_name = field.display_name,
                    .kind = MetadataFieldSuggestionKind::present,
                });
            }
        }
    }
    for (const auto& recent : recent_field_names_) {
        candidates.push_back(MetadataFieldSuggestionCandidate{
            .display_name = recent,
            .kind = MetadataFieldSuggestionKind::recent,
        });
    }
    candidates.insert(candidates.end(), catalog.begin(), catalog.end());

    const auto encoded = query.toUtf8();
    const auto suggestions = metadata::suggest_metadata_field_names(
        std::string_view{encoded.constData(), static_cast<std::size_t>(encoded.size())},
        candidates);
    QStringList display_names;
    display_names.reserve(static_cast<qsizetype>(suggestions.size()));
    for (const auto& suggestion : suggestions) {
        display_names.push_back(display_utf8(suggestion.display_name));
    }
    return display_names;
}

std::vector<std::size_t> MetadataPropertiesDialog::selectedItemIndexes() const {
    std::vector<std::size_t> selected_items;
    if (file_list_ == nullptr || file_list_->selectionModel() == nullptr) {
        return selected_items;
    }
    const auto rows = file_list_->selectionModel()->selectedRows(0);
    selected_items.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
        if (row.isValid() && row.row() >= 0) {
            selected_items.push_back(static_cast<std::size_t>(row.row()));
        }
    }
    std::ranges::sort(selected_items);
    return selected_items;
}

void MetadataPropertiesDialog::promptAddField() {
    if (field_name_dialog_ != nullptr) {
        field_name_dialog_->raise();
        field_name_dialog_->activateWindow();
        return;
    }
    if (aggregate_model_ == nullptr || !aggregate_model_->summaryReady() ||
        aggregate_model_->selectedItemCount() == 0U) {
        return;
    }

    auto* prompt = new QInputDialog(this);
    prompt->setObjectName(QStringLiteral("bench-metadata-add-field-dialog"));
    prompt->setWindowTitle(QStringLiteral("Add metadata field"));
    prompt->setLabelText(QStringLiteral("Field name:"));
    prompt->setInputMode(QInputDialog::TextInput);
    prompt->setWindowModality(Qt::WindowModal);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    auto* field_name = prompt->findChild<QLineEdit*>();
    Q_ASSERT(field_name != nullptr);
    field_name->setObjectName(QStringLiteral("bench-metadata-add-field-name"));
    auto* completion_model = new QStringListModel(prompt);
    completion_model->setObjectName(QStringLiteral("bench-metadata-field-completions"));
    auto* completer = new QCompleter(completion_model, prompt);
    completer->setObjectName(QStringLiteral("bench-metadata-field-completer"));
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setMaxVisibleItems(12);
    field_name->setCompleter(completer);
    completion_model->setStringList(metadataFieldNameSuggestions({}));
    connect(prompt, &QInputDialog::textValueChanged, this,
            [this, prompt, field_name, completion_model, completer](const QString& text) {
                completion_model->setStringList(metadataFieldNameSuggestions(text));
                if (text.trimmed().isEmpty() || completion_model->rowCount() == 0) {
                    return;
                }
                QTimer::singleShot(0, prompt, [prompt, field_name, completer] {
                    if (prompt->isVisible() && field_name->hasFocus()) {
                        completer->complete();
                    }
                });
            });
    field_name_dialog_ = prompt;
    updateFieldButtons();
    updateTransformationButton();
    connect(prompt, &QDialog::accepted, this, [this, prompt] {
        std::vector<int> selected_rows;
        if (file_list_ != nullptr && file_list_->selectionModel() != nullptr) {
            const auto indexes = file_list_->selectionModel()->selectedRows(0);
            selected_rows.reserve(static_cast<std::size_t>(indexes.size()));
            for (const auto& index : indexes) {
                selected_rows.push_back(index.row());
            }
        }
        const auto result = aggregate_model_->ensureField(prompt->textValue());
        if (!result) {
            read_only_->setText(QStringLiteral("Field could not be added · %1")
                                    .arg(display_utf8(result.error().message)));
            return;
        }
        const auto trimmed_name = prompt->textValue().trimmed();
        const auto encoded_name = trimmed_name.toUtf8();
        const auto canonical_name = metadata::canonicalize_field_name(std::string_view{
            encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
        std::erase_if(recent_field_names_, [&canonical_name](const std::string& recent) {
            return metadata::canonicalize_field_name(recent) == canonical_name;
        });
        recent_field_names_.insert(
            recent_field_names_.begin(),
            std::string{encoded_name.constData(), static_cast<std::size_t>(encoded_name.size())});
        constexpr auto maximum_recent_field_names = std::size_t{20U};
        if (recent_field_names_.size() > maximum_recent_field_names) {
            recent_field_names_.resize(maximum_recent_field_names);
        }
        if (file_list_ != nullptr && file_list_->selectionModel() != nullptr &&
            grid_model_ != nullptr) {
            QItemSelection restored_selection;
            for (const auto row : selected_rows) {
                const auto track = grid_model_->index(row, 0);
                restored_selection.select(track, track);
            }
            file_list_->selectionModel()->select(restored_selection,
                                                 QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
        }
        loaded_field_count_ = static_cast<std::size_t>(aggregate_model_->rowCount());
        updateSelectionProjection();
        prompt->setProperty("trackknifeFieldRow", *result);
    });
    connect(prompt, &QDialog::finished, this, [this, prompt] {
        bool has_field_row = false;
        const auto field_row = prompt->property("trackknifeFieldRow").toInt(&has_field_row);
        if (field_name_dialog_ == prompt) {
            field_name_dialog_ = nullptr;
        }
        updateFieldButtons();
        updateTransformationButton();
        if (has_field_row && fields_ != nullptr && aggregate_model_ != nullptr) {
            const auto draft = aggregate_model_->index(field_row, 2);
            fields_->setCurrentIndex(draft);
            fields_->selectionModel()->select(draft, QItemSelectionModel::ClearAndSelect |
                                                         QItemSelectionModel::Rows);
            fields_->scrollTo(draft, QAbstractItemView::PositionAtCenter);
            fields_->setFocus(Qt::OtherFocusReason);
        }
    });
    prompt->open();
}

void MetadataPropertiesDialog::removeSelectedFields() {
    if (aggregate_model_ == nullptr || fields_ == nullptr || fields_->selectionModel() == nullptr) {
        return;
    }
    static_cast<void>(aggregate_model_->removeIndexes(fields_->selectionModel()->selectedRows(0)));
}

void MetadataPropertiesDialog::editCurrentValues() {
    if (exact_values_dialog_ != nullptr) {
        exact_values_dialog_->raise();
        exact_values_dialog_->activateWindow();
        return;
    }
    if (grid_model_ == nullptr || aggregate_model_ == nullptr || fields_ == nullptr ||
        !aggregate_model_->summaryReady() || aggregate_model_->selectedItemCount() == 0U) {
        return;
    }

    const auto current = fields_->currentIndex();
    if (!current.isValid()) {
        return;
    }

    const auto field_index = static_cast<std::size_t>(current.row());
    const auto& field = grid_model_->selection().field(field_index);
    const auto value_index = aggregate_model_->index(current.row(), 2);
    const auto current_values = value_index.data(metadata_cell_values_role).toStringList();
    const auto heading =
        QStringLiteral("%1 — %2 selected %3")
            .arg(display_utf8(field.display_name))
            .arg(selected_item_count_)
            .arg(selected_item_count_ == 1U ? QStringLiteral("file") : QStringLiteral("files"));
    QString context;
    if (current_values.isEmpty()) {
        context = QStringLiteral(
            "The selected files do not currently share one exact value list. Values entered "
            "here replace this field on those files.");
    } else {
        context = QStringLiteral(
            "Edit the exact ordered value list applied to the selected files. Duplicates and "
            "empty values remain distinct.");
    }

    auto* editor = new MetadataExactValueDialog(heading, context, current_values, this);
    exact_values_dialog_ = editor;
    edit_values_button_->setEnabled(false);
    updateTransformationButton();
    const QPersistentModelIndex target{value_index};
    connect(editor, &QDialog::accepted, this, [this, editor, target] {
        if (!target.isValid()) {
            return;
        }
        static_cast<void>(aggregate_model_->replaceRowValues(target.row(), editor->values()));
    });
    connect(editor, &QDialog::finished, this, [this, editor] {
        if (exact_values_dialog_ == editor) {
            exact_values_dialog_ = nullptr;
        }
        updateEditValuesButton();
        updateTransformationButton();
    });
    editor->open();
}

void MetadataPropertiesDialog::promptTransformation(
    const std::optional<core::StableId> initially_selected, const bool preview_initially_selected) {
    if (transformation_dialog_ != nullptr) {
        transformation_dialog_->raise();
        transformation_dialog_->activateWindow();
        return;
    }
    if (grid_model_ == nullptr || aggregate_model_ == nullptr ||
        !aggregate_model_->summaryReady() || aggregate_model_->selectedItemCount() == 0U ||
        exact_values_dialog_ != nullptr || field_name_dialog_ != nullptr || write_plan_running_ ||
        apply_running_ || write_plan_dialog_ != nullptr || apply_dialog_ != nullptr) {
        return;
    }
    auto items = selectedItemIndexes();
    if (items.empty()) {
        return;
    }
    QStringList labels;
    labels.reserve(grid_model_->rowCount());
    for (auto row = 0; row < grid_model_->rowCount(); ++row) {
        labels.push_back(grid_model_->trackLabel(row));
    }
    auto* dialog = new MetadataTransformationDialog(
        grid_model_->sharedSelection(), grid_model_->patches(), std::move(items), std::move(labels),
        [this](const metadata::MetadataTransformationPreview& preview) {
            if (grid_model_ == nullptr || !grid_model_->stageTransformation(preview)) {
                return false;
            }
            loaded_field_count_ = grid_model_->selection().field_count();
            updateSelectionProjection();
            return true;
        },
        transformation_store_, this, initially_selected, preview_initially_selected);
    transformation_dialog_ = dialog;
    updateTransformationButton();
    connect(dialog, &QDialog::finished, this, [this, dialog, initially_selected] {
        if (transformation_dialog_ == dialog) {
            transformation_dialog_ = nullptr;
        }
        loadTransformationCatalog(initially_selected);
        updateTransformationButton();
    });
    dialog->open();
}

bool MetadataPropertiesDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == fields_ && event->type() == QEvent::KeyPress && grid_model_ != nullptr) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Undo)) {
            return grid_model_->undo();
        }
        if (key->matches(QKeySequence::Redo)) {
            return grid_model_->redo();
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers() == Qt::ControlModifier) {
            editCurrentValues();
            return true;
        }
        if (key->key() == Qt::Key_Insert && key->modifiers() == Qt::NoModifier) {
            promptAddField();
            return true;
        }
        if (key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier) {
            return aggregate_model_->removeIndexes(fields_->selectionModel()->selectedIndexes());
        }
        if (key->key() == Qt::Key_Backspace && key->modifiers() == Qt::ControlModifier) {
            return aggregate_model_->revertIndexes(fields_->selectionModel()->selectedIndexes());
        }
    }
    return QDialog::eventFilter(watched, event);
}

void MetadataPropertiesDialog::closeEvent(QCloseEvent* event) {
    if (apply_running_) {
        apply_cancellation_.request_cancellation();
        read_only_->setText(
            QStringLiteral("Cancelling Apply after in-flight sources become safe…"));
        event->ignore();
        return;
    }
    if (apply_committed_) {
        write_plan_cancellation_.request_cancellation();
        event->accept();
        return;
    }
    if (draft_count_ == 0 || grid_model_ == nullptr) {
        write_plan_cancellation_.request_cancellation();
        event->accept();
        return;
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Discard metadata draft?"),
        QStringLiteral("%1 staged %2 exist only in memory and have not been written to files.")
            .arg(draft_count_)
            .arg(draft_count_ == 1 ? QStringLiteral("change") : QStringLiteral("changes")),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Discard) {
        write_plan_cancellation_.request_cancellation();
        static_cast<void>(grid_model_->discardAll());
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace trackknife::bench
