-- SPDX-License-Identifier: GPL-3.0-only

-- Schema 23 cannot represent Add evidence. Development downgrades discard
-- those operation rows (and their cascaded retained-backup bookkeeping)
-- before restoring the older constraint.
DELETE FROM operation_journal
WHERE id IN (
    SELECT journal_id FROM operation_journal_artwork WHERE intent_kind = 2
);

ALTER TABLE operation_journal_artwork RENAME TO operation_journal_artwork_v24;

CREATE TABLE operation_journal_artwork (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES operation_journal(id) ON DELETE CASCADE,
    intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 1),
    target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0),
    original_item_count INTEGER NOT NULL CHECK(original_item_count > 0),
    planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0),
    original_target_fingerprint BLOB NOT NULL
        CHECK(length(original_target_fingerprint) = 32),
    replacement_fingerprint BLOB
        CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32),
    original_inventory_fingerprint BLOB NOT NULL
        CHECK(length(original_inventory_fingerprint) = 32),
    planned_inventory_fingerprint BLOB NOT NULL
        CHECK(length(planned_inventory_fingerprint) = 32),
    CHECK((intent_kind = 0 AND replacement_fingerprint IS NOT NULL AND
           planned_item_count = original_item_count) OR
          (intent_kind = 1 AND replacement_fingerprint IS NULL AND
           planned_item_count + 1 = original_item_count))
);

INSERT INTO operation_journal_artwork
SELECT * FROM operation_journal_artwork_v24;
DROP TABLE operation_journal_artwork_v24;
UPDATE schema_version SET version = 23;
