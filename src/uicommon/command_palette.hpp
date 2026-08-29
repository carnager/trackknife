// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>

class QAction;
class QDialogButtonBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;

namespace trackknife::ui {

class CommandPalette final : public QDialog {
    Q_OBJECT

  public:
    explicit CommandPalette(QList<QAction*> actions, QWidget* parent = nullptr);

    static void restoreShortcuts(const QList<QAction*>& actions);

  private:
    void rebuild();
    void updateSelection();
    void applyShortcut();
    void runCurrent();
    [[nodiscard]] QAction* currentAction() const;

    QList<QAction*> actions_;
    QLineEdit* filter_{nullptr};
    QListWidget* results_{nullptr};
    QKeySequenceEdit* shortcut_{nullptr};
    QLabel* status_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

} // namespace trackknife::ui
