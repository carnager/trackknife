// SPDX-License-Identifier: GPL-3.0-only

#include "convert_dialog.hpp"

#include "bench_main_window_helpers.hpp"
#include "trackknife/convert/preset.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <filesystem>
#include <system_error>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr int preview_limit = 200;

[[nodiscard]] QString settingsText(const QSettings& settings, const char* key,
                                   const QString& fallback) {
    const auto value = settings.value(QLatin1String(key)).toString();
    return value.isEmpty() ? fallback : value;
}

} // namespace

ConvertDialog::ConvertDialog(std::vector<ConvertDialogItem> items, QWidget* parent)
    : QDialog(parent), items_(std::move(items)) {
    setWindowTitle(QStringLiteral("Convert %1 file%2")
                       .arg(items_.size())
                       .arg(items_.size() == 1U ? QString{} : QStringLiteral("s")));
    setObjectName(QStringLiteral("bench-convert-dialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 520);

    const QSettings settings;
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    preset_ = new QComboBox(this);
    preset_->setObjectName(QStringLiteral("bench-convert-preset"));
    const auto saved_preset = settings.value(QStringLiteral("convert/preset")).toString();
    for (const auto& preset : convert::builtin_encoder_presets()) {
        preset_->addItem(displayText(preset.display_name), displayText(preset.id));
        const auto availability = convert::probe_encoder_preset(preset);
        if (!availability.available) {
            const auto row = preset_->count() - 1;
            auto* model = qobject_cast<QStandardItemModel*>(preset_->model());
            if (model != nullptr && model->item(row) != nullptr) {
                model->item(row)->setEnabled(false);
                model->item(row)->setToolTip(displayText(availability.detail));
            }
        } else if (displayText(preset.id) == saved_preset) {
            preset_->setCurrentIndex(preset_->count() - 1);
        }
    }
    form->addRow(QStringLiteral("Preset:"), preset_);

    auto* destination_row = new QHBoxLayout;
    destination_ = new QLineEdit(this);
    destination_->setObjectName(QStringLiteral("bench-convert-destination"));
    destination_->setText(settings.value(QStringLiteral("convert/destination-root")).toString());
    destination_->setPlaceholderText(QStringLiteral("Destination folder"));
    auto* browse = new QPushButton(QStringLiteral("Browse…"), this);
    browse->setObjectName(QStringLiteral("bench-convert-browse"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose the conversion destination"), destination_->text());
        if (!chosen.isEmpty()) {
            destination_->setText(chosen);
        }
    });
    destination_row->addWidget(destination_, 1);
    destination_row->addWidget(browse);
    form->addRow(QStringLiteral("Into:"), destination_row);

    directory_expression_ = new QLineEdit(this);
    directory_expression_->setObjectName(QStringLiteral("bench-convert-directory-expression"));
    directory_expression_->setText(settingsText(settings, "convert/directory-expression",
                                                QStringLiteral("%albumartist%/%album%")));
    form->addRow(QStringLiteral("Folders:"), directory_expression_);

    basename_expression_ = new QLineEdit(this);
    basename_expression_->setObjectName(QStringLiteral("bench-convert-basename-expression"));
    basename_expression_->setText(settingsText(settings, "convert/basename-expression",
                                               QStringLiteral("%tracknumber% - %title%")));
    form->addRow(QStringLiteral("Names:"), basename_expression_);

    parallelism_ = new QSpinBox(this);
    parallelism_->setObjectName(QStringLiteral("bench-convert-parallelism"));
    parallelism_->setRange(1, static_cast<int>(convert::maximum_conversion_parallelism));
    parallelism_->setValue(settings.value(QStringLiteral("convert/parallelism"), 4).toInt());
    form->addRow(QStringLiteral("Parallel files:"), parallelism_);
    layout->addLayout(form);

    preview_ = new QListWidget(this);
    preview_->setObjectName(QStringLiteral("bench-convert-preview"));
    preview_->setSelectionMode(QAbstractItemView::NoSelection);
    preview_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(preview_, 1);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("bench-convert-progress"));
    progress_->setVisible(false);
    layout->addWidget(progress_);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("bench-convert-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* buttons = new QHBoxLayout;
    run_ = new QPushButton(QStringLiteral("Convert"), this);
    run_->setObjectName(QStringLiteral("bench-convert-run"));
    run_->setDefault(true);
    connect(run_, &QPushButton::clicked, this, &ConvertDialog::startConversion);
    stop_ = new QPushButton(QStringLiteral("Stop"), this);
    stop_->setObjectName(QStringLiteral("bench-convert-stop"));
    stop_->setVisible(false);
    connect(stop_, &QPushButton::clicked, this, [this] { cancellation_.request_cancellation(); });
    close_ = new QPushButton(QStringLiteral("Close"), this);
    close_->setObjectName(QStringLiteral("bench-convert-close"));
    connect(close_, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch(1);
    buttons->addWidget(run_);
    buttons->addWidget(stop_);
    buttons->addWidget(close_);
    layout->addLayout(buttons);

    connect(&watcher_, &QFutureWatcherBase::finished, this, &ConvertDialog::finishConversion);
    // The preview recomputes on every edit; planning is pure and fast, so a
    // short debounce only coalesces bursts of keystrokes.
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(150);
    connect(debounce, &QTimer::timeout, this, &ConvertDialog::refreshPreview);
    const auto schedule = [debounce] { debounce->start(); };
    connect(destination_, &QLineEdit::textChanged, this, schedule);
    connect(directory_expression_, &QLineEdit::textChanged, this, schedule);
    connect(basename_expression_, &QLineEdit::textChanged, this, schedule);
    connect(preset_, &QComboBox::currentIndexChanged, this, schedule);

    refreshPreview();
}

ConvertDialog::~ConvertDialog() {
    cancellation_.request_cancellation();
    watcher_.waitForFinished();
}

void ConvertDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        cancellation_.request_cancellation();
        status_->setText(QStringLiteral("Stopping after the files already in flight…"));
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

std::optional<convert::EncoderPreset> ConvertDialog::selectedPreset() const {
    return convert::find_encoder_preset(preset_->currentData().toString().toStdString());
}

void ConvertDialog::refreshPreview() {
    if (running_) {
        return;
    }
    plan_.reset();
    preview_->clear();
    run_->setEnabled(false);

    const auto preset = selectedPreset();
    if (!preset) {
        status_->setText(QStringLiteral("Choose a preset."));
        return;
    }
    if (const auto availability = convert::probe_encoder_preset(*preset); !availability.available) {
        status_->setText(displayText(availability.detail));
        return;
    }
    const auto root = destination_->text().trimmed().toStdString();
    if (root.empty()) {
        status_->setText(QStringLiteral("Choose a destination folder."));
        return;
    }
    std::error_code root_error;
    if (!std::filesystem::is_directory(std::filesystem::path{root}, root_error)) {
        status_->setText(QStringLiteral("The destination folder does not exist."));
        return;
    }

    std::vector<operations::OutputPathPlanningItem> planning_items;
    planning_items.reserve(items_.size());
    QStringList problems;
    for (std::size_t index = 0U; index < items_.size(); ++index) {
        auto& item = items_[index];
        if (!item.source_revision) {
            auto observed = core::observe_local_source_revision(item.raw_path);
            if (!observed) {
                problems.push_back(QStringLiteral("%1: %2").arg(
                    item.label, displayText(observed.error().message)));
                continue;
            }
            item.source_revision = *observed;
        }
        planning_items.push_back(operations::OutputPathPlanningItem{
            .item_index = index,
            .source_raw_path = item.raw_path,
            .source_revision = *item.source_revision,
            .final_metadata = item.metadata,
        });
    }
    if (planning_items.empty()) {
        status_->setText(problems.isEmpty() ? QStringLiteral("Nothing to convert.")
                                            : problems.join(QStringLiteral("\n")));
        return;
    }

    operations::OutputLayoutProfile layout;
    layout.name = "convert";
    layout.relative_directory_expression = directory_expression_->text().trimmed().toStdString();
    layout.basename_expression = basename_expression_->text().trimmed().toStdString();
    operations::DestinationProfile destination;
    destination.name = "convert";
    destination.root_raw_path = std::filesystem::path{root}.lexically_normal().native();
    auto planned = operations::plan_output_paths(
        planning_items, {.rename_files = true, .move_files = true}, std::move(layout),
        std::move(destination), {}, {}, {},
        operations::ConvertedPublicationPolicy{.target_extension = preset->file_extension});
    if (!planned) {
        status_->setText(displayText(planned.error().message));
        return;
    }

    for (const auto& issue : planned->issues) {
        problems.push_back(displayText(issue.message));
    }
    const auto shown =
        std::min<std::size_t>(planned->sources.size(), static_cast<std::size_t>(preview_limit));
    for (std::size_t index = 0U; index < shown; ++index) {
        const auto& source = planned->sources[index];
        auto relative = source.sanitized_relative_directory;
        if (!relative.empty()) {
            relative += '/';
        }
        relative += source.sanitized_basename + "." + preset->file_extension;
        preview_->addItem(displayText(relative));
    }
    if (planned->sources.size() > shown) {
        preview_->addItem(QStringLiteral("… and %1 more").arg(planned->sources.size() - shown));
    }

    const auto ready = planned->ready();
    if (!problems.isEmpty()) {
        status_->setText(problems.join(QStringLiteral("\n")));
    } else {
        status_->setText(QStringLiteral("%1 file%2 ready.")
                             .arg(planned->sources.size())
                             .arg(planned->sources.size() == 1U ? QString{} : QStringLiteral("s")));
    }
    plan_ = std::move(*planned);
    run_->setEnabled(ready);
}

void ConvertDialog::startConversion() {
    if (running_ || !plan_ || !plan_->ready()) {
        return;
    }
    const auto preset = selectedPreset();
    if (!preset) {
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("convert/preset"), preset_->currentData().toString());
    settings.setValue(QStringLiteral("convert/destination-root"), destination_->text());
    settings.setValue(QStringLiteral("convert/directory-expression"),
                      directory_expression_->text());
    settings.setValue(QStringLiteral("convert/basename-expression"), basename_expression_->text());
    settings.setValue(QStringLiteral("convert/parallelism"), parallelism_->value());

    std::vector<convert::ConversionScanItem> scan_items;
    scan_items.reserve(plan_->sources.size());
    for (const auto& source : plan_->sources) {
        for (const auto item_index : source.item_indexes) {
            const auto& item = items_[item_index];
            scan_items.push_back(convert::ConversionScanItem{
                .item_index = item_index,
                .source_raw_path = item.raw_path,
                .selection = item.selection,
                .range = item.segment,
                .destination_raw_path = source.target_raw_path,
                .metadata = item.metadata,
            });
        }
    }

    running_ = true;
    running_total_ = scan_items.size();
    cancellation_ = core::CancellationSource{};
    completed_ = std::make_shared<std::atomic_size_t>(0U);
    run_->setVisible(false);
    stop_->setVisible(true);
    progress_->setVisible(true);
    progress_->setRange(0, static_cast<int>(running_total_));
    progress_->setValue(0);
    status_->setText(QStringLiteral("Converting · 0 of %1 files").arg(running_total_));

    auto* poll = new QTimer(this);
    poll->setInterval(100);
    connect(poll, &QTimer::timeout, this, [this] {
        const auto done = completed_ ? completed_->load() : 0U;
        progress_->setValue(static_cast<int>(done));
        status_->setText(
            QStringLiteral("Converting · %1 of %2 files").arg(done).arg(running_total_));
    });
    connect(&watcher_, &QFutureWatcherBase::finished, poll, &QObject::deleteLater);
    poll->start();

    const auto cancellation = cancellation_.token();
    const auto parallelism = static_cast<std::size_t>(parallelism_->value());
    watcher_.setFuture(QtConcurrent::run([scan_items = std::move(scan_items), preset = *preset,
                                          parallelism, completed = completed_, cancellation] {
        // The conversion core requires existing target directories; create
        // them up front so parallel workers never race directory creation.
        for (const auto& item : scan_items) {
            std::error_code create_error;
            std::filesystem::create_directories(
                std::filesystem::path{item.destination_raw_path}.parent_path(), create_error);
        }
        auto scan = convert::scan_conversion(
            scan_items, {.preset = preset, .maximum_parallelism = parallelism},
            [completed](const convert::ConversionScanProgress& update) {
                completed->store(update.completed_items);
            },
            cancellation);
        return std::make_shared<core::Result<convert::ConversionScanResult>>(std::move(scan));
    }));
}

void ConvertDialog::finishConversion() {
    running_ = false;
    stop_->setVisible(false);
    run_->setVisible(true);
    progress_->setVisible(false);
    const auto outcome = watcher_.result();
    if (!outcome || !*outcome) {
        status_->setText(outcome ? displayText((*outcome).error().message)
                                 : QStringLiteral("Conversion failed."));
        refreshPreview();
        return;
    }
    const auto& result = **outcome;
    QStringList problems;
    for (const auto& item : result.items) {
        if (item.state == convert::ConversionScanState::failed && item.issue) {
            problems.push_back(QStringLiteral("%1: %2").arg(items_[item.item_index].label,
                                                            displayText(item.issue->message)));
        }
    }
    auto summary = QStringLiteral("Converted %1 of %2 files.")
                       .arg(result.converted_count())
                       .arg(result.items.size());
    if (result.cancellation_requested) {
        summary += QStringLiteral(" Stopped early.");
    }
    if (!problems.isEmpty()) {
        summary += QStringLiteral("\n") + problems.join(QStringLiteral("\n"));
    }
    status_->setText(summary);
}

} // namespace trackknife::bench
