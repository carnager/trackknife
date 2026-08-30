-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE output_layout_profiles (
    id TEXT PRIMARY KEY NOT NULL,
    schema_version INTEGER NOT NULL CHECK(schema_version = 1),
    name BLOB NOT NULL UNIQUE,
    dialect BLOB NOT NULL,
    dialect_version INTEGER NOT NULL,
    compiler_schema INTEGER NOT NULL,
    relative_directory_expression BLOB NOT NULL,
    basename_expression BLOB NOT NULL,
    sanitization_policy BLOB NOT NULL,
    sanitization_version INTEGER NOT NULL
);

CREATE TABLE destination_profiles (
    id TEXT PRIMARY KEY NOT NULL,
    schema_version INTEGER NOT NULL CHECK(schema_version = 1),
    name BLOB NOT NULL UNIQUE,
    root_raw_path BLOB NOT NULL,
    containment_policy BLOB NOT NULL,
    containment_version INTEGER NOT NULL
);

UPDATE schema_version SET version = 13;
