// SPDX-License-Identifier: GPL-3.0-only

#include "bench/mpd_library_search_model.hpp"
#include "trackknife/titleformat/compiler.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {
using Kind = quick::MpdSearchResultModel::ResultKind;
QString tag(const mpd::Track& track, const std::string_view name) {
    const auto values = track.metadata.values(name);
    return values.empty() ? QString{} : QString::fromUtf8(values.front());
}
class TrackLabelContext final : public titleformat::EvaluationContext {
  public:
    TrackLabelContext(const QString& title, const QString& artist, const QString& number)
        : title_(title.toStdString()), artist_(artist.toStdString()),
          number_(number.section(QLatin1Char('/'), 0, 0).trimmed().toUInt()) {}
    titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::tree_level;
    }
    std::optional<std::string> resolveField(const std::string_view name) const override {
        if (name == "title")
            return title_;
        if (name == "artist")
            return artist_;
        if (name == "tracknumber" && number_ > 0)
            return std::to_string(number_);
        return std::nullopt;
    }

  private:
    std::string title_;
    std::string artist_;
    unsigned number_;
};
QString trackLabel(const QString& title, const QString& number,
                   const std::optional<QString>& artist = std::nullopt) {
    const titleformat::CompileOptions options{
        .context = titleformat::FormatContextKind::tree_level, .dialect = {}, .parse_options = {}};
    // Match the shipped tkfmt-1 local-library labels, including two-digit numbers.
    static const auto track =
        titleformat::compile("$if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
    static const auto found = titleformat::compile(
        "%artist% — $if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
    const auto& compiled = artist ? found : track;
    if (!compiled.program)
        return title;
    const TrackLabelContext context{title, artist.value_or(QString{}), number};
    const auto rendered = titleformat::evaluate(*compiled.program, context);
    return rendered ? QString::fromUtf8(rendered->text) : title;
}
QStandardItem* placeholder(const QString& text) {
    auto* item = new QStandardItem(text);
    item->setFlags(Qt::NoItemFlags);
    return item;
}
QStandardItem* trackItem(const mpd::Track& track) {
    const auto uri = QString::fromUtf8(track.uri);
    auto title = tag(track, "Title");
    auto* item = new QStandardItem(trackLabel(title.isEmpty() ? uri : title, tag(track, "Track")));
    item->setEditable(false);
    item->setDropEnabled(false);
    item->setData(static_cast<int>(Kind::track), MpdLibrarySearchModel::KindRole);
    item->setData(QStringList{uri}, MpdLibrarySearchModel::UrisRole);
    item->setToolTip(uri);
    item->setIcon(QIcon::fromTheme(QStringLiteral("audio-x-generic")));
    return item;
}
} // namespace

MpdLibrarySearchModel::MpdLibrarySearchModel(quick::MpdSearchResultModel* source, QObject* parent)
    : QStandardItemModel(parent), source_(source) {
    connect(source_, &QAbstractItemModel::modelReset, this, &MpdLibrarySearchModel::rebuild);
    connect(source_, &QAbstractItemModel::dataChanged, this, [this] {
        if (auto* albums = item(0)) {
            for (int row = 0; row < albums->rowCount(); ++row) {
                const auto index = albums->child(row)->index();
                emit dataChanged(index, index, {Qt::DecorationRole});
            }
        }
    });
    rebuild();
}

QVariant MpdLibrarySearchModel::data(const QModelIndex& index, const int role) const {
    if (role == Qt::DecorationRole && index.data(KindRole).isValid() &&
        index.data(KindRole).toInt() == static_cast<int>(Kind::album)) {
        const auto source = source_->index(index.data(SourceRowRole).toInt(), 0);
        const auto art = source.data(Qt::DecorationRole);
        if (art.canConvert<QImage>()) {
            return QIcon{QPixmap::fromImage(art.value<QImage>())};
        }
        return art;
    }
    return QStandardItemModel::data(index, role);
}

bool MpdLibrarySearchModel::actionable(const QModelIndex& index) {
    return index.isValid() && index.data(KindRole).isValid();
}

void MpdLibrarySearchModel::rebuild() {
    auto pending = std::exchange(pending_, {});
    loading_ = false;
    clear();
    for (const auto& label : {tr("Albums"), tr("Tracks")}) {
        auto* group = new QStandardItem(label);
        group->setEditable(false);
        group->setDragEnabled(false);
        group->setDropEnabled(false);
        appendRow(group);
    }
    for (int row = 0; row < source_->rowCount(); ++row) {
        const auto kind = source_->kindAt(row);
        if (kind == Kind::section) {
            continue;
        }
        const auto source_index = source_->index(row, 1);
        auto label = source_index.data().toString();
        if (kind == Kind::track) {
            label = trackLabel(label,
                               source_->index(row, 0)
                                   .data(quick::MpdSearchResultModel::TrackNumberRole)
                                   .toString(),
                               source_->index(row, 0).data().toString());
        }
        auto* entry = new QStandardItem(label);
        entry->setEditable(false);
        entry->setDropEnabled(false);
        entry->setData(static_cast<int>(kind), KindRole);
        entry->setData(row, SourceRowRole);
        entry->setData(source_->index(row, 0).data().toString(), ArtistRole);
        entry->setToolTip(source_->urisAt(row).join(QLatin1Char('\n')));
        if (kind == Kind::album) {
            entry->setToolTip(entry->data(ArtistRole).toString() + QStringLiteral(" — ") + label);
            entry->appendRow(placeholder(tr("Loading…")));
            item(0)->appendRow(entry);
        } else {
            entry->setIcon(QIcon::fromTheme(QStringLiteral("audio-x-generic")));
            entry->setData(source_->urisAt(row), UrisRole);
            item(1)->appendRow(entry);
        }
    }
    for (int group = 0; group < 2; ++group) {
        if (item(group)->rowCount() == 0) {
            item(group)->appendRow(placeholder(tr("No matches")));
        }
    }
    emit rebuilt();
    for (auto& request : pending) {
        for (auto& done : request.completions) {
            done(tr("Search changed before the selection finished loading"));
        }
    }
}

void MpdLibrarySearchModel::setMore(const bool more) {
    auto* tracks = item(1);
    if (!tracks) {
        return;
    }
    const auto has_more =
        tracks->rowCount() && tracks->child(tracks->rowCount() - 1)->data(MoreRole).toBool();
    if (has_more == more)
        return;
    if (has_more) {
        tracks->removeRow(tracks->rowCount() - 1);
    }
    if (more) {
        auto* next = new QStandardItem(tr("Show more…"));
        next->setEditable(false);
        next->setDragEnabled(false);
        next->setDropEnabled(false);
        next->setData(true, MoreRole);
        tracks->appendRow(next);
    }
}

void MpdLibrarySearchModel::loadAlbum(const QModelIndex& index) {
    ensureAlbum(index, [this](const QString& error) {
        if (!error.isEmpty()) {
            emit problem(error);
        }
    });
}

void MpdLibrarySearchModel::ensureAlbum(const QModelIndex& index,
                                        std::function<void(QString)> completion) {
    if (!actionable(index) || index.data(KindRole).toInt() != static_cast<int>(Kind::album) ||
        index.data(LoadedRole).toBool()) {
        completion({});
        return;
    }
    for (auto& pending : pending_) {
        if (pending.index == index) {
            pending.completions.push_back(std::move(completion));
            return;
        }
    }
    if (pending_.size() >= 64U) {
        completion(tr("Please wait for the current library requests."));
        return;
    }
    pending_.push_back({++next_token_, index, {std::move(completion)}});
    pump();
}

void MpdLibrarySearchModel::pump() {
    if (loading_ || pending_.empty()) {
        return;
    }
    const auto& request = pending_.front();
    const auto album = source_->albumAt(request.index.data(SourceRowRole).toInt());
    if (!request.index.isValid() || !album) {
        acceptAlbum(request.token, {}, tr("This album is no longer available"));
        return;
    }
    loading_ = true;
    emit albumRequested(request.token, *album);
}

void MpdLibrarySearchModel::acceptAlbum(const quint64 token, const std::vector<mpd::Track>& tracks,
                                        const QString& error) {
    if (pending_.empty() || pending_.front().token != token) {
        return;
    }
    auto request = std::move(pending_.front());
    pending_.pop_front();
    loading_ = false;
    auto* album = itemFromIndex(request.index);
    if (album) {
        album->removeRows(0, album->rowCount());
        if (error.isEmpty()) {
            auto ordered = tracks;
            mpd::sort_search_results(ordered);
            QStringList uris;
            for (const auto& track : ordered) {
                album->appendRow(trackItem(track));
                uris.push_back(QString::fromUtf8(track.uri));
            }
            album->setData(uris, UrisRole);
            album->setData(true, LoadedRole);
            album->setText(source_->index(album->data(SourceRowRole).toInt(), 1).data().toString() +
                           tr(" (%1)").arg(tracks.size()));
        }
        if (!album->rowCount()) {
            album->appendRow(placeholder(error.isEmpty() ? tr("No matches") : error));
        }
    }
    for (auto& done : request.completions) {
        done(error);
    }
    pump();
}

void MpdLibrarySearchModel::resolve(QModelIndexList indexes, Completion completion) {
    if (indexes.size() > 1'000) {
        completion({}, tr("Select at most 1,000 library entries."));
        return;
    }
    const auto position = [](QModelIndex index) {
        QList<int> rows;
        for (; index.isValid(); index = index.parent()) {
            rows.prepend(index.row());
        }
        return rows;
    };
    std::ranges::sort(indexes,
                      [&](const auto& a, const auto& b) { return position(a) < position(b); });
    auto result = std::make_shared<Resolution>();
    result->completion = std::move(completion);
    for (const auto& index : indexes) {
        if (!actionable(index)) {
            continue;
        }
        bool covered = false;
        for (auto parent = index.parent(); parent.isValid(); parent = parent.parent()) {
            covered = covered || (actionable(parent) && indexes.contains(parent));
        }
        if (!covered) {
            result->indexes.push_back(index);
        }
    }
    resolveNext(result);
}

void MpdLibrarySearchModel::resolveNext(const std::shared_ptr<Resolution>& result) {
    while (result->position < result->indexes.size()) {
        const auto index = result->indexes[result->position];
        if (!index.isValid()) {
            result->completion({}, tr("Search changed before the selection finished loading"));
            return;
        }
        if (index.data(KindRole).toInt() == static_cast<int>(Kind::album) &&
            !index.data(LoadedRole).toBool()) {
            ensureAlbum(index, [this, result](const QString& error) {
                if (!error.isEmpty()) {
                    result->completion({}, error);
                } else {
                    resolveNext(result);
                }
            });
            return;
        }
        for (const auto& uri : index.data(UrisRole).toStringList()) {
            if (!result->seen.contains(uri)) {
                result->seen.insert(uri);
                result->uris.push_back(uri);
                if (result->uris.size() > 4'096) {
                    result->completion({}, tr("Select at most 4096 MPD tracks."));
                    return;
                }
            }
        }
        ++result->position;
    }
    result->completion(std::move(result->uris), {});
}
} // namespace trackknife::bench
