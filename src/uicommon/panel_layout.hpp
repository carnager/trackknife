// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <Qt>

#include <optional>
#include <vector>

namespace trackknife::ui {

inline constexpr int panel_layout_schema_version = 1;

enum class PanelLayoutNodeKind { panel, split, tabs };

// Declarative UI-only composition tree. Panel IDs name registered widget
// instances; split and tab nodes own only layout, never application state.
struct PanelLayoutNode {
    PanelLayoutNodeKind kind{PanelLayoutNodeKind::panel};
    QString panel_id;
    Qt::Orientation orientation{Qt::Horizontal};
    std::vector<int> weights;
    int active_child{0};
    std::vector<PanelLayoutNode> children;

    friend bool operator==(const PanelLayoutNode&, const PanelLayoutNode&) = default;
};

struct PanelLayout {
    int schema_version{panel_layout_schema_version};
    PanelLayoutNode root;

    friend bool operator==(const PanelLayout&, const PanelLayout&) = default;
};

[[nodiscard]] PanelLayoutNode panelLayoutPanel(QString panel_id);
[[nodiscard]] PanelLayoutNode panelLayoutSplit(Qt::Orientation orientation,
                                               std::vector<PanelLayoutNode> children,
                                               std::vector<int> weights);
[[nodiscard]] PanelLayoutNode panelLayoutTabs(std::vector<PanelLayoutNode> children,
                                              int active_child = 0);

[[nodiscard]] QByteArray serializePanelLayout(const PanelLayout& layout);

// Strictly validates schema, shape, bounds, registered panel IDs, and unique
// use of every registered instance. Unknown/newer layouts fail without being
// rewritten by the caller, preserving forward configuration.
[[nodiscard]] std::optional<PanelLayout>
deserializePanelLayout(const QByteArray& bytes, const QStringList& registered_panel_ids,
                       QString* error = nullptr);

} // namespace trackknife::ui
