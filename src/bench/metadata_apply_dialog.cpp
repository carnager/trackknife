// SPDX-License-Identifier: GPL-3.0-only

#include "bench/metadata_apply_dialog.hpp"

#include "bench/metadata_dialog_helpers.hpp"
#include "trackknife/core/local_sources.hpp"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

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
                    .detail = QStringLiteral("Waiting")});
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
        static constexpr std::array labels{"Status", "File", "Details"};
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
                                     ? QStringLiteral("Checking the file and saving tags")
                                 : states[index] == operations::MetadataApplySourceState::committed
                                     ? QStringLiteral(
                                           "Tags saved and every matching list entry updated")
                                 : states[index] == operations::MetadataApplySourceState::pending
                                     ? QStringLiteral("Waiting")
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
        setWindowTitle(QStringLiteral("Save tag changes"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(900, 420);
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(QStringLiteral("Saving 0 of %1 %2…")
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
            QStringLiteral("Saving %1 of %2 %3%4")
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
            summary_->setText(QStringLiteral("Could not save tag changes · %1")
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
            QStringLiteral("%1 saved · %2 failed · %3 cancelled%4")
                .arg(result->committed_source_count())
                .arg(result->failed_source_count())
                .arg(result->cancelled_source_count())
                .arg(result->committed_source_count() == result->sources.size()
                         ? QStringLiteral(" · complete")
                         : QStringLiteral(" · close and review the changes again before retrying")));
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
        summary_->setText(
            QStringLiteral("Cancelling after the files already in progress are safe…"));
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

} // namespace

QDialog* createMetadataApplyDialog(const metadata::MetadataWritePlan& plan,
                                   MetadataApplyCancelCallback cancel, QWidget* parent) {
    return new MetadataApplyDialog(plan, std::move(cancel), parent);
}

void updateMetadataApplyDialog(QDialog& dialog, const MetadataApplyProgressState& progress) {
    static_cast<MetadataApplyDialog&>(dialog).update(progress);
}

void finishMetadataApplyDialog(QDialog& dialog,
                               const core::Result<operations::MetadataApplyResult>& result) {
    static_cast<MetadataApplyDialog&>(dialog).finish(result);
}

} // namespace trackknife::bench
