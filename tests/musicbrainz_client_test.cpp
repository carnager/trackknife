// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/client.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

namespace trackknife::musicbrainz {

class MusicBrainzClientTest final : public QObject {
    Q_OBJECT

  private slots:
    void cacheHitAnswersWithoutTransport();
    void requestsAreSerializedAndPaced();
    void failureStatesAreTyped();
    void successStoresIntoTheCache();
};

void MusicBrainzClientTest::cacheHitAnswersWithoutTransport() {
    int transport_calls = 0;
    MusicBrainzClient client{
        [&transport_calls](const QUrl&, std::function<void(WebResponse)> completion) {
            ++transport_calls;
            completion(WebResponse{.status_code = 200, .body = "live", .transport_error = {}});
        },
        ResponseCacheHooks{
            .load = [](const QString&) { return std::optional<QByteArray>{"cached"}; },
            .store = {},
        },
        0};
    std::optional<core::Result<QByteArray>> delivered;
    client.fetch(QStringLiteral("https://musicbrainz.org/ws/2/release/?query=x"),
                 [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
    QTRY_VERIFY(delivered.has_value());
    QVERIFY(delivered->has_value());
    QCOMPARE(**delivered, QByteArray{"cached"});
    QCOMPARE(transport_calls, 0);
}

void MusicBrainzClientTest::requestsAreSerializedAndPaced() {
    struct Dispatch {
        qint64 at_ms{0};
        std::function<void(WebResponse)> completion;
    };
    QElapsedTimer clock;
    clock.start();
    std::vector<Dispatch> dispatches;
    MusicBrainzClient client{
        [&clock, &dispatches](const QUrl&, std::function<void(WebResponse)> completion) {
            dispatches.push_back(
                Dispatch{.at_ms = clock.elapsed(), .completion = std::move(completion)});
        },
        {},
        120};
    int completed = 0;
    const auto completion = [&completed](core::Result<QByteArray> result) {
        QVERIFY(result.has_value());
        ++completed;
    };
    client.fetch(QStringLiteral("https://musicbrainz.org/one"), completion);
    client.fetch(QStringLiteral("https://musicbrainz.org/two"), completion);
    QCOMPARE(client.pending_request_count(), std::size_t{2U});

    // Only one request is in flight; the second waits for the first response
    // AND the pacing interval.
    QTRY_COMPARE(dispatches.size(), std::size_t{1U});
    QTest::qWait(200);
    QCOMPARE(dispatches.size(), std::size_t{1U});
    dispatches.front().completion(
        WebResponse{.status_code = 200, .body = "one", .transport_error = {}});
    QTRY_COMPARE(dispatches.size(), std::size_t{2U});
    QVERIFY(dispatches[1].at_ms - dispatches[0].at_ms >= 120);
    dispatches.back().completion(
        WebResponse{.status_code = 200, .body = "two", .transport_error = {}});
    QTRY_COMPARE(completed, 2);
    QCOMPARE(client.pending_request_count(), std::size_t{0U});
}

void MusicBrainzClientTest::failureStatesAreTyped() {
    WebResponse scripted;
    MusicBrainzClient client{[&scripted](const QUrl&, std::function<void(WebResponse)> completion) {
                                 completion(scripted);
                             },
                             {},
                             0};
    const auto fetch_error = [&client](const QString& url) {
        std::optional<core::Result<QByteArray>> delivered;
        client.fetch(
            url, [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
        [&] { QTRY_VERIFY(delivered.has_value()); }();
        return delivered->has_value() ? std::optional<core::Error>{}
                                      : std::optional{delivered->error()};
    };

    scripted = WebResponse{
        .status_code = 0, .body = {}, .transport_error = QStringLiteral("no route to host")};
    auto offline = fetch_error(QStringLiteral("https://musicbrainz.org/a"));
    QVERIFY(offline && offline->code == core::ErrorCode::io);

    scripted = WebResponse{.status_code = 503, .body = "busy", .transport_error = {}};
    auto throttled = fetch_error(QStringLiteral("https://musicbrainz.org/b"));
    QVERIFY(throttled && throttled->code == core::ErrorCode::backend);

    scripted = WebResponse{.status_code = 404, .body = {}, .transport_error = {}};
    auto missing = fetch_error(QStringLiteral("https://musicbrainz.org/c"));
    QVERIFY(missing && missing->code == core::ErrorCode::not_found);

    scripted = WebResponse{.status_code = 500, .body = "oops", .transport_error = {}};
    auto server = fetch_error(QStringLiteral("https://musicbrainz.org/d"));
    QVERIFY(server && server->code == core::ErrorCode::backend);
}

void MusicBrainzClientTest::successStoresIntoTheCache() {
    QString stored_url;
    QByteArray stored_body;
    MusicBrainzClient client{
        [](const QUrl&, std::function<void(WebResponse)> completion) {
            completion(WebResponse{.status_code = 200, .body = "payload", .transport_error = {}});
        },
        ResponseCacheHooks{
            .load = [](const QString&) { return std::optional<QByteArray>{}; },
            .store =
                [&stored_url, &stored_body](const QString& url, const QByteArray& body) {
                    stored_url = url;
                    stored_body = body;
                },
        },
        0};
    std::optional<core::Result<QByteArray>> delivered;
    client.fetch(QStringLiteral("https://musicbrainz.org/ws/2/release/x"),
                 [&delivered](core::Result<QByteArray> result) { delivered = std::move(result); });
    QTRY_VERIFY(delivered.has_value());
    QVERIFY(delivered->has_value());
    QCOMPARE(stored_url, QStringLiteral("https://musicbrainz.org/ws/2/release/x"));
    QCOMPARE(stored_body, QByteArray{"payload"});
}

} // namespace trackknife::musicbrainz

QTEST_GUILESS_MAIN(trackknife::musicbrainz::MusicBrainzClientTest)
#include "musicbrainz_client_test.moc"
