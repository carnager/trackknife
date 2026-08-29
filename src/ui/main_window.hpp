// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <vector>

class QAbstractItemModel;
class QModelIndex;
class QPoint;
class QResizeEvent;
class QTableView;
class QWidget;

namespace trackknife::persistence {
struct ListDocument;
}

namespace trackknife::ui {

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(qint64 logical_rows = 0, QWidget* parent = nullptr);
    ~MainWindow() override;

    void openFormatSandbox();
    void openMpdConnectionDialog();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

  protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Impl;

    void buildWorkspace(qint64 logical_rows);
    void initializePersistence();
    void restoreWorkspace();
    void autoConnect();
    void resetWorkspace();
    void refreshUi();
    void refreshSelectionDetails();
    void setLibraryModel(QAbstractItemModel* model);
    void previewSearch();
    void commitSearch();
    void finishSearch(const QString& query, bool success);
    void installSearchView(QTableView* view);
    void openStoredPlaylistTab(const QString& name);
    void createScratchTab();
    void createNamedList();
    void duplicateCurrentTab();
    void toggleCurrentTabPinned();
    void saveCurrentWorkingList();
    void renameCurrentTab();
    void closeCurrentTab();
    void restoreLocalTabs(std::vector<persistence::ListDocument> documents);
    void persistLocalTabs();
    void flushLocalTabs();
    void addLocalListTab(persistence::ListDocument document, bool select);
    void refreshLocalList(QWidget* page, bool dirty);
    void moveLocalSelection(int direction);
    void removeLocalSelection();
    void cropLocalSelection();
    void clearLocalList();
    void sortLocalList();
    void reverseLocalList();
    void randomizeLocalList();
    void deduplicateLocalList();
    void transferSelectionTo(const QString& target_document_id, bool move);
    bool transferRows(QTableView* source, const QVariantList& rows,
                      const QString& target_document_id, bool move, int insertion_row);
    void reorderLocalRows(const QString& document_id, const QVariantList& rows, int insertion_row);
    void rebuildTransferMenus();
    void installTrackContextMenu(QTableView* view);
    void showTrackContextMenu(QTableView* view, const QPoint& position);
    void showTabContextMenu(const QPoint& position);
    void activateServerTreeAction(const QModelIndex& index, int action);
    void showServerTreeContextMenu(const QPoint& position);
    void completePendingServerTreeAction();
    void appendServerTreeSelectionToList(const QModelIndex& index,
                                         const QString& target_document_id);
    void activateCurrentSelection();
    void refreshTransport();
    void addCurrentSelection(bool next);
    [[nodiscard]] QStringList selectedRemoteUris(const QTableView* view) const;
    [[nodiscard]] QTableView* activeLibraryTabView() const;
    void closeLiveSearch();
    void syncLiveSearchView();
    void showToast(const QString& message);
    void positionToast();
    void positionLiveSearchSurface();

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::ui
