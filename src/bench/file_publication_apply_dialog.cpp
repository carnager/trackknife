// SPDX-License-Identifier: GPL-3.0-only

#include "bench/file_publication_apply_dialog.hpp"

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

class FilePublicationApplySourceModel final : public QAbstractTableModel {
  public:
    explicit FilePublicationApplySourceModel(const operations::OutputPathPreflight& preflight,
                                             QObject* parent = nullptr)
        : QAbstractTableModel(parent) {
        rows_.reserve(preflight.sources.size());
        for (const auto& source : preflight.sources) {
            rows_.push_back(Row{
                .state = operations::FilePublicationApplySourceState::pending,
                .source =
                    QString::fromStdString(core::escape_raw_path(source.planned.source_raw_path)),
                .target =
                    QString::fromStdString(core::escape_raw_path(source.planned.target_raw_path)),
                .detail = QStringLiteral("Waiting for a mutation worker"),
            });
        }
    }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }
    [[nodiscard]] QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("%1\n%2\n%3").arg(row.source, row.target, row.detail);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (index.column()) {
        case 0:
            return file_apply_state_text(row.state);
        case 1:
            return row.source;
        case 2:
            return row.target;
        case 3:
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
        static constexpr std::array labels{"Status", "Source", "Target", "Details"};
        return section >= 0 && section < static_cast<int>(labels.size())
                   ? QString::fromLatin1(labels[static_cast<std::size_t>(section)])
                   : QVariant{};
    }

    void update(const std::vector<operations::FilePublicationApplySourceState>& states,
                const std::vector<std::optional<core::Error>>& issues) {
        const auto count = std::min({rows_.size(), states.size(), issues.size()});
        for (std::size_t index = 0U; index < count; ++index) {
            rows_[index].state = states[index];
            rows_[index].detail =
                issues[index]
                    ? display_utf8(issues[index]->message)
                    : (states[index] == operations::FilePublicationApplySourceState::running
                           ? QStringLiteral("Revalidating and publishing safely")
                       : states[index] == operations::FilePublicationApplySourceState::committed
                           ? QStringLiteral("Published and reconciled every occurrence")
                       : states[index] == operations::FilePublicationApplySourceState::unchanged
                           ? QStringLiteral("Source already has the reviewed path")
                       : states[index] == operations::FilePublicationApplySourceState::pending
                           ? QStringLiteral("Waiting for a mutation worker")
                           : QString{});
        }
        if (count > 0U) {
            emit dataChanged(index(0, 0), index(static_cast<int>(count - 1U), columnCount() - 1));
        }
    }

  private:
    struct Row {
        operations::FilePublicationApplySourceState state{
            operations::FilePublicationApplySourceState::pending};
        QString source;
        QString target;
        QString detail;
    };
    std::vector<Row> rows_;
};

class FilePublicationApplyDialog final : public QDialog {
  public:
    using CancelCallback = std::function<void()>;

    explicit FilePublicationApplyDialog(const operations::OutputPathPreflight& preflight,
                                        CancelCallback cancel, QWidget* parent)
        : QDialog(parent), cancel_(std::move(cancel)) {
        setObjectName(QStringLiteral("bench-file-publication-apply"));
        setWindowTitle(QStringLiteral("Apply file path changes"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        resize(980, 420);
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(QStringLiteral("Publishing 0 of %1 physical %2…")
                                  .arg(preflight.sources.size())
                                  .arg(preflight.sources.size() == 1U ? QStringLiteral("source")
                                                                      : QStringLiteral("sources")),
                              this);
        summary_->setObjectName(QStringLiteral("bench-file-publication-apply-summary"));
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        progress_ = new QProgressBar(this);
        progress_->setObjectName(QStringLiteral("bench-file-publication-apply-progress"));
        progress_->setRange(0, static_cast<int>(preflight.sources.size()));
        layout->addWidget(progress_);
        auto* table = new QTableView(this);
        table->setObjectName(QStringLiteral("bench-file-publication-apply-table"));
        model_ = new FilePublicationApplySourceModel(preflight, table);
        table->setModel(model_);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setWordWrap(false);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        buttons_ = new QDialogButtonBox(this);
        buttons_->setObjectName(QStringLiteral("bench-file-publication-apply-buttons"));
        cancel_button_ =
            buttons_->addButton(QStringLiteral("Cancel"), QDialogButtonBox::RejectRole);
        cancel_button_->setObjectName(QStringLiteral("bench-file-publication-apply-cancel"));
        connect(cancel_button_, &QPushButton::clicked, this, [this] { requestCancellation(); });
        layout->addWidget(buttons_);
    }

    void update(const FilePublicationApplyProgressState& progress) {
        std::vector<operations::FilePublicationApplySourceState> states;
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
            QStringLiteral("Publishing %1 of %2 physical %3%4")
                .arg(completed)
                .arg(states.size())
                .arg(states.size() == 1U ? QStringLiteral("source") : QStringLiteral("sources"))
                .arg(cancellation_requested_ ? QStringLiteral(" · cancelling…")
                                             : QStringLiteral("…")));
    }

    void finish(const core::Result<operations::FilePublicationApplyResult>& result) {
        running_ = false;
        cancel_button_->setVisible(false);
        auto* close = buttons_->addButton(QDialogButtonBox::Close);
        close->setObjectName(QStringLiteral("bench-file-publication-apply-close"));
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        if (!result) {
            summary_->setText(QStringLiteral("File publication could not start · %1")
                                  .arg(display_utf8(result.error().message)));
            return;
        }
        std::vector<operations::FilePublicationApplySourceState> states;
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
            QStringLiteral("%1 published · %2 unchanged · %3 failed · %4 cancelled%5")
                .arg(result->committed_source_count())
                .arg(result->unchanged_source_count())
                .arg(result->failed_source_count())
                .arg(result->cancelled_source_count())
                .arg(result->committed_source_count() + result->unchanged_source_count() ==
                             result->sources.size()
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
    FilePublicationApplySourceModel* model_{nullptr};
    QLabel* summary_{nullptr};
    QProgressBar* progress_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
    QPushButton* cancel_button_{nullptr};
    bool running_{true};
    bool cancellation_requested_{false};
};

} // namespace

QDialog* createFilePublicationApplyDialog(const operations::OutputPathPreflight& preflight,
                                          FilePublicationApplyCancelCallback cancel,
                                          QWidget* parent) {
    return new FilePublicationApplyDialog(preflight, std::move(cancel), parent);
}

void updateFilePublicationApplyDialog(QDialog& dialog,
                                      const FilePublicationApplyProgressState& progress) {
    static_cast<FilePublicationApplyDialog&>(dialog).update(progress);
}

void finishFilePublicationApplyDialog(
    QDialog& dialog, const core::Result<operations::FilePublicationApplyResult>& result) {
    static_cast<FilePublicationApplyDialog&>(dialog).finish(result);
}

} // namespace trackknife::bench
