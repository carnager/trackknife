-- SPDX-License-Identifier: GPL-3.0-only
-- ADR-0150: query substrate for the tkq-1 dialect. Every tag value
-- becomes queryable (original bytes beside the normalized form), and
-- the technical properties the scan already probes are retained as
-- typed columns. DDL-only: existing rows fill on their next Refresh.
CREATE TABLE local_library_fields (
    raw_path BLOB NOT NULL
        REFERENCES local_library_tracks(raw_path) ON DELETE CASCADE,
    canonical_name TEXT NOT NULL,
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    value_lower BLOB NOT NULL,
    PRIMARY KEY (raw_path, canonical_name, position)
);
CREATE INDEX local_library_fields_lookup
    ON local_library_fields(canonical_name, value_lower);
ALTER TABLE local_library_tracks ADD COLUMN codec_name TEXT NOT NULL DEFAULT '';
ALTER TABLE local_library_tracks ADD COLUMN sample_rate INTEGER NOT NULL DEFAULT 0;
ALTER TABLE local_library_tracks ADD COLUMN bits INTEGER NOT NULL DEFAULT 0;
ALTER TABLE local_library_tracks ADD COLUMN channels INTEGER NOT NULL DEFAULT 0;
ALTER TABLE local_library_tracks ADD COLUMN duration_ms INTEGER NOT NULL DEFAULT -1;
UPDATE schema_version SET version = 30;
