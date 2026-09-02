// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_identify_dialog.hpp"

#include "trackknife/musicbrainz/proposal_bridge.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

[[nodiscard]] QString display_utf8(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] QString version_text(const musicbrainz::Release& release) {
    QStringList parts;
    for (const auto& part : {release.date, release.country, release.disambiguation, release.label,
                             release.catalog_number}) {
        if (!part.empty()) {
            parts.push_back(display_utf8(part));
        }
    }
    return parts.join(QStringLiteral(" · "));
}

[[nodiscard]] QString media_text(const musicbrainz::Release& release) {
    QStringList formats;
    for (const auto& medium : release.media) {
        if (!medium.format.empty()) {
            formats.push_back(display_utf8(medium.format));
        }
    }
    formats.removeDuplicates();
    auto text = formats.join(QStringLiteral(" + "));
    if (release.media.size() > 1U) {
        text += QStringLiteral(" × %1").arg(release.media.size());
    }
    return text;
}

[[nodiscard]] QString credit_text(const std::vector<musicbrainz::ArtistCredit>& credits) {
    std::string joined;
    for (const auto& credit : credits) {
        joined += credit.name;
        joined += credit.join_phrase;
    }
    return display_utf8(joined);
}

class MusicBrainzIdentifyDialog final : public QDialog {
  public:
    MusicBrainzIdentifyDialog(MusicBrainzLookupService service,
                              std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
                              std::vector<std::size_t> item_indexes, const QString& initial_artist,
                              const QString& initial_release,
                              std::function<void(metadata::MetadataProposalSet)> accepted,
                              QWidget* parent)
        : QDialog(parent), service_(std::move(service)), local_tracks_(std::move(local_tracks)),
          item_indexes_(std::move(item_indexes)), accepted_(std::move(accepted)) {
        setObjectName(QStringLiteral("bench-musicbrainz-identify"));
        setWindowTitle(QStringLiteral("Identify with MusicBrainz"));
        setWindowModality(Qt::WindowModal);
        setAttribute(Qt::WA_DeleteOnClose);
        setMinimumSize(640, 400);
        resize(940, 520);

        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        artist_ = new QLineEdit(initial_artist, this);
        artist_->setObjectName(QStringLiteral("bench-musicbrainz-identify-artist"));
        form->addRow(QStringLiteral("Artist:"), artist_);
        release_ = new QLineEdit(initial_release, this);
        release_->setObjectName(QStringLiteral("bench-musicbrainz-identify-release"));
        form->addRow(QStringLiteral("Album:"), release_);
        auto* form_row = new QHBoxLayout;
        form_row->addLayout(form, 1);
        search_ = new QPushButton(QStringLiteral("Search"), this);
        search_->setObjectName(QStringLiteral("bench-musicbrainz-identify-search"));
        search_->setDefault(true);
        form_row->addWidget(search_, 0, Qt::AlignBottom);
        layout->addLayout(form_row);

        status_ = new QLabel(
            QStringLiteral("Searches MusicBrainz by text — no MusicBrainz tags are needed"), this);
        status_->setObjectName(QStringLiteral("bench-musicbrainz-identify-status"));
        status_->setWordWrap(true);
        layout->addWidget(status_);

        results_ = new QTreeWidget(this);
        results_->setObjectName(QStringLiteral("bench-musicbrainz-identify-results"));
        results_->setAccessibleName(QStringLiteral("MusicBrainz release versions"));
        results_->setColumnCount(6);
        results_->setHeaderLabels({QStringLiteral("Match"), QStringLiteral("Album"),
                                   QStringLiteral("Artist"), QStringLiteral("Tracks"),
                                   QStringLiteral("Media"), QStringLiteral("Version")});
        results_->setRootIsDecorated(false);
        results_->setAlternatingRowColors(true);
        results_->setUniformRowHeights(true);
        results_->setTextElideMode(Qt::ElideRight);
        results_->setSelectionMode(QAbstractItemView::SingleSelection);
        results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
        results_->header()->setStretchLastSection(true);
        layout->addWidget(results_, 1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->setObjectName(QStringLiteral("bench-musicbrainz-identify-buttons"));
        use_ = buttons->addButton(QStringLiteral("Use this version"), QDialogButtonBox::ActionRole);
        use_->setObjectName(QStringLiteral("bench-musicbrainz-identify-use"));
        use_->setToolTip(QStringLiteral(
            "Match the selected files to this release and stage the result as colored draft "
            "edits — nothing is written until you apply"));
        use_->setEnabled(false);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(search_, &QPushButton::clicked, this, [this] { startSearch(); });
        connect(results_, &QTreeWidget::itemSelectionChanged, this,
                [this] { use_->setEnabled(!busy_ && results_->currentItem() != nullptr); });
        connect(results_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem*, int) { useSelected(); });
        connect(use_, &QPushButton::clicked, this, [this] { useSelected(); });
    }

  private:
    void setBusy(const bool busy) {
        busy_ = busy;
        search_->setEnabled(!busy);
        use_->setEnabled(!busy && results_->currentItem() != nullptr);
    }

    void startSearch() {
        if (busy_ || !service_.fetch) {
            return;
        }
        const auto url = musicbrainz::build_release_search_url(musicbrainz::ReleaseSearchQuery{
            .artist = artist_->text().trimmed().toStdString(),
            .release = release_->text().trimmed().toStdString(),
            .track_count = local_tracks_.empty() ? std::optional<std::size_t>{}
                                                 : std::optional{local_tracks_.size()},
            .limit = 25U,
        });
        if (!url) {
            status_->setText(QStringLiteral("Enter an artist or an album to search"));
            return;
        }
        setBusy(true);
        status_->setText(QStringLiteral("Searching MusicBrainz…"));
        results_->clear();
        candidates_.clear();
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
            if (!self.isNull()) {
                self->finishSearch(std::move(body));
            }
        });
    }

    void finishSearch(core::Result<QByteArray> body) {
        setBusy(false);
        if (!body) {
            status_->setText(QStringLiteral("Search failed · %1")
                                 .arg(QString::fromStdString(body.error().message)));
            return;
        }
        auto parsed = musicbrainz::parse_release_search(
            std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
        if (!parsed) {
            status_->setText(QStringLiteral("Search failed · %1")
                                 .arg(QString::fromStdString(parsed.error().message)));
            return;
        }
        const auto ranked = musicbrainz::rank_release_candidates(local_tracks_, *parsed);
        candidates_ = std::move(parsed->releases);
        for (const auto& entry : ranked) {
            const auto& release = candidates_[entry.release_index];
            auto* item = new QTreeWidgetItem(results_);
            item->setData(0, Qt::UserRole, static_cast<qulonglong>(entry.release_index));
            item->setText(0, QString::number(entry.score));
            item->setText(1, display_utf8(release.title));
            item->setText(2, credit_text(release.artist_credits));
            item->setText(3, QString::number(release.track_count));
            item->setText(4, media_text(release));
            item->setText(5, version_text(release));
            item->setToolTip(1, display_utf8(release.id));
            item->setToolTip(5, version_text(release));
        }
        if (candidates_.empty()) {
            status_->setText(QStringLiteral("No releases found — adjust the search text"));
        } else {
            status_->setText(
                QStringLiteral("%1 release %2 · every version of an album is its own row")
                    .arg(candidates_.size())
                    .arg(candidates_.size() == 1U ? QStringLiteral("version")
                                                  : QStringLiteral("versions")));
            results_->setCurrentItem(results_->topLevelItem(0));
        }
    }

    void useSelected() {
        if (busy_ || results_->currentItem() == nullptr || !service_.fetch) {
            return;
        }
        const auto index =
            static_cast<std::size_t>(results_->currentItem()->data(0, Qt::UserRole).toULongLong());
        if (index >= candidates_.size()) {
            return;
        }
        const auto url = musicbrainz::build_release_lookup_url(candidates_[index].id);
        if (!url) {
            status_->setText(QStringLiteral("This candidate has no usable release id"));
            return;
        }
        setBusy(true);
        status_->setText(QStringLiteral("Loading the release's track list…"));
        QPointer<MusicBrainzIdentifyDialog> self{this};
        service_.fetch(QString::fromStdString(*url), [self](core::Result<QByteArray> body) {
            if (!self.isNull()) {
                self->finishLookup(std::move(body));
            }
        });
    }

    void finishLookup(core::Result<QByteArray> body) {
        setBusy(false);
        if (!body) {
            status_->setText(QStringLiteral("Loading failed · %1")
                                 .arg(QString::fromStdString(body.error().message)));
            return;
        }
        auto release = musicbrainz::parse_release_lookup(
            std::string_view{body->constData(), static_cast<std::size_t>(body->size())});
        if (!release) {
            status_->setText(QStringLiteral("Loading failed · %1")
                                 .arg(QString::fromStdString(release.error().message)));
            return;
        }
        const auto alignment = musicbrainz::align_release_tracks(local_tracks_, *release);
        auto proposals =
            musicbrainz::release_metadata_proposals(*release, alignment, item_indexes_);
        if (!proposals) {
            status_->setText(QStringLiteral("Matching failed · %1")
                                 .arg(QString::fromStdString(proposals.error().message)));
            return;
        }
        if (proposals->items.empty()) {
            status_->setText(QStringLiteral(
                "No confident match between the selected files and this version — try another"));
            return;
        }
        if (accepted_) {
            accepted_(std::move(*proposals));
        }
        close();
    }

    MusicBrainzLookupService service_;
    std::vector<musicbrainz::LocalTrackDescriptor> local_tracks_;
    std::vector<std::size_t> item_indexes_;
    std::function<void(metadata::MetadataProposalSet)> accepted_;
    std::vector<musicbrainz::Release> candidates_;
    QLineEdit* artist_{nullptr};
    QLineEdit* release_{nullptr};
    QPushButton* search_{nullptr};
    QPushButton* use_{nullptr};
    QLabel* status_{nullptr};
    QTreeWidget* results_{nullptr};
    bool busy_{false};
};

} // namespace

QDialog* createMusicBrainzIdentifyDialog(
    MusicBrainzLookupService service, std::vector<musicbrainz::LocalTrackDescriptor> local_tracks,
    std::vector<std::size_t> item_indexes, const QString initial_artist,
    const QString initial_release, std::function<void(metadata::MetadataProposalSet)> accepted,
    QWidget* parent) {
    return new MusicBrainzIdentifyDialog(std::move(service), std::move(local_tracks),
                                         std::move(item_indexes), initial_artist, initial_release,
                                         std::move(accepted), parent);
}

} // namespace trackknife::bench
