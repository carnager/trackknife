// SPDX-License-Identifier: GPL-3.0-only

#include "bench/preparation_plan_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

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

class OutputPathReviewModel final : public QAbstractTableModel {
  public:
    explicit OutputPathReviewModel(std::shared_ptr<const operations::PreparationPlan> plan,
                                   QObject* parent = nullptr)
        : QAbstractTableModel(parent), plan_(std::move(plan)) {}

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() || !plan_->output_paths
                   ? 0
                   : static_cast<int>(
                         std::min(plan_->output_paths->sources.size(),
                                  static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }

    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 6;
    }

    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= columnCount()) {
            return {};
        }
        const auto source_index = static_cast<std::size_t>(index.row());
        const auto& source = plan_->output_paths->sources[source_index];
        const operations::OutputPathPreflightSource* preflight = nullptr;
        if (plan_->path_preflight && source_index < plan_->path_preflight->sources.size()) {
            preflight = &plan_->path_preflight->sources[source_index];
        }
        const auto plan_issue_applies = [&source](const auto& issue) {
            return (issue.source_raw_path && *issue.source_raw_path == source.source_raw_path) ||
                   std::ranges::any_of(issue.item_indexes, [&source](const auto item_index) {
                       return std::ranges::find(source.item_indexes, item_index) !=
                              source.item_indexes.end();
                   });
        };
        const auto source_blocked = std::ranges::any_of(
            plan_->output_paths->issues, [&plan_issue_applies](const auto& issue) {
                return issue.blocking && plan_issue_applies(issue);
            });
        const auto preflight_blocked =
            plan_->path_preflight &&
            std::ranges::any_of(plan_->path_preflight->issues, [&source](const auto& issue) {
                return issue.blocking && issue.source_raw_path == source.source_raw_path;
            });
        if (role == Qt::DisplayRole) {
            switch (index.column()) {
            case 0:
                return source_blocked || preflight_blocked
                           ? QStringLiteral("Blocked")
                           : (preflight != nullptr ? QStringLiteral("Ready")
                                                   : QStringLiteral("Planning"));
            case 1:
                return QString::fromStdString(core::escape_raw_path(source.source_raw_path));
            case 2:
                return QString::fromStdString(core::escape_raw_path(source.target_raw_path));
            case 3:
                return source.raw_relative_directory.empty()
                           ? QStringLiteral("—")
                           : display_utf8(source.raw_relative_directory);
            case 4:
                return source.raw_basename.empty() ? QStringLiteral("—")
                                                   : display_utf8(source.raw_basename);
            case 5:
                return preflight != nullptr ? publication_kind_text(preflight->publication)
                                            : QStringLiteral("Preflight unavailable");
            default:
                return {};
            }
        }
        if (role == Qt::ToolTipRole) {
            auto details =
                QStringLiteral("Source: %1\nTarget: %2")
                    .arg(QString::fromStdString(core::escape_raw_path(source.source_raw_path)),
                         QString::fromStdString(core::escape_raw_path(source.target_raw_path)));
            if (source.sanitized) {
                details += QStringLiteral("\nGenerated path text was visibly sanitized with "
                                          "linux-v1.");
            }
            for (const auto& issue : plan_->output_paths->issues) {
                if (plan_issue_applies(issue)) {
                    details += QStringLiteral("\n%1: %2")
                                   .arg(display_utf8(operations::output_path_plan_issue_kind_name(
                                            issue.kind)),
                                        display_utf8(issue.message));
                }
            }
            if (plan_->path_preflight) {
                for (const auto& issue : plan_->path_preflight->issues) {
                    if (issue.source_raw_path == source.source_raw_path) {
                        details +=
                            QStringLiteral("\n%1: %2")
                                .arg(display_utf8(operations::output_path_preflight_issue_kind_name(
                                         issue.kind)),
                                     display_utf8(issue.message));
                    }
                }
            }
            return details;
        }
        if (role == Qt::ForegroundRole && (source_blocked || preflight_blocked)) {
            return QApplication::palette().brush(QPalette::PlaceholderText);
        }
        if (role == Qt::TextAlignmentRole && index.column() == 0) {
            return Qt::AlignCenter;
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
        constexpr std::array headers{"Status",      "Source",       "Target",
                                     "Raw folders", "Raw filename", "Publication"};
        return section >= 0 && section < static_cast<int>(headers.size())
                   ? QVariant{QString::fromLatin1(headers[static_cast<std::size_t>(section)])}
                   : QVariant{};
    }

  private:
    std::shared_ptr<const operations::PreparationPlan> plan_;
};

class PreparationPlanDialog final : public QDialog {
  public:
    using ApplyCallback = std::function<void(std::shared_ptr<const operations::PreparationPlan>)>;

    explicit PreparationPlanDialog(std::shared_ptr<const operations::PreparationPlan> plan,
                                   ApplyCallback apply, QWidget* parent)
        : QDialog(parent) {
        setObjectName(QStringLiteral("bench-metadata-write-plan"));
        setWindowTitle(QStringLiteral("Preparation review"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(1'100, 520);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);
        const auto path_count = plan->output_paths ? plan->output_paths->sources.size() : 0U;
        const auto source_count =
            path_count > 0U ? path_count : (plan->metadata ? plan->metadata->sources.size() : 0U);
        const auto ready_count = !plan->has_path_operation() && plan->metadata
                                     ? plan->metadata->ready_source_count()
                                     : (plan->ready() ? source_count : 0U);
        auto* summary = new QLabel(
            QStringLiteral("%1 staged %2 · %3 physical %4 · %5 ready · %6 blocking %7")
                .arg(plan->metadata_context_change_count)
                .arg(plan->metadata_context_change_count == 1U ? QStringLiteral("change")
                                                               : QStringLiteral("changes"))
                .arg(source_count)
                .arg(source_count == 1U ? QStringLiteral("source") : QStringLiteral("sources"))
                .arg(ready_count)
                .arg(plan->blocking_issue_count())
                .arg(plan->blocking_issue_count() == 1U ? QStringLiteral("issue")
                                                        : QStringLiteral("issues")),
            this);
        summary->setObjectName(QStringLiteral("bench-metadata-write-plan-summary"));
        layout->addWidget(summary);

        auto* explanation = new QLabel(
            plan->ready() ? QStringLiteral("Everything below was just revalidated against the "
                                           "files on disk. Apply performs exactly these changes.")
                          : QStringLiteral("Apply is blocked. Hover rows for exact path, revision, "
                                           "adapter, preservation, and filesystem details."),
            this);
        explanation->setObjectName(QStringLiteral("bench-metadata-write-plan-explanation"));
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        auto configure_table = [](QTableView* table) {
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
        };
        auto* effects = new QTabWidget(this);
        effects->setObjectName(QStringLiteral("bench-preparation-plan-effects"));
        if (plan->metadata) {
            auto metadata_plan =
                std::shared_ptr<const metadata::MetadataWritePlan>(plan, &*plan->metadata);
            auto* table = new QTableView(effects);
            table->setObjectName(QStringLiteral("bench-metadata-write-plan-table"));
            table->setAccessibleName(QStringLiteral("Revalidated metadata changes"));
            table->setModel(new MetadataWritePlanModel(metadata_plan, table));
            configure_table(table);
            table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
            table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
            table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
            table->setColumnWidth(0, 100);
            table->setColumnWidth(2, 150);
            table->setColumnWidth(5, 190);
            effects->addTab(table, QStringLiteral("Tag writes"));
        }
        if (plan->output_paths) {
            auto* table = new QTableView(effects);
            table->setObjectName(QStringLiteral("bench-output-path-plan-table"));
            table->setAccessibleName(QStringLiteral("Revalidated file path changes"));
            table->setModel(new OutputPathReviewModel(plan, table));
            configure_table(table);
            table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
            table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
            table->setColumnWidth(0, 90);
            table->setColumnWidth(3, 150);
            table->setColumnWidth(4, 150);
            table->setColumnWidth(5, 210);
            effects->addTab(table, QStringLiteral("File paths"));
        }
        layout->addWidget(effects, 1);

        if (!plan->issues.empty()) {
            QStringList messages;
            for (const auto& issue : plan->issues) {
                messages.push_back(display_utf8(issue.message));
            }
            auto* issues = new QLabel(messages.join(QChar{'\n'}), this);
            issues->setObjectName(QStringLiteral("bench-preparation-plan-issues"));
            issues->setWordWrap(true);
            layout->addWidget(issues);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->setObjectName(QStringLiteral("bench-metadata-write-plan-buttons"));
        if (plan->ready() && apply) {
            auto* apply_button = buttons->addButton(QDialogButtonBox::Apply);
            apply_button->setObjectName(QStringLiteral("bench-metadata-write-plan-apply"));
            apply_button->setToolTip(QStringLiteral("Apply exactly the reviewed changes"));
            connect(apply_button, &QPushButton::clicked, this,
                    [plan = std::move(plan), apply = std::move(apply)] { apply(plan); });
        }
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        layout->addWidget(buttons);
    }
};

} // namespace

QDialog* createPreparationPlanDialog(std::shared_ptr<const operations::PreparationPlan> plan,
                                     PreparationPlanApplyCallback apply, QWidget* parent) {
    return new PreparationPlanDialog(std::move(plan), std::move(apply), parent);
}

} // namespace trackknife::bench
