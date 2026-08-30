-- SPDX-License-Identifier: GPL-3.0-only

CREATE TEMP TABLE metadata_matching_numbering_downgrade_guard (
    value INTEGER NOT NULL CHECK(value = 0)
);
INSERT INTO metadata_matching_numbering_downgrade_guard
    SELECT COUNT(*) FROM metadata_transformation_actions
    WHERE kind BETWEEN 11 AND 13
       OR integer_argument IS NOT NULL
       OR integer_argument_2 IS NOT NULL;
DROP TABLE metadata_matching_numbering_downgrade_guard;

ALTER TABLE metadata_transformation_action_values
    RENAME TO metadata_transformation_action_values_v12;
ALTER TABLE metadata_transformation_actions
    RENAME TO metadata_transformation_actions_v12;

CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL
        REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 10),
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

INSERT INTO metadata_transformation_actions
    SELECT chain_id, position, kind, target_field, argument, dialect,
           dialect_version, compiler_schema
    FROM metadata_transformation_actions_v12;
INSERT INTO metadata_transformation_action_values
    SELECT * FROM metadata_transformation_action_values_v12;

DROP TABLE metadata_transformation_action_values_v12;
DROP TABLE metadata_transformation_actions_v12;
UPDATE schema_version SET version = 11;
