// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_properties_dialog.hpp"

#include "bench/file_publication_apply_dialog.hpp"
#include "bench/metadata_apply_dialog.hpp"
#include "bench/metadata_exact_value_dialog.hpp"
#include "bench/metadata_dialog_helpers.hpp"
#include "bench/metadata_grid_model.hpp"
#include "bench/metadata_rule_script_import_dialog.hpp"
#include "bench/metadata_scalar_delegate.hpp"
#include "bench/metadata_transformation_dialog.hpp"
#include "bench/metadata_transformation_preview_model.hpp"
#include "bench/preparation_plan_dialog.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/metadata/field_suggestions.hpp"
#include "trackknife/metadata/rule_script_import.hpp"

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
#include <QFrame>
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
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
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
#include <iterator>
#include <limits>
#include <mutex>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr auto properties_geometry_key = "workspace/metadata-properties-geometry-v1";
constexpr auto properties_content_splitter_key =
    "workspace/metadata-properties-content-splitter-v1";
constexpr auto properties_metadata_splitter_key =
    "workspace/metadata-properties-metadata-splitter-v1";

class EmptyStateListWidget final : public QListWidget {
  public:
    explicit EmptyStateListWidget(QString empty_state, QWidget* parent)
        : QListWidget(parent), empty_state_(std::move(empty_state)) {
        setProperty("bench-empty-state-text", empty_state_);
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        QListWidget::paintEvent(event);
        if (count() != 0 || empty_state_.isEmpty()) {
            return;
        }
        QPainter painter{viewport()};
        painter.setClipRegion(event->region());
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                       QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(16, 16, -16, -16),
                         Qt::AlignCenter | Qt::TextWordWrap, empty_state_);
    }

  private:
    QString empty_state_;
};

} // namespace

MetadataPropertiesDialog::MetadataPropertiesDialog(
    const std::size_t requested_item_count, MetadataPropertiesSourceReader source_reader,
    const std::span<const std::string_view> preferred_fields,
    MetadataWritePlanApplierFactory plan_applier_factory, MetadataApplyObserver apply_observer,
    MetadataTransformationStore transformation_store, OutputProfileStore output_profile_store,
    FilePublicationPlanApplierFactory file_plan_applier_factory,
    FilePublicationApplyObserver file_apply_observer, QWidget* parent,
    MetadataDialogLayoutStore layout_store)
    : QDialog(parent), selection_watcher_(this), write_plan_watcher_(this),
      metadata_apply_watcher_(this), file_apply_watcher_(this),
      source_reader_(std::move(source_reader)),
      plan_applier_factory_(std::move(plan_applier_factory)),
      apply_observer_(std::move(apply_observer)),
      transformation_store_(std::move(transformation_store)),
      output_profile_store_(std::move(output_profile_store)),
      file_plan_applier_factory_(std::move(file_plan_applier_factory)),
      file_apply_observer_(std::move(file_apply_observer)), layout_store_(std::move(layout_store)),
      requested_item_count_(requested_item_count) {
    setObjectName(QStringLiteral("bench-metadata-properties"));
    setWindowTitle(QStringLiteral("Track properties"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1'020, 620);
    restoreLayoutState();

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
    read_only_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto* side_panel = new QWidget(this);
    side_panel->setObjectName(QStringLiteral("bench-metadata-side-panel"));
    side_panel->setMinimumWidth(260);
    side_panel->setMaximumWidth(380);
    transformation_panel_ = side_panel;
    transformation_panel_->hide();
    auto* side_layout = new QVBoxLayout(side_panel);
    // Only the splitter-facing edge is inset so the panel's right edge lines
    // up with the footer buttons below it.
    side_layout->setContentsMargins(8, 0, 0, 0);
    side_layout->setSpacing(6);

    const auto section_heading = [side_panel](const QString& text, const QString& object_name,
                                              const QString& tool_tip) {
        auto* heading = new QLabel(text, side_panel);
        heading->setObjectName(object_name);
        auto heading_font = heading->font();
        heading_font.setBold(true);
        heading->setFont(heading_font);
        heading->setToolTip(tool_tip);
        return heading;
    };

    side_layout->addWidget(section_heading(
        QStringLiteral("When you apply"), QStringLiteral("bench-preparation-actions-heading"),
        QStringLiteral("Every checked action is revalidated together in one preview before "
                       "anything is written")));
    save_tags_check_ = new QCheckBox(QStringLiteral("Save tags"), side_panel);
    save_tags_check_->setObjectName(QStringLiteral("bench-preparation-save-tags"));
    save_tags_check_->setChecked(true);
    save_tags_check_->setToolTip(
        QStringLiteral("Write the drafted tag edits into the files"));
    side_layout->addWidget(save_tags_check_);
    rename_files_check_ = new QCheckBox(QStringLiteral("Rename files"), side_panel);
    rename_files_check_->setObjectName(QStringLiteral("bench-preparation-rename-files"));
    rename_files_check_->setEnabled(false);
    rename_files_check_->setToolTip(QStringLiteral("Choose a naming layout first"));
    side_layout->addWidget(rename_files_check_);
    auto* rename_row = new QHBoxLayout;
    rename_row->setContentsMargins(22, 0, 0, 0);
    rename_row->setSpacing(4);
    output_layout_combo_ = new QComboBox(side_panel);
    output_layout_combo_->setObjectName(QStringLiteral("bench-output-layout-profile"));
    output_layout_combo_->setAccessibleName(QStringLiteral("Saved naming layout"));
    output_layout_combo_->setPlaceholderText(QStringLiteral("None saved yet"));
    rename_row->addWidget(output_layout_combo_, 1);
    auto* manage_layouts_button = new QPushButton(QStringLiteral("Edit…"), side_panel);
    manage_layouts_button->setObjectName(QStringLiteral("bench-output-layout-manage"));
    manage_layouts_button->setToolTip(
        QStringLiteral("Create, change, or remove naming layouts"));
    rename_row->addWidget(manage_layouts_button);
    side_layout->addLayout(rename_row);
    move_files_check_ = new QCheckBox(QStringLiteral("Move files"), side_panel);
    move_files_check_->setObjectName(QStringLiteral("bench-preparation-move-files"));
    move_files_check_->setEnabled(false);
    move_files_check_->setToolTip(
        QStringLiteral("Choose a naming layout and a destination first"));
    side_layout->addWidget(move_files_check_);
    auto* move_row = new QHBoxLayout;
    move_row->setContentsMargins(22, 0, 0, 0);
    move_row->setSpacing(4);
    destination_combo_ = new QComboBox(side_panel);
    destination_combo_->setObjectName(QStringLiteral("bench-destination-profile"));
    destination_combo_->setAccessibleName(QStringLiteral("Saved move destination"));
    destination_combo_->setPlaceholderText(QStringLiteral("None saved yet"));
    move_row->addWidget(destination_combo_, 1);
    auto* manage_destinations_button = new QPushButton(QStringLiteral("Edit…"), side_panel);
    manage_destinations_button->setObjectName(QStringLiteral("bench-destination-manage"));
    manage_destinations_button->setToolTip(
        QStringLiteral("Create, change, or remove move destinations"));
    move_row->addWidget(manage_destinations_button);
    side_layout->addLayout(move_row);
    replaygain_check_ = new QCheckBox(QStringLiteral("ReplayGain"), side_panel);
    replaygain_check_->setObjectName(QStringLiteral("bench-preparation-replaygain"));
    replaygain_check_->setEnabled(false);
    replaygain_check_->setToolTip(QStringLiteral("ReplayGain analysis is planned for M7"));
    side_layout->addWidget(replaygain_check_);
    output_profile_status_ = new QLabel(QStringLiteral("Loading output profiles…"), side_panel);
    output_profile_status_->setObjectName(QStringLiteral("bench-output-profile-status"));
    output_profile_status_->setWordWrap(true);
    side_layout->addWidget(output_profile_status_);

    auto* section_separator = new QFrame(side_panel);
    section_separator->setFrameShape(QFrame::HLine);
    section_separator->setFrameShadow(QFrame::Sunken);
    side_layout->addWidget(section_separator);

    side_layout->addWidget(section_heading(
        QStringLiteral("Scripts"), QStringLiteral("bench-metadata-scripts-heading"),
        QStringLiteral("Checked scripts run automatically whenever this draft is previewed and "
                       "applied; the checked state is saved")));
    transformation_list_ =
        new EmptyStateListWidget(QStringLiteral("No saved scripts yet"), side_panel);
    transformation_list_->setObjectName(QStringLiteral("bench-metadata-transformation-list"));
    transformation_list_->setAccessibleName(QStringLiteral("Saved tagging scripts"));
    transformation_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    transformation_list_->setAlternatingRowColors(true);
    transformation_list_->setEnabled(false);
    side_layout->addWidget(transformation_list_, 1);
    transformation_status_ = new QLabel(QStringLiteral("Loading saved scripts…"), side_panel);
    transformation_status_->setObjectName(QStringLiteral("bench-metadata-transformation-status"));
    transformation_status_->setWordWrap(true);
    side_layout->addWidget(transformation_status_);
    transform_button_ = new QPushButton(QStringLiteral("Open script editor…"), side_panel);
    transform_button_->setObjectName(QStringLiteral("bench-metadata-transform"));
    transform_button_->setToolTip(
        QStringLiteral("Open the selected saved script, or create a new one"));
    transform_button_->setEnabled(false);
    side_layout->addWidget(transform_button_);

    auto* layout_manager = new QDialog(this);
    layout_manager->setObjectName(QStringLiteral("bench-output-layout-manager"));
    layout_manager->setWindowTitle(QStringLiteral("Naming layouts"));
    layout_manager->setModal(false);
    layout_manager->setMinimumWidth(460);
    auto* layout_manager_box = new QVBoxLayout(layout_manager);
    auto* layout_manager_hint = new QLabel(
        QStringLiteral("Changes the layout currently selected in Track properties. "
                       "“New” starts a blank layout."),
        layout_manager);
    layout_manager_hint->setWordWrap(true);
    layout_manager_box->addWidget(layout_manager_hint);
    auto* layout_form = new QFormLayout;
    output_layout_name_ = new QLineEdit(layout_manager);
    output_layout_name_->setObjectName(QStringLiteral("bench-output-layout-name"));
    output_layout_name_->setPlaceholderText(QStringLiteral("For example: Album folders"));
    layout_form->addRow(QStringLiteral("Name:"), output_layout_name_);
    output_directory_expression_ = new QLineEdit(layout_manager);
    output_directory_expression_->setObjectName(
        QStringLiteral("bench-output-layout-directory-expression"));
    output_directory_expression_->setPlaceholderText(
        QStringLiteral("For example: %album artist%/%album%"));
    layout_form->addRow(QStringLiteral("Folders:"), output_directory_expression_);
    output_basename_expression_ = new QLineEdit(layout_manager);
    output_basename_expression_->setObjectName(
        QStringLiteral("bench-output-layout-basename-expression"));
    output_basename_expression_->setPlaceholderText(
        QStringLiteral("For example: %tracknumber% - %title%"));
    layout_form->addRow(QStringLiteral("Filename:"), output_basename_expression_);
    layout_manager_box->addLayout(layout_form);
    auto* layout_buttons = new QHBoxLayout;
    output_layout_new_button_ = new QPushButton(QStringLiteral("New"), layout_manager);
    output_layout_new_button_->setObjectName(QStringLiteral("bench-output-layout-new"));
    output_layout_new_button_->setAutoDefault(false);
    output_layout_save_button_ = new QPushButton(QStringLiteral("Save"), layout_manager);
    output_layout_save_button_->setObjectName(QStringLiteral("bench-output-layout-save"));
    output_layout_save_button_->setAutoDefault(false);
    output_layout_remove_button_ = new QPushButton(QStringLiteral("Remove"), layout_manager);
    output_layout_remove_button_->setObjectName(QStringLiteral("bench-output-layout-remove"));
    output_layout_remove_button_->setAutoDefault(false);
    layout_buttons->addWidget(output_layout_new_button_);
    layout_buttons->addWidget(output_layout_save_button_);
    layout_buttons->addWidget(output_layout_remove_button_);
    layout_buttons->addStretch(1);
    auto* layout_manager_close = new QPushButton(QStringLiteral("Close"), layout_manager);
    layout_manager_close->setObjectName(QStringLiteral("bench-output-layout-manager-close"));
    layout_manager_close->setAutoDefault(false);
    layout_buttons->addWidget(layout_manager_close);
    layout_manager_box->addLayout(layout_buttons);
    connect(layout_manager_close, &QPushButton::clicked, layout_manager, &QDialog::close);
    connect(manage_layouts_button, &QPushButton::clicked, layout_manager, [layout_manager] {
        layout_manager->show();
        layout_manager->raise();
        layout_manager->activateWindow();
    });

    auto* destination_manager = new QDialog(this);
    destination_manager->setObjectName(QStringLiteral("bench-destination-manager"));
    destination_manager->setWindowTitle(QStringLiteral("Move destinations"));
    destination_manager->setModal(false);
    destination_manager->setMinimumWidth(460);
    auto* destination_manager_box = new QVBoxLayout(destination_manager);
    auto* destination_manager_hint = new QLabel(
        QStringLiteral("Changes the destination currently selected in Track properties. "
                       "“New” starts a blank destination."),
        destination_manager);
    destination_manager_hint->setWordWrap(true);
    destination_manager_box->addWidget(destination_manager_hint);
    auto* destination_form = new QFormLayout;
    destination_name_ = new QLineEdit(destination_manager);
    destination_name_->setObjectName(QStringLiteral("bench-destination-name"));
    destination_name_->setPlaceholderText(QStringLiteral("For example: Music library"));
    destination_form->addRow(QStringLiteral("Name:"), destination_name_);
    auto* root_row = new QHBoxLayout;
    destination_root_ = new QLineEdit(destination_manager);
    destination_root_->setObjectName(QStringLiteral("bench-destination-root"));
    destination_root_->setPlaceholderText(QStringLiteral("Choose an absolute folder"));
    destination_browse_button_ = new QPushButton(QStringLiteral("Browse…"), destination_manager);
    destination_browse_button_->setObjectName(QStringLiteral("bench-destination-browse"));
    root_row->addWidget(destination_root_, 1);
    root_row->addWidget(destination_browse_button_);
    destination_form->addRow(QStringLiteral("Root:"), root_row);
    destination_manager_box->addLayout(destination_form);
    auto* destination_buttons = new QHBoxLayout;
    destination_new_button_ = new QPushButton(QStringLiteral("New"), destination_manager);
    destination_new_button_->setObjectName(QStringLiteral("bench-destination-new"));
    destination_new_button_->setAutoDefault(false);
    destination_save_button_ = new QPushButton(QStringLiteral("Save"), destination_manager);
    destination_save_button_->setObjectName(QStringLiteral("bench-destination-save"));
    destination_save_button_->setAutoDefault(false);
    destination_remove_button_ = new QPushButton(QStringLiteral("Remove"), destination_manager);
    destination_remove_button_->setObjectName(QStringLiteral("bench-destination-remove"));
    destination_remove_button_->setAutoDefault(false);
    destination_buttons->addWidget(destination_new_button_);
    destination_buttons->addWidget(destination_save_button_);
    destination_buttons->addWidget(destination_remove_button_);
    destination_buttons->addStretch(1);
    auto* destination_manager_close = new QPushButton(QStringLiteral("Close"), destination_manager);
    destination_manager_close->setObjectName(QStringLiteral("bench-destination-manager-close"));
    destination_manager_close->setAutoDefault(false);
    destination_buttons->addWidget(destination_manager_close);
    destination_manager_box->addLayout(destination_buttons);
    connect(destination_manager_close, &QPushButton::clicked, destination_manager,
            &QDialog::close);
    connect(manage_destinations_button, &QPushButton::clicked, destination_manager,
            [destination_manager] {
                destination_manager->show();
                destination_manager->raise();
                destination_manager->activateWindow();
            });

    loading_ = new QLabel(QStringLiteral("Preparing metadata grid…"), this);
    loading_->setObjectName(QStringLiteral("bench-metadata-loading"));
    loading_->setAlignment(Qt::AlignCenter);
    root_layout_->addWidget(loading_, 1);

    grid_tools_ = new QWidget(this);
    grid_tools_->setObjectName(QStringLiteral("bench-metadata-grid-tools"));
    grid_tools_->hide();
    auto* grid_tools_layout = new QHBoxLayout(grid_tools_);
    grid_tools_layout->setContentsMargins(0, 0, 0, 0);
    grid_tools_layout->setSpacing(6);
    add_field_button_ = new QPushButton(QStringLiteral("Add field…"), grid_tools_);
    add_field_button_->setObjectName(QStringLiteral("bench-metadata-add-field"));
    add_field_button_->setToolTip(QStringLiteral("Add an arbitrary metadata field (Insert)"));
    add_field_button_->setEnabled(false);
    grid_tools_layout->addWidget(add_field_button_);
    remove_field_button_ = new QPushButton(QStringLiteral("Remove field"), grid_tools_);
    remove_field_button_->setObjectName(QStringLiteral("bench-metadata-remove-field"));
    remove_field_button_->setToolTip(
        QStringLiteral("Remove the selected fields from the selected files (Delete)"));
    remove_field_button_->setEnabled(false);
    grid_tools_layout->addWidget(remove_field_button_);
    edit_values_button_ = new QPushButton(QStringLiteral("Edit values…"), grid_tools_);
    edit_values_button_->setObjectName(QStringLiteral("bench-metadata-edit-values"));
    edit_values_button_->setToolTip(
        QStringLiteral("Edit the exact ordered value list (Ctrl+Enter)"));
    edit_values_button_->setEnabled(false);
    grid_tools_layout->addWidget(edit_values_button_);
    grid_tools_layout->addStretch(1);
    undo_button_ = new QPushButton(QStringLiteral("Undo"), grid_tools_);
    undo_button_->setObjectName(QStringLiteral("bench-metadata-undo"));
    undo_button_->setToolTip(QStringLiteral("Undo the last draft edit (Ctrl+Z)"));
    undo_button_->setShortcut(QKeySequence::Undo);
    undo_button_->setEnabled(false);
    grid_tools_layout->addWidget(undo_button_);
    redo_button_ = new QPushButton(QStringLiteral("Redo"), grid_tools_);
    redo_button_->setObjectName(QStringLiteral("bench-metadata-redo"));
    redo_button_->setToolTip(QStringLiteral("Redo the last undone draft edit (Ctrl+Shift+Z)"));
    redo_button_->setShortcut(QKeySequence::Redo);
    redo_button_->setEnabled(false);
    grid_tools_layout->addWidget(redo_button_);
    discard_button_ = new QPushButton(QStringLiteral("Discard"), grid_tools_);
    discard_button_->setObjectName(QStringLiteral("bench-metadata-discard"));
    discard_button_->setToolTip(QStringLiteral("Throw away every pending draft edit"));
    discard_button_->setEnabled(false);
    grid_tools_layout->addWidget(discard_button_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons_->setObjectName(QStringLiteral("bench-metadata-buttons"));
    preview_plan_button_ =
        buttons_->addButton(QStringLiteral("Preview changes…"), QDialogButtonBox::ActionRole);
    preview_plan_button_->setObjectName(QStringLiteral("bench-metadata-preview-write-plan"));
    preview_plan_button_->setToolTip(QStringLiteral(
        "Revalidate every enabled change against the files and review it before applying"));
    preview_plan_button_->setEnabled(false);
    preview_plan_button_->setDefault(true);
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
    connect(output_layout_combo_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        selectOutputLayout(index);
        invalidateWritePlan();
    });
    connect(destination_combo_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        selectDestination(index);
        invalidateWritePlan();
    });
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
    connect(save_tags_check_, &QCheckBox::toggled, this, [this] {
        invalidateWritePlan();
        updateDraftState(draft_count_, undo_button_->isEnabled(), redo_button_->isEnabled());
    });
    for (auto* path_choice : {rename_files_check_, move_files_check_}) {
        connect(path_choice, &QCheckBox::toggled, this, [this] {
            invalidateWritePlan();
            updateWritePlanButton();
        });
    }
    connect(preview_plan_button_, &QPushButton::clicked, this,
            &MetadataPropertiesDialog::previewWritePlan);
    connect(&write_plan_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishWritePlan);
    connect(&metadata_apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishMetadataApply);
    connect(&file_apply_watcher_, &QFutureWatcherBase::finished, this,
            &MetadataPropertiesDialog::finishFileApply);
    apply_progress_timer_ = new QTimer(this);
    apply_progress_timer_->setInterval(50);
    connect(apply_progress_timer_, &QTimer::timeout, this,
            &MetadataPropertiesDialog::updateApplyProgress);
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("bench-metadata-footer"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(0, 0, 0, 0);
    footer_layout->setSpacing(8);
    footer_layout->addWidget(read_only_, 1);
    footer_layout->addWidget(buttons_);
    root_layout_->addWidget(footer);
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
        metadata_apply_watcher_.waitForFinished();
        file_apply_watcher_.waitForFinished();
    }
}

void MetadataPropertiesDialog::restoreLayoutState() {
    if (!layout_store_.load) {
        return;
    }
    const QPointer self{this};
    layout_store_.load(QString::fromLatin1(properties_geometry_key),
                       [self](QByteArray state, const QString& error) {
                           if (self && error.isEmpty() && !state.isEmpty()) {
                               static_cast<void>(self->restoreGeometry(state));
                           }
                       });
    layout_store_.load(QString::fromLatin1(properties_content_splitter_key),
                       [self](QByteArray state, const QString& error) {
                           if (!self || !error.isEmpty() || state.isEmpty()) {
                               return;
                           }
                           self->pending_content_splitter_state_ = std::move(state);
                           if (self->content_splitter_ != nullptr) {
                               static_cast<void>(self->content_splitter_->restoreState(
                                   self->pending_content_splitter_state_));
                           }
                       });
    layout_store_.load(QString::fromLatin1(properties_metadata_splitter_key),
                       [self](QByteArray state, const QString& error) {
                           if (!self || !error.isEmpty() || state.isEmpty()) {
                               return;
                           }
                           self->pending_metadata_splitter_state_ = std::move(state);
                           if (self->metadata_splitter_ != nullptr) {
                               static_cast<void>(self->metadata_splitter_->restoreState(
                                   self->pending_metadata_splitter_state_));
                           }
                       });
}

void MetadataPropertiesDialog::persistLayoutState() {
    if (layout_state_saved_ || !layout_store_.save) {
        return;
    }
    layout_state_saved_ = true;
    layout_store_.save(QString::fromLatin1(properties_geometry_key), saveGeometry(), {});
    if (content_splitter_ != nullptr) {
        layout_store_.save(QString::fromLatin1(properties_content_splitter_key),
                           content_splitter_->saveState(), {});
    }
    if (metadata_splitter_ != nullptr) {
        layout_store_.save(QString::fromLatin1(properties_metadata_splitter_key),
                           metadata_splitter_->saveState(), {});
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

    content_splitter_ = new QSplitter(Qt::Horizontal, this);
    content_splitter_->setObjectName(QStringLiteral("bench-metadata-content-splitter"));
    content_splitter_->setChildrenCollapsible(false);
    metadata_splitter_ = new QSplitter(Qt::Vertical, content_splitter_);
    metadata_splitter_->setObjectName(QStringLiteral("bench-metadata-splitter"));
    metadata_splitter_->setChildrenCollapsible(false);

    file_list_ = new QTableView(metadata_splitter_);
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

    fields_ = new QTableView(metadata_splitter_);
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
    fields_->setItemDelegate(createMetadataScalarDelegate(fields_));
    fields_->installEventFilter(this);
    fields_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    fields_->verticalHeader()->hide();
    fields_->verticalHeader()->setDefaultSectionSize(24);
    fields_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    fields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    fields_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    fields_->setColumnWidth(0, 190);

    auto* fields_pane = new QWidget(metadata_splitter_);
    fields_pane->setObjectName(QStringLiteral("bench-metadata-fields-pane"));
    auto* fields_pane_layout = new QVBoxLayout(fields_pane);
    fields_pane_layout->setContentsMargins(0, 0, 0, 0);
    fields_pane_layout->setSpacing(4);
    grid_tools_->setParent(fields_pane);
    fields_pane_layout->addWidget(grid_tools_);
    fields_pane_layout->addWidget(fields_, 1);
    grid_tools_->show();

    metadata_splitter_->addWidget(file_list_);
    metadata_splitter_->addWidget(fields_pane);
    metadata_splitter_->setStretchFactor(0, 1);
    metadata_splitter_->setStretchFactor(1, 3);
    metadata_splitter_->setSizes({170, 390});
    if (!pending_metadata_splitter_state_.isEmpty()) {
        static_cast<void>(metadata_splitter_->restoreState(pending_metadata_splitter_state_));
    }
    content_splitter_->addWidget(metadata_splitter_);
    content_splitter_->addWidget(transformation_panel_);
    transformation_panel_->show();
    content_splitter_->setStretchFactor(0, 1);
    content_splitter_->setStretchFactor(1, 0);
    content_splitter_->setSizes({760, 260});
    if (!pending_content_splitter_state_.isEmpty()) {
        static_cast<void>(content_splitter_->restoreState(pending_content_splitter_state_));
    }
    root_layout_->insertWidget(root_layout_->count() - 1, content_splitter_, 1);

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
            ? QStringLiteral(
                  "Save tags is off · tag edits stay in the draft and Rename/Move uses the "
                  "file's current tags")
            : (patch_count == 0
                   ? QStringLiteral("No pending edits")
                   : QStringLiteral("Draft only · nothing is written until you preview and "
                                    "apply")));
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
    const auto has_saved_scripts = !transformation_catalog_.empty();
    transformation_status_->setVisible(has_saved_scripts);
    transformation_status_->setText(
        has_saved_scripts
            ? QStringLiteral("%1 of %2 checked · run in the order shown")
                  .arg(automatic_count)
                  .arg(transformation_catalog_.size())
            : QString{});
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
    const auto layout_ready =
        editing_output_layout_id_.has_value() &&
        std::ranges::any_of(output_layout_catalog_, [this](const auto& entry) {
            return entry.id == *editing_output_layout_id_;
        });
    const auto destination_ready =
        editing_destination_id_.has_value() &&
        std::ranges::any_of(destination_catalog_, [this](const auto& entry) {
            return entry.id == *editing_destination_id_;
        });
    const auto publication_available = static_cast<bool>(file_plan_applier_factory_);
    rename_files_check_->setEnabled(available && layout_ready && publication_available);
    move_files_check_->setEnabled(available && layout_ready && destination_ready &&
                                  publication_available);
    rename_files_check_->setToolTip(
        publication_available
            ? (layout_ready ? QStringLiteral("Generate a new basename with the saved layout")
                            : QStringLiteral("Select a saved naming layout first"))
            : QStringLiteral("File publication is unavailable"));
    move_files_check_->setToolTip(
        publication_available
            ? (layout_ready && destination_ready
                   ? QStringLiteral("Move below the saved destination using the saved layout")
                   : QStringLiteral("Select a saved layout and destination first"))
            : QStringLiteral("File publication is unavailable"));
    if (!rename_files_check_->isEnabled() && rename_files_check_->isChecked()) {
        rename_files_check_->setChecked(false);
    }
    if (!move_files_check_->isEnabled() && move_files_check_->isChecked()) {
        move_files_check_->setChecked(false);
    }
}

void MetadataPropertiesDialog::updateWritePlanButton() {
    if (preview_plan_button_ == nullptr) {
        return;
    }
    const auto has_automatic = std::ranges::any_of(transformation_catalog_, [](const auto& entry) {
        return entry.automatic && !entry.chain.actions.empty();
    });
    const auto has_metadata_effect =
        save_tags_check_->isChecked() && (draft_count_ > 0 || has_automatic);
    const auto has_path_effect = rename_files_check_->isChecked() || move_files_check_->isChecked();
    preview_plan_button_->setEnabled(
        grid_model_ != nullptr && (has_metadata_effect || has_path_effect) &&
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
    const operations::PreparationOperationSelection operation_selection{
        .save_tags = save_tags_check_->isChecked(),
        .rename_files = rename_files_check_->isChecked(),
        .move_files = move_files_check_->isChecked(),
        .replaygain = replaygain_check_->isChecked(),
    };
    const auto has_path_operation =
        operation_selection.rename_files || operation_selection.move_files;
    if (grid_model_ == nullptr || write_plan_running_ ||
        (!operation_selection.save_tags && !has_path_operation)) {
        return;
    }

    std::optional<operations::OutputLayoutProfile> output_layout;
    std::optional<operations::DestinationProfile> destination;
    if (has_path_operation) {
        if (!editing_output_layout_id_) {
            read_only_->setText(QStringLiteral("Select a saved naming layout before preview"));
            return;
        }
        const auto layout = std::ranges::find(output_layout_catalog_, *editing_output_layout_id_,
                                              &persistence::SavedOutputLayoutProfile::id);
        if (layout == output_layout_catalog_.end()) {
            read_only_->setText(QStringLiteral("The selected naming layout is unavailable"));
            return;
        }
        output_layout = layout->profile;
        if (operation_selection.move_files) {
            if (!editing_destination_id_) {
                read_only_->setText(
                    QStringLiteral("Select a saved move destination before preview"));
                return;
            }
            const auto selected_destination =
                std::ranges::find(destination_catalog_, *editing_destination_id_,
                                  &persistence::SavedDestinationProfile::id);
            if (selected_destination == destination_catalog_.end()) {
                read_only_->setText(QStringLiteral("The selected move destination is unavailable"));
                return;
            }
            destination = selected_destination->profile;
        }
    }
    metadata::MetadataTransformationChain combined{
        .schema_version = 1U,
        .name = "Automatic saved transformations",
        .actions = {},
    };
    const metadata::MetadataTransformationLimits limits;
    if (operation_selection.save_tags) {
        for (const auto& saved : transformation_catalog_) {
            if (!saved.automatic) {
                continue;
            }
            if (saved.chain.actions.size() > limits.actions - combined.actions.size()) {
                read_only_->setText(
                    QStringLiteral(
                        "Automatic transformations exceed the %1-step combined limit; disable or "
                        "shorten a chain before previewing the preparation.")
                        .arg(limits.actions));
                return;
            }
            combined.actions.insert(combined.actions.end(), saved.chain.actions.begin(),
                                    saved.chain.actions.end());
        }
    }

    auto draft = grid_model_->patches();
    std::vector<std::size_t> items;
    items.reserve(grid_model_->selection().item_count());
    for (std::size_t item_index = 0U; item_index < grid_model_->selection().item_count();
         ++item_index) {
        items.push_back(item_index);
    }
    if (items.empty() || (draft.empty() && combined.actions.empty() && !has_path_operation)) {
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
    read_only_->setText(
        !operation_selection.save_tags && has_path_operation
            ? QStringLiteral("Planning Rename/Move strictly from current source tags, then "
                             "freshly preflighting every path…")
        : automatic_action_count == 0U
            ? QStringLiteral("Rechecking every enabled change against the files…")
            : QStringLiteral("Running %1 checked %2, then rechecking every enabled "
                             "change…")
                  .arg(automatic_action_count)
                  .arg(automatic_action_count == 1U ? QStringLiteral("step")
                                                    : QStringLiteral("steps")));
    write_plan_watcher_.setFuture(
        QtConcurrent::run([selection, draft = std::move(draft), items = std::move(items),
                           combined = std::move(combined), operation_selection,
                           output_layout = std::move(output_layout),
                           destination = std::move(destination), cancellation]() mutable {
            auto effective_selection = *selection;
            if (!combined.actions.empty()) {
                auto preview = metadata::plan_metadata_transformation(
                    *selection, draft, items, std::move(combined), cancellation);
                if (!preview) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(preview.error())));
                }
                for (const auto& cell : preview->cells) {
                    auto field_index =
                        cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                            ? effective_selection.exact_native_field_index(cell.display_field)
                            : effective_selection.field_index(cell.canonical_field);
                    if (!field_index) {
                        auto inserted =
                            cell.match_mode == metadata::MetadataFieldMatchMode::exact_native
                                ? effective_selection.ensure_exact_native_field(
                                      cell.display_field, "Exact native: " + cell.display_field)
                                : effective_selection.ensure_missing_field(cell.canonical_field,
                                                                           cell.display_field);
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
            }

            const auto metadata_context_change_count =
                operation_selection.save_tags ? draft.patch_count() : 0U;
            std::optional<metadata::MetadataWritePlan> metadata_plan;
            if (operation_selection.save_tags && !draft.empty()) {
                auto revalidated = metadata::revalidate_metadata_write_plan(effective_selection,
                                                                            draft, cancellation);
                if (!revalidated) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(revalidated.error())));
                }
                metadata_plan = std::move(*revalidated);
            }

            std::optional<operations::OutputPathPlan> path_plan;
            std::optional<operations::OutputPathPreflight> path_preflight;
            if (operation_selection.rename_files || operation_selection.move_files) {
                const metadata::StagedMetadataPatchSet actual_source_tags;
                const auto& naming_selection =
                    operation_selection.save_tags ? effective_selection : *selection;
                const auto& naming_context =
                    operation_selection.save_tags ? draft : actual_source_tags;
                auto documents = metadata::materialize_metadata_draft(
                    naming_selection, naming_context, items, cancellation);
                if (!documents) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(documents.error())));
                }
                std::vector<operations::OutputPathPlanningItem> planning_items;
                planning_items.reserve(items.size());
                for (std::size_t position = 0U; position < items.size(); ++position) {
                    const auto item_index = items[position];
                    const auto& source = naming_selection.source(item_index);
                    if (!source.source_revision) {
                        return std::make_shared<WritePlanResult>(std::unexpected(core::Error{
                            .code = core::ErrorCode::conflict,
                            .message = "File path planning requires a fresh source revision "
                                       "for every selected track",
                            .context = {{.key = "item", .value = std::to_string(item_index)}},
                        }));
                    }
                    planning_items.push_back(operations::OutputPathPlanningItem{
                        .item_index = item_index,
                        .source_raw_path = source.raw_path,
                        .source_revision = *source.source_revision,
                        .final_metadata = std::move((*documents)[position]),
                    });
                }
                auto planned = operations::plan_output_paths(
                    planning_items,
                    operations::OutputPathOperationSelection{
                        .rename_files = operation_selection.rename_files,
                        .move_files = operation_selection.move_files,
                    },
                    std::move(*output_layout), std::move(destination), {}, cancellation);
                if (!planned) {
                    return std::make_shared<WritePlanResult>(
                        std::unexpected(std::move(planned.error())));
                }
                path_plan = std::move(*planned);
                if (path_plan->ready()) {
                    auto checked = operations::preflight_output_paths(*path_plan, cancellation);
                    if (!checked) {
                        return std::make_shared<WritePlanResult>(
                            std::unexpected(std::move(checked.error())));
                    }
                    path_preflight = std::move(*checked);
                }
            }
            return std::make_shared<WritePlanResult>(operations::assemble_preparation_plan(
                operation_selection, metadata_context_change_count, std::move(metadata_plan),
                std::move(path_plan), std::move(path_preflight)));
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
                                    : QStringLiteral("The preparation task returned no result");
        read_only_->setText(QStringLiteral("Preparation preview failed · %1").arg(message));
        return;
    }

    auto plan = std::make_shared<const operations::PreparationPlan>(std::move(**result));
    const auto source_count = plan->output_paths
                                  ? plan->output_paths->sources.size()
                                  : (plan->metadata ? plan->metadata->sources.size() : 0U);
    const auto blocker_count = plan->blocking_issue_count();
    auto* dialog = createPreparationPlanDialog(
        plan,
        [this](std::shared_ptr<const operations::PreparationPlan> ready_plan) {
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
        QStringLiteral("Fresh preparation review · %1 physical %2 · %3 blocking %4 · %5")
            .arg(source_count)
            .arg(source_count == 1U ? QStringLiteral("source") : QStringLiteral("sources"))
            .arg(blocker_count)
            .arg(blocker_count == 1U ? QStringLiteral("issue") : QStringLiteral("issues"))
            .arg(plan->ready() ? QStringLiteral("explicit Apply is available")
                               : QStringLiteral("Apply is blocked")));
}

void MetadataPropertiesDialog::startApply(std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan || !plan->ready() || apply_running_ || apply_dialog_ != nullptr) {
        return;
    }
    if (plan->has_path_operation()) {
        startFileApply(std::move(plan));
        return;
    }
    startMetadataApply(std::move(plan));
}

void MetadataPropertiesDialog::startMetadataApply(
    std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->metadata || !plan_applier_factory_) {
        read_only_->setText(QStringLiteral("Metadata Apply is unavailable"));
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
    apply_progress_state_->states.assign(plan->metadata->sources.size(),
                                         operations::MetadataApplySourceState::pending);
    apply_progress_state_->issues.resize(plan->metadata->sources.size());
    file_apply_progress_state_.reset();
    apply_running_ = true;
    applying_file_paths_ = false;
    apply_committed_ = false;
    updateWritePlanButton();
    read_only_->setText(QStringLiteral("Applying tag changes…"));

    auto* dialog = createMetadataApplyDialog(
        *plan->metadata, [this] { apply_cancellation_.request_cancellation(); }, this);
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
    metadata_apply_watcher_.setFuture(
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
                applier(*plan->metadata, progress, cancellation));
        }));
}

void MetadataPropertiesDialog::startFileApply(
    std::shared_ptr<const operations::PreparationPlan> plan) {
    if (!plan->path_preflight || !file_plan_applier_factory_) {
        read_only_->setText(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    auto applier = file_plan_applier_factory_();
    if (!applier) {
        read_only_->setText(QStringLiteral("File publication Apply is unavailable"));
        return;
    }
    if (write_plan_dialog_ != nullptr) {
        write_plan_dialog_->close();
    }
    apply_cancellation_.request_cancellation();
    apply_cancellation_ = core::CancellationSource{};
    file_apply_progress_state_ = std::make_shared<FilePublicationApplyProgressState>();
    file_apply_progress_state_->states.assign(plan->path_preflight->sources.size(),
                                              operations::FilePublicationApplySourceState::pending);
    file_apply_progress_state_->issues.resize(plan->path_preflight->sources.size());
    apply_progress_state_.reset();
    apply_running_ = true;
    applying_file_paths_ = true;
    apply_committed_ = false;
    updateWritePlanButton();
    read_only_->setText(
        QStringLiteral("Applying file renames and moves…"));

    auto* dialog = createFilePublicationApplyDialog(
        *plan->path_preflight, [this] { apply_cancellation_.request_cancellation(); }, this);
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
    const auto progress_state = file_apply_progress_state_;
    file_apply_watcher_.setFuture(
        QtConcurrent::run([plan = std::move(plan), applier = std::move(applier), progress_state,
                           cancellation]() mutable {
            const operations::FilePublicationApplyProgressCallback progress =
                [progress_state](const operations::FilePublicationApplyProgress& update) {
                    std::scoped_lock lock{progress_state->mutex};
                    if (update.source_index >= progress_state->states.size()) {
                        return;
                    }
                    progress_state->states[update.source_index] = update.state;
                    progress_state->issues[update.source_index] = update.issue;
                    progress_state->completed_sources = update.completed_sources;
                };
            return std::make_shared<core::Result<operations::FilePublicationApplyResult>>(
                applier(*plan->path_preflight, progress, cancellation));
        }));
}

void MetadataPropertiesDialog::updateApplyProgress() {
    if (apply_dialog_ == nullptr) {
        return;
    }
    if (applying_file_paths_ && file_apply_progress_state_) {
        updateFilePublicationApplyDialog(*apply_dialog_, *file_apply_progress_state_);
    } else if (apply_progress_state_) {
        updateMetadataApplyDialog(*apply_dialog_, *apply_progress_state_);
    }
}

void MetadataPropertiesDialog::finishMetadataApply() {
    apply_running_ = false;
    apply_progress_timer_->stop();
    updateApplyProgress();
    const auto result = metadata_apply_watcher_.result();
    if (result && *result) {
        apply_committed_ = (**result).committed_source_count() > 0U;
        if (apply_observer_) {
            apply_observer_(**result);
        }
    }
    if (apply_dialog_ != nullptr) {
        if (result) {
            finishMetadataApplyDialog(*apply_dialog_, *result);
        } else {
            finishMetadataApplyDialog(*apply_dialog_, std::unexpected(core::Error{
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
    }
    ++write_plan_generation_;
    apply_progress_state_.reset();
    updateWritePlanButton();
}

void MetadataPropertiesDialog::finishFileApply() {
    apply_running_ = false;
    apply_progress_timer_->stop();
    updateApplyProgress();
    const auto result = file_apply_watcher_.result();
    if (result && *result) {
        apply_committed_ = (**result).committed_source_count() > 0U;
        if (file_apply_observer_) {
            file_apply_observer_(**result);
        }
    }
    if (apply_dialog_ != nullptr) {
        if (result) {
            finishFilePublicationApplyDialog(*apply_dialog_, *result);
        } else {
            finishFilePublicationApplyDialog(*apply_dialog_, std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "The file-publication Apply task returned no result",
                .context = {},
            }));
        }
    }
    if (!result || !*result) {
        const auto message = result ? display_utf8(result->error().message)
                                    : QStringLiteral("The Apply task returned no result");
        read_only_->setText(QStringLiteral("File publication failed · %1").arg(message));
    } else if (apply_committed_) {
        read_only_->setText(
            QStringLiteral("Published %1 physical %2 and reconciled every occurrence · close "
                           "results to refresh Properties")
                .arg((**result).committed_source_count())
                .arg((**result).committed_source_count() == 1U ? QStringLiteral("source")
                                                               : QStringLiteral("sources")));
    } else {
        read_only_->setText(
            QStringLiteral("No path changes were committed · preview again before retrying"));
    }
    ++write_plan_generation_;
    file_apply_progress_state_.reset();
    applying_file_paths_ = false;
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

    auto* editor = createMetadataExactValueDialog(heading, context, current_values, this);
    exact_values_dialog_ = editor;
    edit_values_button_->setEnabled(false);
    updateTransformationButton();
    const QPersistentModelIndex target{value_index};
    connect(editor, &QDialog::accepted, this, [this, editor, target] {
        if (!target.isValid()) {
            return;
        }
        static_cast<void>(aggregate_model_->replaceRowValues(
            target.row(), metadataExactValueDialogValues(editor)));
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
    auto* dialog = createMetadataTransformationDialog(
        grid_model_->sharedSelection(), grid_model_->patches(), std::move(items), std::move(labels),
        [this](const metadata::MetadataTransformationPreview& preview) {
            if (grid_model_ == nullptr || !grid_model_->stageTransformation(preview)) {
                return false;
            }
            loaded_field_count_ = grid_model_->selection().field_count();
            updateSelectionProjection();
            return true;
        },
        transformation_store_, this, initially_selected, preview_initially_selected, layout_store_);
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
        persistLayoutState();
        event->accept();
        return;
    }
    if (draft_count_ == 0 || grid_model_ == nullptr) {
        write_plan_cancellation_.request_cancellation();
        persistLayoutState();
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
        persistLayoutState();
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace trackknife::bench
