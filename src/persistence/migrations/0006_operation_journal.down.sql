-- SPDX-License-Identifier: GPL-3.0-only

DROP TABLE operation_journal_intents;
DROP TABLE operation_journal_values;
DROP TABLE operation_journal_changes;
DROP TABLE operation_journal_occurrences;
DROP INDEX operation_journal_state;
DROP TABLE operation_journal;
UPDATE schema_version SET version = 5;
