// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::musicbrainz {

// AcoustID asks for at most three requests per second per application; the
// bench fetcher paces below that.
inline constexpr int acoustid_minimum_request_interval_ms = 400;
inline constexpr std::string_view acoustid_lookup_url = "https://api.acoustid.org/v2/lookup";

struct AcoustIdRecording {
    std::string id;
    std::vector<std::string> release_ids;

    friend bool operator==(const AcoustIdRecording&, const AcoustIdRecording&) = default;
};

struct AcoustIdResult {
    double score{0.0};
    std::vector<AcoustIdRecording> recordings;

    friend bool operator==(const AcoustIdResult&, const AcoustIdResult&) = default;
};

struct AcoustIdLookup {
    std::vector<AcoustIdResult> results;

    friend bool operator==(const AcoustIdLookup&, const AcoustIdLookup&) = default;
};

// Builds the form-encoded POST body for one fingerprint lookup, requesting
// recordings with their releases. The client key, a positive duration, and
// a nonempty chromaprint fingerprint are all required.
[[nodiscard]] core::Result<std::string> build_acoustid_lookup_body(std::string_view client_key,
                                                                   std::size_t duration_seconds,
                                                                   std::string_view fingerprint);

// Parses an AcoustID lookup response. A non-ok status becomes a typed
// backend error carrying the service's message; recording and release ids
// survive only when they are valid MusicBrainz UUIDs.
[[nodiscard]] core::Result<AcoustIdLookup>
parse_acoustid_lookup(std::string_view body, const WebServiceLimits& limits = {});

} // namespace trackknife::musicbrainz
