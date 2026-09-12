-- SPDX-License-Identifier: GPL-3.0-only
DROP INDEX local_library_fields_lookup;
DROP TABLE local_library_fields;
ALTER TABLE local_library_tracks DROP COLUMN codec_name;
ALTER TABLE local_library_tracks DROP COLUMN sample_rate;
ALTER TABLE local_library_tracks DROP COLUMN bits;
ALTER TABLE local_library_tracks DROP COLUMN channels;
ALTER TABLE local_library_tracks DROP COLUMN duration_ms;
UPDATE schema_version SET version = 29;
