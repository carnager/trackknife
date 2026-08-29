// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "ui/server_library_tree_model.hpp"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QTableWidget;

namespace trackknife::ui {

class LibraryTreeEditorDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit LibraryTreeEditorDialog(LibraryTreeDefinition definition, QWidget* parent = nullptr);

    [[nodiscard]] LibraryTreeDefinition definition() const;

  protected:
    void accept() override;

  private:
    void populate(const LibraryTreeDefinition& definition);
    void addLevel(LibraryTreeLevelDefinition level, int row = -1);
    void moveCurrentLevel(int offset);

    QLineEdit* name_{nullptr};
    QComboBox* root_tag_{nullptr};
    QTableWidget* levels_{nullptr};
};

} // namespace trackknife::ui
