// SPDX-License-Identifier: GPL-3.0-only

#include "bench/local_library_panel.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace trackknife::bench {
namespace {
constexpr int entry_role = Qt::UserRole + 1;
constexpr int query_role = Qt::UserRole + 2;
constexpr int loaded_role = Qt::UserRole + 3;
constexpr int more_role = Qt::UserRole + 4;

QString text(const std::string& value) { return QString::fromUtf8(value); }
std::string bytes(const QString& value) { return value.toUtf8().toStdString(); }
QString pathLabel(const std::string& value) { return text(core::escape_raw_path(value)); }
QByteArray entryKey(const persistence::LibraryEntry& entry) {
    return QByteArray::number(static_cast<int>(entry.kind)) + ':' +
           QByteArray::fromStdString(entry.key);
}
} // namespace

LocalLibraryPanel::LocalLibraryPanel(std::filesystem::path database_path, QWidget* parent)
    : QWidget(parent), database_path_(std::move(database_path)) {
    setObjectName(QStringLiteral("bench-local-library"));
    pool_.setMaxThreadCount(2);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* tools = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("local-library-search"));
    search_->setPlaceholderText(tr("Search albums and tracks"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("Search local library"));
    layout->addWidget(search_);
    auto* folders = new QToolButton(this);
    folders->setObjectName(QStringLiteral("local-library-folders"));
    folders->setText(tr("Folders…"));
    folders->setToolTip(tr("Choose which folders belong to your music library"));
    connect(folders, &QToolButton::clicked, this, &LocalLibraryPanel::showFolders);
    scan_button_ = new QToolButton(this);
    scan_button_->setObjectName(QStringLiteral("local-library-scan"));
    scan_button_->setText(tr("Refresh"));
    connect(scan_button_, &QToolButton::clicked, this, [this] {
        if (scanning_) {
            scan_cancellation_.request_cancellation();
            rescan_pending_ = false;
            status_->setText(tr("Stopping scan…"));
        } else {
            startScan();
        }
    });
    tools->addWidget(folders);
    tools->addStretch();
    tools->addWidget(scan_button_);
    layout->addLayout(tools);
    tree_ = new QTreeView(this);
    tree_->setObjectName(QStringLiteral("local-library-tree"));
    tree_->setAccessibleName(tr("Local artists, albums, and tracks"));
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    model_ = new QStandardItemModel(tree_);
    tree_->setModel(model_);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        auto* item = model_->itemFromIndex(index);
        if (item != nullptr && item->data(entry_role).isValid()) {
            expanded_entries_.insert(
                entryKey(item->data(entry_role).value<persistence::LibraryEntry>()));
        }
        if (item == nullptr || item->data(loaded_role).toBool() ||
            !item->data(query_role).isValid()) {
            return;
        }
        item->setData(true, loaded_role);
        loadChildren(QPersistentModelIndex{index},
                     item->data(query_role).value<persistence::LibraryQuery>());
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (index.data(entry_role).isValid()) {
            expanded_entries_.remove(
                entryKey(index.data(entry_role).value<persistence::LibraryEntry>()));
        }
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& index) {
                if (index.data(entry_role).isValid()) {
                    current_entry_ =
                        entryKey(index.data(entry_role).value<persistence::LibraryEntry>());
                }
            });
    connect(tree_, &QTreeView::activated, this, &LocalLibraryPanel::activate);
    connect(tree_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& position) {
        const auto index = tree_->indexAt(position);
        if (!index.data(entry_role).isValid()) {
            return;
        }
        auto* menu = new QMenu(tree_);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const QPersistentModelIndex target{index};
        menu->addAction(tr("Open in local queue"), this, [this, target] { activate(target); });
        menu->popup(tree_->viewport()->mapToGlobal(position));
    });
    status_ = new QLabel(tr("Choose Folders… to add your music collection."), this);
    status_->setObjectName(QStringLiteral("local-library-status"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(200);
    connect(search_, &QLineEdit::textChanged, search_timer_, qOverload<>(&QTimer::start));
    connect(search_, &QLineEdit::textChanged, this, [this] { status_->setText(tr("Searching…")); });
    connect(search_timer_, &QTimer::timeout, this, &LocalLibraryPanel::reloadTree);
    connect(&query_watcher_, &QFutureWatcherBase::finished, this, [this] {
        querying_ = false;
        auto outcome = query_watcher_.result();
        auto done = std::exchange(completion_, {});
        if (!stopped_) {
            if (done) {
                done(std::move(outcome));
            }
            pump();
        }
    });
    connect(&scan_watcher_, &QFutureWatcherBase::finished, this, [this] {
        scanning_ = false;
        setProperty("scanning", false);
        scan_button_->setText(tr("Refresh"));
        poll_timer_->stop();
        if (stopped_) {
            return;
        }
        const auto outcome = scan_watcher_.result();
        if (!outcome.error.isEmpty()) {
            status_->setText(outcome.error);
        } else if (outcome.result.cancelled) {
            status_->setText(tr("Scan stopped. Completed updates were kept."));
        } else if (outcome.result.incomplete) {
            status_->setText(
                tr("Scan incomplete. %1 files could not be read; previous entries were kept.")
                    .arg(progress_->failed.load()));
        } else {
            const auto updated = progress_->indexed.load();
            status_->setText(updated == 0U ? tr("Library up to date.")
                             : updated == 1U
                                 ? tr("Library up to date. 1 file updated.")
                                 : tr("Library up to date. %1 files updated.").arg(updated));
        }
        reloadTree();
        loadRoots();
        if (std::exchange(rescan_pending_, false)) {
            startScan();
        }
    });
    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(200);
    connect(poll_timer_, &QTimer::timeout, this, &LocalLibraryPanel::updateProgress);
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(30'000);
    connect(refresh_timer_, &QTimer::timeout, this, [this] {
        if (!scanning_) {
            startScan();
        }
    });
    refresh_timer_->start();
    change_timer_ = new QTimer(this);
    change_timer_->setSingleShot(true);
    change_timer_->setInterval(250);
    connect(change_timer_, &QTimer::timeout, this, [this] {
        reloadTree();
        if (scanning_) {
            rescan_pending_ = true;
        } else {
            startScan();
        }
    });
    reloadTree();
    loadRoots();
    startScan();
}

LocalLibraryPanel::~LocalLibraryPanel() { stop(); }

void LocalLibraryPanel::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    lifetime_cancellation_.request_cancellation();
    view_cancellation_.request_cancellation();
    scan_cancellation_.request_cancellation();
    search_timer_->stop();
    refresh_timer_->stop();
    poll_timer_->stop();
    change_timer_->stop();
    tasks_.clear();
    pool_.waitForDone();
}

void LocalLibraryPanel::enqueue(Task task) {
    if (stopped_) {
        return;
    }
    if (tasks_.size() >= 64U) {
        status_->setText(tr("Please wait for the current library requests."));
        return;
    }
    tasks_.push_back(std::move(task));
    pump();
}

void LocalLibraryPanel::pump() {
    if (querying_ || tasks_.empty() || stopped_) {
        return;
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    completion_ = std::move(task.done);
    querying_ = true;
    query_watcher_.setFuture(
        QtConcurrent::run(&pool_, [path = database_path_, work = std::move(task.work)] {
            auto library = persistence::LocalLibrary::open(path);
            if (!library) {
                Outcome result;
                result.error = text(library.error().message);
                return result;
            }
            return work(*library);
        }));
}

void LocalLibraryPanel::reloadTree() {
    ++generation_;
    view_cancellation_.request_cancellation();
    view_cancellation_ = core::CancellationSource{};
    std::erase_if(tasks_, [](const Task& task) { return task.view_query; });
    if (previous_search_ != search_->text()) {
        previous_search_ = search_->text();
        expanded_entries_.clear();
        current_entry_.clear();
    }
    model_->clear();
    const auto query_text = bytes(search_->text().trimmed());
    if (query_text.empty()) {
        loadChildren({}, {});
        return;
    }
    for (const auto kind :
         {persistence::LibraryEntryKind::album, persistence::LibraryEntryKind::track}) {
        auto* group = new QStandardItem(
            kind == persistence::LibraryEntryKind::album ? tr("Albums") : tr("Tracks"));
        group->setEditable(false);
        group->setData(true, loaded_role);
        model_->appendRow(group);
        persistence::LibraryQuery query;
        query.kind = kind;
        query.text = query_text;
        loadChildren(QPersistentModelIndex{group->index()}, query);
        tree_->expand(group->index());
    }
}

void LocalLibraryPanel::loadChildren(const QPersistentModelIndex& parent,
                                     persistence::LibraryQuery query) {
    const auto generation = generation_;
    const auto root = !parent.isValid();
    enqueue(
        {[query, cancellation = view_cancellation_.token()](persistence::LocalLibrary& library) {
             Outcome outcome;
             const auto result = library.query(query, cancellation);
             if (result) {
                 outcome.page = *result;
             } else {
                 outcome.error = text(result.error().message);
             }
             return outcome;
         },
         [this, parent, root, query, generation](Outcome outcome) mutable {
             if (generation != generation_ || (!root && !parent.isValid())) {
                 return;
             }
             auto* target = root ? model_->invisibleRootItem() : model_->itemFromIndex(parent);
             if (target == nullptr) {
                 return;
             }
             if (!outcome.error.isEmpty()) {
                 status_->setText(outcome.error);
                 target->setData(false, loaded_role);
                 return;
             }
             if (query.offset == 0U) {
                 target->removeRows(0, target->rowCount());
             }
             if (!scanning_ && status_->text() == tr("Searching…")) {
                 status_->setText(search_->text().trimmed().isEmpty()
                                      ? tr("Browse artists and albums.")
                                      : tr("Search results"));
             }
             for (const auto& entry : outcome.page.entries) {
                 auto label = text(entry.label);
                 if (entry.kind != persistence::LibraryEntryKind::track) {
                     label += tr(" (%1)").arg(entry.tracks);
                 }
                 if (entry.available == 0U) {
                     label += tr(" — unavailable");
                 } else if (entry.available < entry.tracks) {
                     label += tr(" — %1 unavailable").arg(entry.tracks - entry.available);
                 }
                 auto* item = new QStandardItem(label);
                 item->setEditable(false);
                 item->setData(QVariant::fromValue(entry), entry_role);
                 item->setToolTip(entry.kind == persistence::LibraryEntryKind::track
                                      ? pathLabel(entry.key)
                                      : text(entry.artist + " — " + entry.album));
                 if (entry.kind != persistence::LibraryEntryKind::track) {
                     persistence::LibraryQuery children;
                     if (entry.kind == persistence::LibraryEntryKind::artist) {
                         children.kind = persistence::LibraryEntryKind::album;
                         children.artist = entry.key;
                     } else {
                         children.kind = persistence::LibraryEntryKind::track;
                         children.album_key = entry.key;
                     }
                     item->setData(QVariant::fromValue(children), query_role);
                     item->appendRow(new QStandardItem(tr("Loading…")));
                 }
                 target->appendRow(item);
                 if (current_entry_ == entryKey(entry)) {
                     tree_->setCurrentIndex(item->index());
                 }
                 if (expanded_entries_.contains(entryKey(entry))) {
                     tree_->expand(item->index());
                 }
             }
             if (outcome.page.more) {
                 query.offset += outcome.page.entries.size();
                 auto* more = new QStandardItem(tr("Show more…"));
                 more->setEditable(false);
                 more->setData(true, more_role);
                 more->setData(QVariant::fromValue(query), query_role);
                 target->appendRow(more);
             } else if (target->rowCount() == 0) {
                 auto* empty = new QStandardItem(tr("No matches"));
                 empty->setEnabled(false);
                 target->appendRow(empty);
             }
         },
         true});
}

void LocalLibraryPanel::activate(const QModelIndex& index) {
    auto* item = model_->itemFromIndex(index);
    if (item == nullptr) {
        return;
    }
    if (item->data(more_role).toBool()) {
        const auto query = item->data(query_role).value<persistence::LibraryQuery>();
        const QPersistentModelIndex parent{index.parent()};
        model_->removeRow(index.row(), index.parent());
        loadChildren(parent, query);
        return;
    }
    if (!item->data(entry_role).isValid()) {
        return;
    }
    const auto entry = item->data(entry_role).value<persistence::LibraryEntry>();
    if (entry.available == 0U) {
        status_->setText(
            tr("These files are unavailable. Reconnect the folder and refresh the library."));
        return;
    }
    persistence::LibraryQuery query;
    if (entry.kind == persistence::LibraryEntryKind::artist) {
        query.artist = entry.key;
    } else if (entry.kind == persistence::LibraryEntryKind::album) {
        query.album_key = entry.key;
    } else {
        query.raw_path = entry.key;
    }
    enqueue({[query,
              cancellation = lifetime_cancellation_.token()](persistence::LocalLibrary& library) {
                 Outcome outcome;
                 auto result = library.paths(query, cancellation);
                 if (result) {
                     outcome.paths = std::move(*result);
                 } else {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this, unavailable = entry.tracks - entry.available](Outcome outcome) {
                 if (!outcome.error.isEmpty()) {
                     status_->setText(outcome.error);
                     return;
                 }
                 if (unavailable > 0U) {
                     status_->setText(tr("%1 unavailable files were skipped.").arg(unavailable));
                 }
                 if (!outcome.paths.empty()) {
                     emit pathsRequested(std::move(outcome.paths));
                 }
             }});
}

void LocalLibraryPanel::addRoot(std::string raw_path) {
    enqueue({[raw_path = std::move(raw_path)](persistence::LocalLibrary& library) {
                 Outcome outcome;
                 const auto result = library.add_root(raw_path);
                 if (!result) {
                     outcome.error = text(result.error().message);
                 }
                 return outcome;
             },
             [this](Outcome outcome) {
                 if (outcome.error.isEmpty()) {
                     loadRoots();
                     refreshLibrary();
                 } else {
                     status_->setText(outcome.error);
                 }
                 if (folders_dialog_) {
                     roots_error_->setText(outcome.error);
                 }
             }});
}

void LocalLibraryPanel::showFolders() {
    if (folders_dialog_) {
        folders_dialog_->raise();
        folders_dialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("local-library-folders-dialog"));
    dialog->setWindowTitle(tr("Local library folders"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(560, 320);
    folders_dialog_ = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* explanation =
        new QLabel(tr("Choose the folders to browse and search as your local music library. "
                      "Removing a folder from this list leaves its files untouched."),
                   dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    roots_list_ = new QListWidget(dialog);
    roots_list_->setObjectName(QStringLiteral("local-library-roots"));
    layout->addWidget(roots_list_, 1);
    roots_error_ = new QLabel(dialog);
    roots_error_->setObjectName(QStringLiteral("local-library-folder-error"));
    roots_error_->setWordWrap(true);
    layout->addWidget(roots_error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* add = buttons->addButton(tr("Add folder…"), QDialogButtonBox::ActionRole);
    auto* remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    remove->setEnabled(false);
    connect(roots_list_, &QListWidget::currentRowChanged, remove,
            [remove](int row) { remove->setEnabled(row >= 0); });
    connect(add, &QPushButton::clicked, this, [this] {
        const auto path =
            QFileDialog::getExistingDirectory(folders_dialog_, tr("Add music folder"));
        if (!path.isEmpty()) {
            addRoot(QFile::encodeName(path).toStdString());
        }
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!folders_dialog_ || roots_list_->currentItem() == nullptr) {
            return;
        }
        const auto path =
            roots_list_->currentItem()->data(Qt::UserRole).toByteArray().toStdString();
        enqueue({[path](persistence::LocalLibrary& library) {
                     Outcome outcome;
                     auto result = library.remove_root(path);
                     if (!result) {
                         outcome.error = text(result.error().message);
                     }
                     return outcome;
                 },
                 [this](Outcome outcome) {
                     if (outcome.error.isEmpty()) {
                         loadRoots();
                         reloadTree();
                     } else {
                         status_->setText(outcome.error);
                     }
                     if (folders_dialog_) {
                         roots_error_->setText(outcome.error);
                     }
                 }});
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    loadRoots();
    dialog->show();
}

void LocalLibraryPanel::loadRoots() {
    enqueue({[](persistence::LocalLibrary& library) {
                 Outcome outcome;
                 auto roots = library.roots();
                 if (roots) {
                     outcome.roots = std::move(*roots);
                 } else {
                     outcome.error = text(roots.error().message);
                 }
                 return outcome;
             },
             [this](Outcome outcome) {
                 if (!outcome.error.isEmpty()) {
                     status_->setText(outcome.error);
                     return;
                 }
                 if (outcome.roots.empty()) {
                     status_->setText(tr("Choose Folders… to add your music collection."));
                 }
                 std::size_t offline = 0;
                 if (folders_dialog_) {
                     roots_list_->clear();
                 }
                 for (const auto& root : outcome.roots) {
                     if (!root.available) {
                         ++offline;
                     }
                     if (!folders_dialog_) {
                         continue;
                     }
                     auto* item = new QListWidgetItem(
                         pathLabel(root.raw_path) +
                             (root.available ? QString{} : tr(" — unavailable")),
                         roots_list_);
                     item->setData(Qt::UserRole, QByteArray::fromStdString(root.raw_path));
                     item->setToolTip(text(root.error));
                 }
                 if (offline > 0U && !scanning_) {
                     status_->setText(
                         tr("%1 folders unavailable. Cached music is still shown.").arg(offline));
                 }
             }});
}

void LocalLibraryPanel::refreshLibrary() {
    if (!stopped_) {
        change_timer_->start();
    }
}

void LocalLibraryPanel::startScan() {
    if (stopped_ || scanning_) {
        return;
    }
    scanning_ = true;
    setProperty("scanning", true);
    scan_cancellation_ = core::CancellationSource{};
    progress_ = std::make_shared<persistence::LibraryScanProgress>();
    scan_button_->setText(tr("Stop"));
    poll_timer_->start();
    updateProgress();
    scan_watcher_.setFuture(
        QtConcurrent::run(&pool_, [path = database_path_, cancellation = scan_cancellation_.token(),
                                   progress = progress_] {
            ScanOutcome outcome;
            auto library = persistence::LocalLibrary::open(path);
            if (!library) {
                outcome.error = text(library.error().message);
                return outcome;
            }
            auto result = library->scan(cancellation, *progress);
            if (result) {
                outcome.result = *result;
            } else {
                outcome.error = text(result.error().message);
            }
            return outcome;
        }));
}

void LocalLibraryPanel::updateProgress() {
    if (!progress_) {
        return;
    }
    if (scan_cancellation_.is_cancellation_requested()) {
        status_->setText(tr("Stopping scan…"));
        return;
    }
    status_->setText(tr("Scanning… %1 entries checked, %2 files updated, %3 unreadable")
                         .arg(progress_->visited.load())
                         .arg(progress_->indexed.load())
                         .arg(progress_->failed.load()));
}

} // namespace trackknife::bench
