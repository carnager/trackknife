-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE metadata_transformation_chains (
    id TEXT PRIMARY KEY NOT NULL,
    schema_version INTEGER NOT NULL CHECK(schema_version = 1),
    name BLOB NOT NULL UNIQUE
);
CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL
        REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 9),
    target_field BLOB NOT NULL,
    argument BLOB,
    dialect BLOB,
    dialect_version INTEGER,
    compiler_schema INTEGER,
    PRIMARY KEY(chain_id, position)
);
CREATE TABLE metadata_transformation_action_values (
    chain_id TEXT NOT NULL,
    action_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(chain_id, action_position, position),
    FOREIGN KEY(chain_id, action_position)
        REFERENCES metadata_transformation_actions(chain_id, position)
        ON DELETE CASCADE
);
UPDATE schema_version SET version = 9;
