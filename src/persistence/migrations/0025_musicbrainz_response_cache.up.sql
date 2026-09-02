-- SPDX-License-Identifier: GPL-3.0-only
-- Bounded MusicBrainz web-service response cache (ADR-0088). Rows are exact
-- response bodies keyed by request URL; age drives TTL expiry and pruning.
CREATE TABLE musicbrainz_response_cache (
    url BLOB PRIMARY KEY NOT NULL,
    body BLOB NOT NULL,
    fetched_at_unix_seconds INTEGER NOT NULL CHECK(fetched_at_unix_seconds >= 0));
CREATE INDEX musicbrainz_response_cache_age
    ON musicbrainz_response_cache(fetched_at_unix_seconds);
UPDATE schema_version SET version = 25;
