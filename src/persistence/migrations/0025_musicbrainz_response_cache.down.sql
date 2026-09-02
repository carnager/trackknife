-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX musicbrainz_response_cache_age;
DROP TABLE musicbrainz_response_cache;
UPDATE schema_version SET version = 24;
