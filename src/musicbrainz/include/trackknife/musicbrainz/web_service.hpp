// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::musicbrainz {

// MusicBrainz asks for at most one request per second per client plus an
// identifying User-Agent; the client paces slightly slower than required.
inline constexpr int minimum_request_interval_ms = 1'100;
// Release data changes slowly; cached web-service responses stay valid for
// two weeks and the cache stays bounded.
inline constexpr std::int64_t response_cache_ttl_seconds = 14LL * 24 * 60 * 60;
inline constexpr std::size_t response_cache_maximum_entries = 10'000U;

struct WebServiceLimits {
    std::size_t releases{25U};
    std::size_t media{64U};
    std::size_t tracks_per_medium{256U};
    std::size_t artist_credits{32U};
    std::size_t text_bytes{2'048U};
    std::size_t body_bytes{8U * 1024U * 1024U};
};

struct ReleaseSearchQuery {
    std::string artist;
    std::string release;
    std::optional<std::size_t> track_count;
    std::size_t limit{8U};
};

// Builds the ws/2 release-search URL with Lucene phrase escaping and URL
// encoding. At least one of artist/release must be non-empty.
[[nodiscard]] core::Result<std::string> build_release_search_url(const ReleaseSearchQuery& query);

// Builds the ws/2 release lookup URL including artist credits, recordings,
// release groups, and labels. The id must be a MusicBrainz UUID.
[[nodiscard]] core::Result<std::string> build_release_lookup_url(std::string_view release_id);

struct ArtistCredit {
    std::string name;
    std::string join_phrase;
    std::string artist_id;
    std::string sort_name;

    friend bool operator==(const ArtistCredit&, const ArtistCredit&) = default;
};

struct ReleaseTrack {
    std::string track_id;
    std::string recording_id;
    std::size_t position{0U};
    std::string number;
    std::string title;
    std::optional<std::int64_t> length_ms;
    std::vector<ArtistCredit> artist_credits;

    friend bool operator==(const ReleaseTrack&, const ReleaseTrack&) = default;
};

struct ReleaseMedium {
    std::size_t position{0U};
    std::string format;
    std::size_t track_count{0U};
    std::vector<ReleaseTrack> tracks;

    friend bool operator==(const ReleaseMedium&, const ReleaseMedium&) = default;
};

struct Release {
    std::string id;
    std::string title;
    std::string status;
    std::string disambiguation;
    std::string date;
    std::string country;
    std::string barcode;
    std::string release_group_id;
    std::string label;
    std::string catalog_number;
    int search_score{0};
    std::size_t track_count{0U};
    std::vector<ArtistCredit> artist_credits;
    std::vector<ReleaseMedium> media;

    friend bool operator==(const Release&, const Release&) = default;
};

struct ReleaseSearchResult {
    std::size_t total_count{0U};
    std::vector<Release> releases;

    friend bool operator==(const ReleaseSearchResult&, const ReleaseSearchResult&) = default;
};

// Parses a ws/2 fmt=json release-search payload. Malformed JSON, missing
// release identities, and exceeded limits fail typed; optional detail that
// is absent or mistyped is dropped, never invented.
[[nodiscard]] core::Result<ReleaseSearchResult>
parse_release_search(std::string_view body, const WebServiceLimits& limits = {});

// Parses a ws/2 fmt=json release lookup including per-medium track listings.
[[nodiscard]] core::Result<Release> parse_release_lookup(std::string_view body,
                                                         const WebServiceLimits& limits = {});

} // namespace trackknife::musicbrainz
