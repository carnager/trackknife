-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE list_documents (
    id TEXT PRIMARY KEY NOT NULL,
    kind INTEGER NOT NULL,
    name BLOB NOT NULL,
    pinned INTEGER NOT NULL CHECK(pinned IN (0,1)),
    dirty INTEGER NOT NULL CHECK(dirty IN (0,1)),
    position INTEGER NOT NULL UNIQUE
);
CREATE TABLE list_items (
    document_id TEXT NOT NULL REFERENCES list_documents(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    source INTEGER NOT NULL,
    profile_id TEXT,
    source_reference BLOB NOT NULL,
    duration_ms INTEGER,
    PRIMARY KEY(document_id, position)
);
CREATE TABLE list_item_fields (
    document_id TEXT NOT NULL,
    item_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    name BLOB NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(document_id, item_position, position),
    FOREIGN KEY(document_id, item_position)
        REFERENCES list_items(document_id, position) ON DELETE CASCADE
);
CREATE INDEX list_item_fields_item
    ON list_item_fields(document_id, item_position, position);
UPDATE schema_version SET version = 1;
