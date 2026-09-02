// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/musicbrainz_identify_dialog.hpp"
#include "trackknife/musicbrainz/client.hpp"

#include <QFutureWatcher>
#include <QObject>

#include <filesystem>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace trackknife::bench {

// Owns the paced ADR-0088 client and routes the durable SQLite response
// cache through worker threads, keeping every byte of SQL off the UI
// thread. Fetches stay explicit, serialized, and cache-first.
class MusicBrainzFetchService final : public QObject {
    Q_OBJECT

  public:
    MusicBrainzFetchService(std::filesystem::path database_path, QObject* parent = nullptr);
    ~MusicBrainzFetchService() override;

    [[nodiscard]] MusicBrainzLookupService lookupService();

  private:
    void fetch(const QString& url, std::function<void(core::Result<QByteArray>)> completion);

    std::filesystem::path database_path_;
    QNetworkAccessManager* network_{nullptr};
    musicbrainz::MusicBrainzClient* client_{nullptr};
};

} // namespace trackknife::bench
