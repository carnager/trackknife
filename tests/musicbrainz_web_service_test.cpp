// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/web_service.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

using namespace trackknife::musicbrainz;

void searchUrlEscapesLuceneAndPercentEncodes() {
    const auto url = build_release_search_url(ReleaseSearchQuery{
        .artist = "AC/DC \"Live\"",
        .release = "Back in Black",
        .track_count = 10U,
        .limit = 5U,
    });
    CHECK(url.has_value());
    CHECK(url->starts_with("https://musicbrainz.org/ws/2/release/?query="));
    CHECK(url->ends_with("&fmt=json&limit=5"));
    // The quote inside the artist phrase is Lucene-escaped, then everything
    // is percent-encoded: \" becomes %5C%22.
    CHECK(url->find("%5C%22Live%5C%22") != std::string::npos);
    CHECK(url->find("tracks%3A10") != std::string::npos);
    CHECK(url->find(' ') == std::string::npos);

    CHECK(!build_release_search_url(ReleaseSearchQuery{}).has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 0U})
               .has_value());
    CHECK(!build_release_search_url(
               ReleaseSearchQuery{.artist = "x", .release = {}, .track_count = {}, .limit = 999U})
               .has_value());
}

void lookupUrlRequiresAReleaseId() {
    const auto url = build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789abc");
    CHECK(url.has_value());
    CHECK(*url == "https://musicbrainz.org/ws/2/release/2f2ac1b7-1111-4f4f-8f8f-123456789abc"
                  "?inc=artist-credits+recordings+release-groups+labels&fmt=json");
    CHECK(!build_release_lookup_url("").has_value());
    CHECK(!build_release_lookup_url("not-a-uuid").has_value());
    CHECK(!build_release_lookup_url("2f2ac1b7-1111-4f4f-8f8f-123456789ab/../x").has_value());
}

constexpr std::string_view search_fixture = R"json({
  "count": 42,
  "offset": 0,
  "releases": [
    {
      "id": "11111111-2222-3333-4444-555555555555",
      "score": 100,
      "title": "Alpha",
      "status": "Official",
      "disambiguation": "remastered",
      "date": "1999-09-09",
      "country": "DE",
      "barcode": "0123456789012",
      "track-count": 10,
      "artist-credit": [
        {"name": "Band", "joinphrase": " feat. ",
         "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                    "name": "Band", "sort-name": "Band, The"}},
        {"name": "Guest", "artist": {"id": "ffffffff-0000-1111-2222-333333333333",
                                     "name": "Guest", "sort-name": "Guest"}}
      ],
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "label-info": [
        {"catalog-number": "CAT-123", "label": {"name": "Label Records"}}
      ],
      "media": [{"format": "CD", "track-count": 10}]
    },
    {
      "id": "21111111-2222-3333-4444-555555555555",
      "score": 87,
      "title": "Alpha",
      "status": "Official",
      "date": "2001-01-01",
      "country": "US",
      "track-count": 11,
      "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
      "media": [{"format": "12\" Vinyl", "track-count": 11}]
    }
  ]
})json";

void searchParsingCarriesVersionDetail() {
    const auto result = parse_release_search(search_fixture);
    CHECK(result.has_value());
    if (!result) {
        return;
    }
    CHECK(result->total_count == 42U);
    CHECK(result->releases.size() == 2U);
    const auto& first = result->releases.front();
    CHECK(first.id == "11111111-2222-3333-4444-555555555555");
    CHECK(first.title == "Alpha");
    CHECK(first.search_score == 100);
    CHECK(first.disambiguation == "remastered");
    CHECK(first.date == "1999-09-09");
    CHECK(first.country == "DE");
    CHECK(first.barcode == "0123456789012");
    CHECK(first.release_group_id == "99999999-8888-7777-6666-555555555555");
    CHECK(first.label == "Label Records");
    CHECK(first.catalog_number == "CAT-123");
    CHECK(first.track_count == 10U);
    CHECK(first.artist_credits.size() == 2U);
    CHECK(first.artist_credits[0].name == "Band");
    CHECK(first.artist_credits[0].join_phrase == " feat. ");
    CHECK(first.artist_credits[0].artist_id == "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    CHECK(first.artist_credits[0].sort_name == "Band, The");
    CHECK(first.media.size() == 1U && first.media.front().format == "CD");
    // Two versions of the same release group stay distinct candidates.
    const auto& second = result->releases.back();
    CHECK(second.release_group_id == first.release_group_id);
    CHECK(second.country == "US" && second.media.front().format == "12\" Vinyl");
}

constexpr std::string_view lookup_fixture = R"json({
  "id": "11111111-2222-3333-4444-555555555555",
  "title": "Alpha",
  "status": "Official",
  "date": "1999-09-09",
  "country": "DE",
  "release-group": {"id": "99999999-8888-7777-6666-555555555555"},
  "artist-credit": [{"name": "Band",
                     "artist": {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                                "name": "Band", "sort-name": "Band, The"}}],
  "media": [
    {
      "position": 1,
      "format": "CD",
      "track-count": 2,
      "tracks": [
        {"id": "aaaa1111-0000-0000-0000-000000000001", "position": 1, "number": "1",
         "title": "One", "length": 61000,
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000001", "title": "One"}},
        {"id": "aaaa1111-0000-0000-0000-000000000002", "position": 2, "number": "2",
         "recording": {"id": "bbbb1111-0000-0000-0000-000000000002",
                       "title": "Two (recording)", "length": 59000}}
      ]
    }
  ]
})json";

void lookupParsingAlignsTracksAndRecordings() {
    const auto release = parse_release_lookup(lookup_fixture);
    CHECK(release.has_value());
    if (!release) {
        return;
    }
    CHECK(release->media.size() == 1U);
    const auto& medium = release->media.front();
    CHECK(medium.position == 1U && medium.format == "CD");
    CHECK(medium.tracks.size() == 2U);
    CHECK(medium.tracks[0].position == 1U && medium.tracks[0].title == "One");
    CHECK(medium.tracks[0].length_ms == std::optional<std::int64_t>{61'000});
    CHECK(medium.tracks[0].recording_id == "bbbb1111-0000-0000-0000-000000000001");
    // Track title/length fall back to the recording when absent.
    CHECK(medium.tracks[1].title == "Two (recording)");
    CHECK(medium.tracks[1].length_ms == std::optional<std::int64_t>{59'000});
    CHECK(release->track_count == 2U);
}

void malformedAndOversizedPayloadsFailTyped() {
    CHECK(!parse_release_search("not json").has_value());
    CHECK(!parse_release_search("[1,2,3]").has_value());
    CHECK(!parse_release_search(R"({"count": 1})").has_value());
    CHECK(!parse_release_search(R"({"count": 1, "releases": [{"title": "No id"}]})").has_value());
    WebServiceLimits tiny;
    tiny.body_bytes = 8U;
    const auto oversized = parse_release_search(search_fixture, tiny);
    CHECK(!oversized.has_value() &&
          oversized.error().code == trackknife::core::ErrorCode::limit_exceeded);
}

} // namespace

int main() {
    searchUrlEscapesLuceneAndPercentEncodes();
    lookupUrlRequiresAReleaseId();
    searchParsingCarriesVersionDetail();
    lookupParsingAlignsTracksAndRecordings();
    malformedAndOversizedPayloadsFailTyped();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
