// SPDX-License-Identifier: GPL-3.0-only

#include "quick/mpd_browser_model.hpp"

#include <QString>

#include <algorithm>
#include <array>
#include <concepts>
#include <limits>
#include <string_view>
#include <type_traits>

namespace trackknife::quick {
namespace {

constexpr std::array<const char*, MpdBrowserModel::column_count> headers{"Name", "Type",
                                                                         "Modified"};

[[nodiscard]] QString from_utf8(const std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] QString leaf_name(const std::string_view uri) {
    const auto slash = uri.find_last_of('/');
    return from_utf8(slash == std::string_view::npos ? uri : uri.substr(slash + 1U));
}

} // namespace

MpdBrowserModel::MpdBrowserModel(QObject* parent) : QAbstractTableModel(parent) {}

int MpdBrowserModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(
        std::min(entries_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

int MpdBrowserModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : column_count;
}

QVariant MpdBrowserModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.column() < 0 ||
        index.column() >= column_count ||
        static_cast<std::size_t>(index.row()) >= entries_.size()) {
        return {};
    }
    const auto& entry = entries_.at(static_cast<std::size_t>(index.row()));
    const auto kind = kindAt(index.row());
    const auto uri = uriAt(index.row());
    if (!kind || !uri) {
        return {};
    }
    if (role == UriRole) {
        return from_utf8(*uri);
    }
    if (role == KindRole) {
        return static_cast<int>(*kind);
    }
    if (role == ContainerRole) {
        return *kind != EntryKind::track;
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    if (index.column() == 0) {
        return leaf_name(*uri);
    }
    if (index.column() == 1) {
        switch (*kind) {
        case EntryKind::directory:
            return QStringLiteral("Folder");
        case EntryKind::track:
            return QStringLiteral("Track");
        case EntryKind::playlist:
            return QStringLiteral("Playlist");
        }
    }
    const auto& modified = std::visit(
        [](const auto& value) -> const std::optional<std::string>& { return value.last_modified; },
        entry);
    if (modified) {
        return from_utf8(*modified);
    }
    return {};
}

QVariant MpdBrowserModel::headerData(const int section, const Qt::Orientation orientation,
                                     const int role) const {
    if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 &&
        section < column_count) {
        return QString::fromLatin1(headers.at(static_cast<std::size_t>(section)));
    }
    return {};
}

QHash<int, QByteArray> MpdBrowserModel::roleNames() const {
    auto roles = QAbstractTableModel::roleNames();
    roles.insert(UriRole, QByteArrayLiteral("uri"));
    roles.insert(KindRole, QByteArrayLiteral("entryKind"));
    roles.insert(ContainerRole, QByteArrayLiteral("container"));
    return roles;
}

std::optional<MpdBrowserModel::EntryKind> MpdBrowserModel::kindAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return std::nullopt;
    }
    const auto& entry = entries_.at(static_cast<std::size_t>(row));
    if (std::holds_alternative<mpd::DatabaseDirectory>(entry)) {
        return EntryKind::directory;
    }
    if (std::holds_alternative<mpd::Track>(entry)) {
        return EntryKind::track;
    }
    return EntryKind::playlist;
}

std::optional<std::string> MpdBrowserModel::uriAt(const int row) const {
    if (row < 0 || static_cast<std::size_t>(row) >= entries_.size()) {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& entry) {
            if constexpr (std::same_as<std::decay_t<decltype(entry)>, mpd::StoredPlaylist>) {
                return entry.name;
            } else {
                return entry.uri;
            }
        },
        entries_.at(static_cast<std::size_t>(row)));
}

void MpdBrowserModel::replaceEntries(std::vector<mpd::DatabaseEntry> entries) {
    if (entries_ == entries) {
        return;
    }
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

} // namespace trackknife::quick
