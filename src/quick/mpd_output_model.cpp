// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_output_model.hpp"

#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace trackknife::quick {
namespace {

[[nodiscard]] QString from_utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString output_detail(const mpd::Output& output) {
    QStringList parts;
    if (output.online) {
        parts.push_back(*output.online ? QStringLiteral("online") : QStringLiteral("offline"));
    }
    if (output.primary && *output.primary) {
        parts.push_back(QStringLiteral("primary"));
    }
    if (output.stream_format) {
        parts.push_back(from_utf8(*output.stream_format));
    } else if (output.plugin) {
        parts.push_back(from_utf8(*output.plugin));
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace

MpdOutputModel::MpdOutputModel(QObject* parent) : QAbstractListModel(parent) {}

int MpdOutputModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(
        std::min(outputs_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

QVariant MpdOutputModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= outputs_.size()) {
        return {};
    }
    const auto& output = outputs_.at(static_cast<std::size_t>(index.row()));
    switch (role) {
    case OutputIdRole:
        return QVariant::fromValue(output.id);
    case NameRole:
        return from_utf8(output.name);
    case EnabledRole:
        return output.enabled;
    case PrimaryRole:
        return output.primary.value_or(false);
    case OnlineKnownRole:
        return output.online.has_value();
    case OnlineRole:
        return output.online.value_or(false);
    case DetailRole:
        return output_detail(output);
    default:
        return {};
    }
}

QHash<int, QByteArray> MpdOutputModel::roleNames() const {
    return {
        {OutputIdRole, QByteArrayLiteral("outputId")},
        {NameRole, QByteArrayLiteral("outputName")},
        {EnabledRole, QByteArrayLiteral("outputEnabled")},
        {PrimaryRole, QByteArrayLiteral("primary")},
        {OnlineKnownRole, QByteArrayLiteral("onlineKnown")},
        {OnlineRole, QByteArrayLiteral("online")},
        {DetailRole, QByteArrayLiteral("detail")},
    };
}

void MpdOutputModel::replaceOutputs(std::vector<mpd::Output> outputs) {
    if (outputs_ == outputs) {
        return;
    }
    beginResetModel();
    outputs_ = std::move(outputs);
    endResetModel();
}

} // namespace trackknife::quick
