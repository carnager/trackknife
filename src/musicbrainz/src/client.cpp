// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/client.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace trackknife::musicbrainz {
namespace {

[[nodiscard]] core::Error client_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

MusicBrainzClient::MusicBrainzClient(WebTransport transport, ResponseCacheHooks cache,
                                     const int minimum_interval_ms, QObject* parent)
    : QObject(parent), transport_(std::move(transport)), cache_(std::move(cache)),
      minimum_interval_ms_(std::max(minimum_interval_ms, 0)) {}

std::size_t MusicBrainzClient::pending_request_count() const noexcept {
    return pending_.size() + (in_flight_ ? 1U : 0U);
}

void MusicBrainzClient::fetch(const QString& url, Completion completion) {
    if (!completion) {
        return;
    }
    if (!transport_) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] {
            completion(std::unexpected(
                client_error(core::ErrorCode::unsupported,
                             "the MusicBrainz client has no transport configured")));
        });
        return;
    }
    if (cache_.load) {
        if (auto cached = cache_.load(url)) {
            QTimer::singleShot(0, this,
                               [completion = std::move(completion), body = std::move(*cached)] {
                                   completion(body);
                               });
            return;
        }
    }
    pending_.push_back(Pending{.url = url, .completion = std::move(completion)});
    scheduleDispatch();
}

void MusicBrainzClient::scheduleDispatch() {
    if (dispatch_scheduled_ || in_flight_ || pending_.empty()) {
        return;
    }
    const auto elapsed = dispatched_once_ ? since_last_dispatch_.elapsed() : minimum_interval_ms_;
    const auto wait = std::max<qint64>(0, minimum_interval_ms_ - elapsed);
    dispatch_scheduled_ = true;
    QTimer::singleShot(static_cast<int>(wait), this, &MusicBrainzClient::dispatchNext);
}

void MusicBrainzClient::dispatchNext() {
    dispatch_scheduled_ = false;
    if (in_flight_ || pending_.empty()) {
        return;
    }
    auto request = std::move(pending_.front());
    pending_.pop_front();
    in_flight_ = true;
    dispatched_once_ = true;
    since_last_dispatch_.restart();
    QPointer<MusicBrainzClient> self{this};
    transport_(QUrl{request.url},
               [self, url = request.url,
                completion = std::move(request.completion)](const WebResponse& response) {
                   if (self.isNull()) {
                       return;
                   }
                   self->finishRequest(url, completion, response);
               });
}

void MusicBrainzClient::finishRequest(const QString& url, Completion completion,
                                      const WebResponse& response) {
    in_flight_ = false;
    scheduleDispatch();
    if (!response.transport_error.isEmpty()) {
        completion(std::unexpected(
            client_error(core::ErrorCode::io,
                         "MusicBrainz is unreachable: " + response.transport_error.toStdString())));
        return;
    }
    if (response.status_code == 503) {
        completion(std::unexpected(client_error(
            core::ErrorCode::backend, "MusicBrainz asked to slow down; try again shortly")));
        return;
    }
    if (response.status_code == 404) {
        completion(std::unexpected(
            client_error(core::ErrorCode::not_found, "MusicBrainz has no such entity")));
        return;
    }
    if (response.status_code != 200 || response.body.isEmpty()) {
        completion(std::unexpected(client_error(
            core::ErrorCode::backend, "MusicBrainz returned an unexpected response (status " +
                                          std::to_string(response.status_code) + ")")));
        return;
    }
    if (cache_.store) {
        cache_.store(url, response.body);
    }
    completion(response.body);
}

WebTransport MusicBrainzClient::qtNetworkTransport(QNetworkAccessManager* manager,
                                                   const QString& user_agent) {
    QPointer<QNetworkAccessManager> guarded{manager};
    return [guarded, user_agent](const QUrl& url, std::function<void(WebResponse)> completion) {
        if (guarded.isNull()) {
            completion(WebResponse{.status_code = 0,
                                   .body = {},
                                   .transport_error = QStringLiteral("network manager is gone")});
            return;
        }
        QNetworkRequest request{url};
        request.setHeader(QNetworkRequest::UserAgentHeader, user_agent);
        // Cover Art Archive image URLs redirect cross-origin to the
        // Internet Archive; refuse only scheme downgrades.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = guarded->get(request);
        QObject::connect(
            reply, &QNetworkReply::finished, reply, [reply, completion = std::move(completion)] {
                const auto status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                WebResponse response{
                    .status_code = status,
                    .body = reply->readAll(),
                    .transport_error =
                        reply->error() == QNetworkReply::NoError ? QString{} : reply->errorString(),
                };
                // HTTP-level failures carry a status; keep them
                // out of the transport-error channel so the
                // client maps them precisely.
                if (status != 0 && reply->error() != QNetworkReply::NoError &&
                    reply->error() != QNetworkReply::OperationCanceledError) {
                    response.transport_error.clear();
                }
                reply->deleteLater();
                completion(response);
            });
    };
}

} // namespace trackknife::musicbrainz
