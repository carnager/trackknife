// SPDX-License-Identifier: GPL-3.0-only

#include "bench/musicbrainz_fetch_service.hpp"

#include "trackknife/musicbrainz/web_service.hpp"
#include "trackknife/persistence/musicbrainz_cache.hpp"

#include <QFutureWatcher>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>

namespace trackknife::bench {
namespace {

// MusicBrainz requires an identifying User-Agent naming the application.
const auto user_agent = QStringLiteral("Trackbench/0.0.1 ( https://trackknife.dev )");

[[nodiscard]] std::optional<QByteArray> load_cached(const std::filesystem::path& database_path,
                                                    const std::string& url) {
    auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_path);
    if (!cache) {
        return std::nullopt;
    }
    const auto loaded = cache->load(url, static_cast<std::int64_t>(std::time(nullptr)),
                                    musicbrainz::response_cache_ttl_seconds);
    if (!loaded || !*loaded) {
        return std::nullopt;
    }
    return QByteArray{reinterpret_cast<const char*>((*loaded)->data()),
                      static_cast<qsizetype>((*loaded)->size())};
}

void store_cached(const std::filesystem::path& database_path, const std::string& url,
                  const QByteArray& body) {
    auto cache = persistence::SqliteMusicBrainzResponseCache::open(database_path);
    if (!cache) {
        return;
    }
    static_cast<void>(cache->store(
        url, std::string_view{body.constData(), static_cast<std::size_t>(body.size())},
        static_cast<std::int64_t>(std::time(nullptr)), musicbrainz::response_cache_ttl_seconds,
        musicbrainz::response_cache_maximum_entries));
}

} // namespace

MusicBrainzFetchService::MusicBrainzFetchService(std::filesystem::path database_path,
                                                 QObject* parent)
    : QObject(parent), database_path_(std::move(database_path)),
      network_(new QNetworkAccessManager(this)),
      client_(new musicbrainz::MusicBrainzClient(
          musicbrainz::MusicBrainzClient::qtNetworkTransport(network_, user_agent), {},
          musicbrainz::minimum_request_interval_ms, this)) {}

MusicBrainzFetchService::~MusicBrainzFetchService() = default;

MusicBrainzLookupService MusicBrainzFetchService::lookupService() {
    QPointer<MusicBrainzFetchService> self{this};
    return MusicBrainzLookupService{
        .fetch =
            [self](const QString& url, std::function<void(core::Result<QByteArray>)> completion) {
                if (self.isNull()) {
                    completion(std::unexpected(core::Error{
                        .code = core::ErrorCode::cancelled,
                        .message = "MusicBrainz lookups are shutting down",
                        .context = {},
                    }));
                    return;
                }
                self->fetch(url, std::move(completion));
            },
    };
}

void MusicBrainzFetchService::fetch(const QString& url,
                                    std::function<void(core::Result<QByteArray>)> completion) {
    auto* watcher = new QFutureWatcher<std::optional<QByteArray>>(this);
    QPointer<MusicBrainzFetchService> self{this};
    connect(watcher, &QFutureWatcherBase::finished, this,
            [self, watcher, url, completion = std::move(completion)]() mutable {
                watcher->deleteLater();
                if (self.isNull()) {
                    completion(std::unexpected(core::Error{
                        .code = core::ErrorCode::cancelled,
                        .message = "MusicBrainz lookups are shutting down",
                        .context = {},
                    }));
                    return;
                }
                if (auto cached = watcher->result()) {
                    completion(std::move(*cached));
                    return;
                }
                const auto database_path = self->database_path_;
                self->client_->fetch(url, [database_path, url, completion = std::move(completion)](
                                              core::Result<QByteArray> body) {
                    if (body) {
                        // Fire-and-forget durable store off the UI thread.
                        auto stored = QtConcurrent::run(
                            [database_path, url = url.toStdString(), payload = *body] {
                                store_cached(database_path, url, payload);
                            });
                        static_cast<void>(stored);
                    }
                    completion(std::move(body));
                });
            });
    watcher->setFuture(
        QtConcurrent::run([database_path = database_path_, encoded = url.toStdString()] {
            return load_cached(database_path, encoded);
        }));
}

} // namespace trackknife::bench
