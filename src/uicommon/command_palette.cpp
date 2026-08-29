// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/command_palette.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>

namespace trackknife::ui {
namespace {

[[nodiscard]] QString commandName(const QAction* action) {
    auto name = action->text();
    name.remove(QLatin1Char('&'));
    return name.trimmed();
}

[[nodiscard]] QString shortcutKey(const QAction* action) {
    return QStringLiteral("shortcuts/%1").arg(action->objectName());
}

} // namespace

CommandPalette::CommandPalette(QList<QAction*> actions, QWidget* parent)
    : QDialog(parent), actions_(std::move(actions)) {
    actions_.removeIf([](const QAction* action) {
        return action == nullptr || action->isSeparator() || action->objectName().isEmpty() ||
               commandName(action).isEmpty();
    });
    std::ranges::sort(actions_, {},
                      [](const QAction* action) { return commandName(action).toCaseFolded(); });

    setObjectName(QStringLiteral("command-palette"));
    setWindowTitle(QStringLiteral("Commands and shortcuts"));
    setModal(false);
    resize(620, 480);

    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("command-filter"));
    filter_->setPlaceholderText(QStringLiteral("Type a command…"));
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(QStringLiteral("Command search"));
    results_ = new QListWidget(this);
    results_->setObjectName(QStringLiteral("command-results"));
    results_->setAccessibleName(QStringLiteral("Matching commands"));
    results_->setAlternatingRowColors(true);
    shortcut_ = new QKeySequenceEdit(this);
    shortcut_->setObjectName(QStringLiteral("command-shortcut"));
    shortcut_->setAccessibleName(QStringLiteral("Shortcut for selected command"));
    status_ = new QLabel(QStringLiteral("Choose a command to run or change its shortcut."), this);
    status_->setObjectName(QStringLiteral("command-status"));
    status_->setWordWrap(true);

    auto* shortcut_form = new QFormLayout;
    shortcut_form->addRow(QStringLiteral("Shortcut"), shortcut_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* apply =
        buttons_->addButton(QStringLiteral("Apply shortcut"), QDialogButtonBox::ApplyRole);
    apply->setObjectName(QStringLiteral("command-apply-shortcut"));
    auto* run = buttons_->addButton(QStringLiteral("Run"), QDialogButtonBox::AcceptRole);
    run->setObjectName(QStringLiteral("command-run"));
    run->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(filter_);
    layout->addWidget(results_, 1);
    layout->addLayout(shortcut_form);
    layout->addWidget(status_);
    layout->addWidget(buttons_);

    connect(filter_, &QLineEdit::textChanged, this, &CommandPalette::rebuild);
    connect(results_, &QListWidget::currentRowChanged, this, &CommandPalette::updateSelection);
    connect(results_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem*) { runCurrent(); });
    connect(apply, &QPushButton::clicked, this, &CommandPalette::applyShortcut);
    connect(run, &QPushButton::clicked, this, &CommandPalette::runCurrent);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::close);

    rebuild();
    filter_->setFocus();
}

void CommandPalette::restoreShortcuts(const QList<QAction*>& actions) {
    QSettings settings;
    for (auto* action : actions) {
        if (action == nullptr || action->objectName().isEmpty()) {
            continue;
        }
        const auto key = shortcutKey(action);
        if (settings.contains(key)) {
            action->setShortcut(QKeySequence::fromString(settings.value(key).toString(),
                                                         QKeySequence::PortableText));
        }
    }
}

void CommandPalette::rebuild() {
    const auto query = filter_->text().trimmed();
    results_->clear();
    for (auto* action : actions_) {
        const auto name = commandName(action);
        const auto shortcut = action->shortcut().toString(QKeySequence::NativeText);
        if (!query.isEmpty() && !name.contains(query, Qt::CaseInsensitive) &&
            !action->objectName().contains(query, Qt::CaseInsensitive) &&
            !shortcut.contains(query, Qt::CaseInsensitive)) {
            continue;
        }
        auto* item = new QListWidgetItem(action->icon(), name, results_);
        item->setData(Qt::UserRole, QVariant::fromValue(static_cast<QObject*>(action)));
        item->setToolTip(action->toolTip().isEmpty() ? action->statusTip() : action->toolTip());
        if (!shortcut.isEmpty()) {
            item->setText(QStringLiteral("%1\t%2").arg(name, shortcut));
        }
        item->setFlags(item->flags().setFlag(Qt::ItemIsEnabled, action->isEnabled()));
    }
    if (results_->count() > 0) {
        results_->setCurrentRow(0);
    } else {
        shortcut_->clear();
        shortcut_->setEnabled(false);
        status_->setText(QStringLiteral("No matching command"));
    }
}

QAction* CommandPalette::currentAction() const {
    const auto* item = results_->currentItem();
    if (item == nullptr) {
        return nullptr;
    }
    return qobject_cast<QAction*>(item->data(Qt::UserRole).value<QObject*>());
}

void CommandPalette::updateSelection() {
    auto* action = currentAction();
    shortcut_->setEnabled(action != nullptr);
    shortcut_->setKeySequence(action != nullptr ? action->shortcut() : QKeySequence{});
    status_->setText(action == nullptr
                         ? QStringLiteral("No matching command")
                         : QStringLiteral("%1 · %2").arg(action->isEnabled()
                                                             ? QStringLiteral("Available")
                                                             : QStringLiteral("Unavailable here"),
                                                         action->objectName()));
}

void CommandPalette::applyShortcut() {
    auto* action = currentAction();
    if (action == nullptr) {
        return;
    }
    const auto sequence = shortcut_->keySequence();
    const auto conflict = std::ranges::find_if(actions_, [action, &sequence](const QAction* other) {
        return other != action && !sequence.isEmpty() && other->shortcut() == sequence;
    });
    if (conflict != actions_.end()) {
        status_->setText(QStringLiteral("Already used by “%1”").arg(commandName(*conflict)));
        return;
    }
    action->setShortcut(sequence);
    QSettings settings;
    settings.setValue(shortcutKey(action), sequence.toString(QKeySequence::PortableText));
    status_->setText(QStringLiteral("Shortcut saved"));
    rebuild();
}

void CommandPalette::runCurrent() {
    auto* action = currentAction();
    if (action == nullptr || !action->isEnabled()) {
        return;
    }
    close();
    action->trigger();
}

} // namespace trackknife::ui
