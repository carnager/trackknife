// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/write_plan.hpp"
#include "trackknife/operations/file_publication_apply.hpp"
#include "trackknife/operations/metadata_apply.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QDialog>
#include <QFutureWatcher>
#include <QPointer>
#include <QStringList>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QModelIndex;
class QCloseEvent;
class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QInputDialog;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableView;
class QTimer;
class QVBoxLayout;

namespace trackknife::bench {

class MetadataGridModel;
class MetadataAggregateModel;

struct MetadataPropertiesSource {
    metadata::StagedMetadataSource source;
    QString track_label;
};

using MetadataPropertiesSourceReader =
    std::function<std::optional<MetadataPropertiesSource>(std::size_t)>;
using MetadataWritePlanApplier = std::function<core::Result<operations::MetadataApplyResult>(
    const metadata::MetadataWritePlan&, const operations::MetadataApplyProgressCallback&,
    const core::CancellationToken&)>;
using MetadataWritePlanApplierFactory = std::function<MetadataWritePlanApplier()>;
using MetadataApplyObserver = std::function<void(const operations::MetadataApplyResult&)>;
using FilePublicationPlanApplier =
    std::function<core::Result<operations::FilePublicationApplyResult>(
        const operations::OutputPathPreflight&,
        const operations::FilePublicationApplyProgressCallback&, const core::CancellationToken&)>;
using FilePublicationPlanApplierFactory = std::function<FilePublicationPlanApplier()>;
using FilePublicationApplyObserver =
    std::function<void(const operations::FilePublicationApplyResult&)>;

struct MetadataTransformationStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedMetadataTransformationChain>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedMetadataTransformationChain, Completion)> save;
    std::function<void(core::StableId, Completion)> remove;
};

struct OutputProfileStore {
    using LoadCompletion =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using Completion = std::function<void(QString)>;

    std::function<void(LoadCompletion)> load;
    std::function<void(persistence::SavedOutputLayoutProfile, Completion)> save_layout;
    std::function<void(core::StableId, Completion)> remove_layout;
    std::function<void(persistence::SavedDestinationProfile, Completion)> save_destination;
    std::function<void(core::StableId, Completion)> remove_destination;
};

struct MetadataApplyProgressState;
struct FilePublicationApplyProgressState;

class MetadataPropertiesDialog final : public QDialog {
    Q_OBJECT

  public:
    MetadataPropertiesDialog(std::size_t requested_item_count,
                             MetadataPropertiesSourceReader source_reader,
                             std::span<const std::string_view> preferred_fields,
                             MetadataWritePlanApplierFactory plan_applier_factory,
                             MetadataApplyObserver apply_observer,
                             MetadataTransformationStore transformation_store = {},
                             OutputProfileStore output_profile_store = {},
                             FilePublicationPlanApplierFactory file_plan_applier_factory = {},
                             FilePublicationApplyObserver file_apply_observer = {},
                             QWidget* parent = nullptr);
    ~MetadataPropertiesDialog() override;

  private:
    using SelectionResult = core::Result<metadata::StagedMetadataSelection>;
    using WritePlanResult = core::Result<operations::PreparationPlan>;

    void captureSources();
    void startSelection();
    void finishSelection();
    void buildGrid(metadata::StagedMetadataSelection selection);
    void scheduleSelectionProjection();
    void updateSelectionProjection();
    void updateDraftState(int patch_count, bool can_undo, bool can_redo);
    void updateFieldButtons();
    void updateEditValuesButton();
    void updateTransformationButton();
    void loadTransformationCatalog(std::optional<core::StableId> selected = std::nullopt);
    void
    rebuildTransformationCatalogControls(std::optional<core::StableId> selected = std::nullopt);
    void toggleAutomaticTransformation(core::StableId id, bool enabled);
    void loadOutputProfiles();
    void
    rebuildOutputProfileControls(std::optional<core::StableId> selected_layout = std::nullopt,
                                 std::optional<core::StableId> selected_destination = std::nullopt);
    void selectOutputLayout(int index);
    void selectDestination(int index);
    void newOutputLayout();
    void newDestination();
    void saveOutputLayout();
    void saveDestination();
    void removeOutputLayout();
    void removeDestination();
    void updateOutputProfileButtons();
    void updateWritePlanButton();
    void invalidateWritePlan();
    void previewWritePlan();
    void finishWritePlan();
    void startApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startMetadataApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void startFileApply(std::shared_ptr<const operations::PreparationPlan> plan);
    void updateApplyProgress();
    void finishMetadataApply();
    void finishFileApply();
    [[nodiscard]] QStringList metadataFieldNameSuggestions(const QString& query) const;
    [[nodiscard]] std::vector<std::size_t> selectedItemIndexes() const;
    void promptAddField();
    void removeSelectedFields();
    void editCurrentValues();
    void promptTransformation(std::optional<core::StableId> initially_selected = std::nullopt,
                              bool preview_initially_selected = false);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

    QFutureWatcher<std::shared_ptr<SelectionResult>> selection_watcher_;
    QFutureWatcher<std::shared_ptr<WritePlanResult>> write_plan_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::MetadataApplyResult>>>
        metadata_apply_watcher_;
    QFutureWatcher<std::shared_ptr<core::Result<operations::FilePublicationApplyResult>>>
        file_apply_watcher_;
    MetadataPropertiesSourceReader source_reader_;
    MetadataWritePlanApplierFactory plan_applier_factory_;
    MetadataApplyObserver apply_observer_;
    MetadataTransformationStore transformation_store_;
    OutputProfileStore output_profile_store_;
    FilePublicationPlanApplierFactory file_plan_applier_factory_;
    FilePublicationApplyObserver file_apply_observer_;
    std::vector<persistence::SavedMetadataTransformationChain> transformation_catalog_;
    std::vector<persistence::SavedOutputLayoutProfile> output_layout_catalog_;
    std::vector<persistence::SavedDestinationProfile> destination_catalog_;
    std::vector<metadata::StagedMetadataSource> sources_;
    std::vector<std::string> preferred_fields_;
    std::vector<std::string> recent_field_names_;
    QStringList track_labels_;
    std::size_t requested_item_count_{0U};
    std::size_t capture_index_{0U};
    QVBoxLayout* root_layout_{nullptr};
    QLabel* summary_{nullptr};
    QLabel* read_only_{nullptr};
    QLabel* loading_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* undo_button_{nullptr};
    QPushButton* redo_button_{nullptr};
    QPushButton* discard_button_{nullptr};
    QPushButton* add_field_button_{nullptr};
    QPushButton* remove_field_button_{nullptr};
    QPushButton* edit_values_button_{nullptr};
    QPushButton* transform_button_{nullptr};
    QWidget* transformation_panel_{nullptr};
    QWidget* grid_tools_{nullptr};
    QListWidget* transformation_list_{nullptr};
    QLabel* transformation_status_{nullptr};
    QCheckBox* save_tags_check_{nullptr};
    QCheckBox* rename_files_check_{nullptr};
    QCheckBox* move_files_check_{nullptr};
    QCheckBox* replaygain_check_{nullptr};
    QComboBox* output_layout_combo_{nullptr};
    QLineEdit* output_layout_name_{nullptr};
    QLineEdit* output_directory_expression_{nullptr};
    QLineEdit* output_basename_expression_{nullptr};
    QPushButton* output_layout_new_button_{nullptr};
    QPushButton* output_layout_save_button_{nullptr};
    QPushButton* output_layout_remove_button_{nullptr};
    QComboBox* destination_combo_{nullptr};
    QLineEdit* destination_name_{nullptr};
    QLineEdit* destination_root_{nullptr};
    QPushButton* destination_browse_button_{nullptr};
    QPushButton* destination_new_button_{nullptr};
    QPushButton* destination_save_button_{nullptr};
    QPushButton* destination_remove_button_{nullptr};
    QLabel* output_profile_status_{nullptr};
    QPushButton* preview_plan_button_{nullptr};
    MetadataGridModel* grid_model_{nullptr};
    MetadataAggregateModel* aggregate_model_{nullptr};
    QTableView* fields_{nullptr};
    QTableView* file_list_{nullptr};
    QTimer* selection_debounce_{nullptr};
    QTimer* apply_progress_timer_{nullptr};
    QPointer<QDialog> exact_values_dialog_;
    QPointer<QInputDialog> field_name_dialog_;
    QPointer<QDialog> transformation_dialog_;
    QPointer<QDialog> write_plan_dialog_;
    QPointer<QDialog> apply_dialog_;
    std::shared_ptr<MetadataApplyProgressState> apply_progress_state_;
    std::shared_ptr<FilePublicationApplyProgressState> file_apply_progress_state_;
    QString selection_summary_;
    QString revision_summary_;
    std::size_t loaded_item_count_{0U};
    std::size_t loaded_source_count_{0U};
    std::size_t loaded_field_count_{0U};
    std::size_t selected_item_count_{0U};
    std::size_t write_plan_generation_{0U};
    std::size_t write_plan_job_generation_{0U};
    core::CancellationSource write_plan_cancellation_;
    core::CancellationSource apply_cancellation_;
    int draft_count_{0};
    bool transformation_catalog_loading_{false};
    bool output_profiles_loading_{false};
    bool output_profile_mutation_running_{false};
    std::optional<core::StableId> editing_output_layout_id_;
    std::optional<core::StableId> editing_destination_id_;
    std::string destination_root_raw_path_;
    bool write_plan_running_{false};
    bool apply_running_{false};
    bool applying_file_paths_{false};
    bool apply_committed_{false};
};

} // namespace trackknife::bench
