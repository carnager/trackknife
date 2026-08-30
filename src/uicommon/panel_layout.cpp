// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/panel_layout.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <utility>

namespace trackknife::ui {

namespace {

constexpr int maximum_layout_depth = 16;
constexpr int maximum_layout_nodes = 64;
constexpr int maximum_split_weight = 1'000'000;

[[nodiscard]] QString kindName(const PanelLayoutNodeKind kind) {
    switch (kind) {
    case PanelLayoutNodeKind::panel:
        return QStringLiteral("panel");
    case PanelLayoutNodeKind::split:
        return QStringLiteral("split");
    case PanelLayoutNodeKind::tabs:
        return QStringLiteral("tabs");
    }
    return {};
}

[[nodiscard]] QJsonObject serializeNode(const PanelLayoutNode& node) {
    QJsonObject object{{QStringLiteral("kind"), kindName(node.kind)}};
    if (node.kind == PanelLayoutNodeKind::panel) {
        object.insert(QStringLiteral("panel"), node.panel_id);
        return object;
    }

    QJsonArray children;
    for (const auto& child : node.children) {
        children.push_back(serializeNode(child));
    }
    object.insert(QStringLiteral("children"), children);
    if (node.kind == PanelLayoutNodeKind::split) {
        object.insert(QStringLiteral("orientation"), node.orientation == Qt::Horizontal
                                                         ? QStringLiteral("horizontal")
                                                         : QStringLiteral("vertical"));
        QJsonArray weights;
        for (const auto weight : node.weights) {
            weights.push_back(weight);
        }
        object.insert(QStringLiteral("weights"), weights);
    } else {
        object.insert(QStringLiteral("active"), node.active_child);
    }
    return object;
}

[[nodiscard]] std::optional<PanelLayoutNode> deserializeNode(const QJsonValue& value,
                                                             const QSet<QString>& registered,
                                                             QSet<QString>& used, int& node_count,
                                                             const int depth, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<PanelLayoutNode> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (!value.isObject() || depth > maximum_layout_depth || ++node_count > maximum_layout_nodes) {
        return fail(QStringLiteral("Panel layout exceeds its shape limits"));
    }
    const auto object = value.toObject();
    const auto kind = object.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("panel")) {
        const auto panel_id = object.value(QStringLiteral("panel")).toString();
        if (panel_id.isEmpty() || !registered.contains(panel_id)) {
            return fail(QStringLiteral("Panel layout references an unavailable panel"));
        }
        if (used.contains(panel_id)) {
            return fail(QStringLiteral("Panel layout uses one panel instance more than once"));
        }
        used.insert(panel_id);
        return panelLayoutPanel(panel_id);
    }

    const auto child_values = object.value(QStringLiteral("children")).toArray();
    std::vector<PanelLayoutNode> children;
    children.reserve(static_cast<std::size_t>(child_values.size()));
    for (const auto& child_value : child_values) {
        auto child = deserializeNode(child_value, registered, used, node_count, depth + 1, error);
        if (!child) {
            return std::nullopt;
        }
        children.push_back(std::move(*child));
    }

    if (kind == QStringLiteral("split")) {
        if (children.size() < 2U) {
            return fail(QStringLiteral("A panel split requires at least two children"));
        }
        const auto orientation_value = object.value(QStringLiteral("orientation")).toString();
        Qt::Orientation orientation;
        if (orientation_value == QStringLiteral("horizontal")) {
            orientation = Qt::Horizontal;
        } else if (orientation_value == QStringLiteral("vertical")) {
            orientation = Qt::Vertical;
        } else {
            return fail(QStringLiteral("Panel split orientation is invalid"));
        }
        std::vector<int> weights;
        const auto weight_values = object.value(QStringLiteral("weights")).toArray();
        weights.reserve(static_cast<std::size_t>(weight_values.size()));
        for (const auto& weight : weight_values) {
            const auto parsed = weight.toInt();
            if (parsed <= 0 || parsed > maximum_split_weight) {
                return fail(QStringLiteral("Panel split weights are outside the supported range"));
            }
            weights.push_back(parsed);
        }
        if (weights.size() != children.size()) {
            return fail(QStringLiteral("Panel split weights do not match its children"));
        }
        return panelLayoutSplit(orientation, std::move(children), std::move(weights));
    }

    if (kind == QStringLiteral("tabs")) {
        if (children.empty()) {
            return fail(QStringLiteral("A panel tab stack requires at least one child"));
        }
        const auto active = object.value(QStringLiteral("active")).toInt(-1);
        if (active < 0 || active >= static_cast<int>(children.size())) {
            return fail(QStringLiteral("Panel tab-stack selection is invalid"));
        }
        return panelLayoutTabs(std::move(children), active);
    }
    return fail(QStringLiteral("Panel layout node kind is unsupported"));
}

} // namespace

PanelLayoutNode panelLayoutPanel(QString panel_id) {
    return PanelLayoutNode{.kind = PanelLayoutNodeKind::panel,
                           .panel_id = std::move(panel_id),
                           .orientation = Qt::Horizontal,
                           .weights = {},
                           .active_child = 0,
                           .children = {}};
}

PanelLayoutNode panelLayoutSplit(const Qt::Orientation orientation,
                                 std::vector<PanelLayoutNode> children, std::vector<int> weights) {
    return PanelLayoutNode{.kind = PanelLayoutNodeKind::split,
                           .panel_id = {},
                           .orientation = orientation,
                           .weights = std::move(weights),
                           .active_child = 0,
                           .children = std::move(children)};
}

PanelLayoutNode panelLayoutTabs(std::vector<PanelLayoutNode> children, const int active_child) {
    return PanelLayoutNode{.kind = PanelLayoutNodeKind::tabs,
                           .panel_id = {},
                           .orientation = Qt::Horizontal,
                           .weights = {},
                           .active_child = active_child,
                           .children = std::move(children)};
}

QByteArray serializePanelLayout(const PanelLayout& layout) {
    return QJsonDocument{QJsonObject{{QStringLiteral("schema"), layout.schema_version},
                                     {QStringLiteral("root"), serializeNode(layout.root)}}}
        .toJson(QJsonDocument::Compact);
}

std::optional<PanelLayout> deserializePanelLayout(const QByteArray& bytes,
                                                  const QStringList& registered_panel_ids,
                                                  QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<PanelLayout> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("Invalid panel-layout JSON"));
    }
    const auto object = document.object();
    const auto schema = object.value(QStringLiteral("schema")).toInt();
    if (schema != panel_layout_schema_version) {
        return fail(QStringLiteral("Unsupported panel-layout version"));
    }
    const QSet<QString> registered{registered_panel_ids.begin(), registered_panel_ids.end()};
    if (registered.size() != registered_panel_ids.size() || registered.isEmpty()) {
        return fail(QStringLiteral("Panel registry is invalid"));
    }
    QSet<QString> used;
    int node_count = 0;
    auto root = deserializeNode(object.value(QStringLiteral("root")), registered, used, node_count,
                                0, error);
    if (!root) {
        return std::nullopt;
    }
    if (used != registered) {
        return fail(QStringLiteral("Panel layout does not place every registered panel"));
    }
    return PanelLayout{.schema_version = schema, .root = std::move(*root)};
}

} // namespace trackknife::ui
