-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE track_view_presets (
    binding BLOB PRIMARY KEY NOT NULL,
    header_state BLOB NOT NULL,
    position INTEGER NOT NULL UNIQUE
);
UPDATE schema_version SET version = 3;
