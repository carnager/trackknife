// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"

#include "bench/local_library_panel.hpp"
#include "bench/metadata_properties_dialog.hpp"
#include "trackknife/audio/local_audition.hpp"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QMetaObject>
#include <QMimeData>
#include <QPointer>
#include <QResizeEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#if defined(TRACKKNIFE_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
#endif

namespace trackknife::bench {

void BenchMainWindow::stopBackgroundWork() {
    if (local_library_ != nullptr) {
        local_library_->stop();
    }
    probe_cancellation_.request_cancellation();
    metadata_operation_cancellation_.request_cancellation();
    probe_queue_.clear();
    artwork_queue_.clear();
    if (probe_watcher_.isRunning()) {
        probe_watcher_.waitForFinished();
    }
    if (artwork_watcher_.isRunning()) {
        artwork_watcher_.waitForFinished();
    }
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    if (artwork_outcome_) {
        __tsan_acquire(artwork_outcome_.get());
    }
#endif
    if (discovery_watcher_.isRunning()) {
        discovery_watcher_.waitForFinished();
    }
    if (metadata_operation_watcher_.isRunning()) {
        metadata_operation_watcher_.waitForFinished();
    }
    probe_running_ = false;
    artwork_running_ = false;
    discovery_running_ = false;
    metadata_operation_running_ = false;
    if (transport_timer_ != nullptr) {
        transport_timer_->stop();
    }
    player_ = nullptr;
    player_storage_.reset();
}

void BenchMainWindow::closeEvent(QCloseEvent* event) {
    std::vector<QPointer<MetadataPropertiesDialog>> properties_tabs;
    for (auto index = 0; index < tabs_->count(); ++index) {
        if (auto* properties = qobject_cast<MetadataPropertiesDialog*>(tabs_->widget(index))) {
            properties_tabs.emplace_back(properties);
        }
    }
    for (const auto& properties : properties_tabs) {
        if (properties != nullptr && !properties->close()) {
            event->ignore();
            return;
        }
    }
    stopBackgroundWork();
    persistNow(true);
    event->accept();
}

void BenchMainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    resizeMpdSearchField();
    if (mpd_search_surface_ != nullptr && mpd_search_surface_->isVisible()) {
        positionMpdSearchSurface();
    }
}

bool BenchMainWindow::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == tabs_ || (tabs_ != nullptr && watched == tabs_->tabBar())) &&
        event->type() == QEvent::Resize) {
        resizeMpdSearchField();
        QMetaObject::invokeMethod(this, [this] { resizeMpdSearchField(); }, Qt::QueuedConnection);
        if (mpd_search_surface_ != nullptr && mpd_search_surface_->isVisible()) {
            positionMpdSearchSurface();
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void BenchMainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void BenchMainWindow::dropEvent(QDropEvent* event) {
    std::vector<std::string> raw_paths;
    const auto urls = event->mimeData()->urls();
    raw_paths.reserve(static_cast<std::size_t>(urls.size()));
    for (const auto& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const auto encoded = QFile::encodeName(url.toLocalFile());
        raw_paths.emplace_back(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    }
    if (raw_paths.empty()) {
        return;
    }
    event->acceptProposedAction();
    openLocalPaths(std::move(raw_paths));
}

} // namespace trackknife::bench
