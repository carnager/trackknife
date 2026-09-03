-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE encoder_presets (
    id TEXT PRIMARY KEY NOT NULL,
    preset_id BLOB NOT NULL,
    preset_version INTEGER NOT NULL CHECK(preset_version >= 1),
    display_name BLOB NOT NULL UNIQUE,
    codec_name BLOB NOT NULL,
    container_name BLOB NOT NULL,
    file_extension BLOB NOT NULL,
    lossless INTEGER NOT NULL CHECK(lossless IN (0, 1)),
    bit_rate INTEGER,
    vbr_quality INTEGER,
    sample_format_hint BLOB NOT NULL
);

UPDATE schema_version SET version = 27;
