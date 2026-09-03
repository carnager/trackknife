// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/operations/output_path_plan.hpp"

#include <QDialog>
#include <QFutureWatcher>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace trackknife::bench {

struct ConvertDialogItem {
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::optional<core::LocalSourceRevision> source_revision;
    metadata::MetadataDocument metadata;
    QString label;
};

// Converts the current selection below a destination root (ADR-0107):
// preset choice with probed availability, a tkfmt-1 naming layout with a
// live target preview, and a direct bounded parallel conversion with
// problems-only feedback — no review step between preview and output.
class ConvertDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit ConvertDialog(std::vector<ConvertDialogItem> items, QWidget* parent = nullptr);
    ~ConvertDialog() override;

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void refreshPreview();
    void startConversion();
    void finishConversion();
    [[nodiscard]] std::optional<convert::EncoderPreset> selectedPreset() const;

    std::vector<ConvertDialogItem> items_;
    QComboBox* preset_{nullptr};
    QLineEdit* destination_{nullptr};
    QLineEdit* directory_expression_{nullptr};
    QLineEdit* basename_expression_{nullptr};
    QSpinBox* parallelism_{nullptr};
    QListWidget* preview_{nullptr};
    QLabel* status_{nullptr};
    QProgressBar* progress_{nullptr};
    QPushButton* run_{nullptr};
    QPushButton* stop_{nullptr};
    QPushButton* close_{nullptr};

    std::optional<operations::OutputPathPlan> plan_;
    bool running_{false};
    core::CancellationSource cancellation_;
    std::shared_ptr<std::atomic_size_t> completed_;
    std::size_t running_total_{0U};
    QFutureWatcher<std::shared_ptr<core::Result<convert::ConversionScanResult>>> watcher_;
};

} // namespace trackknife::bench
