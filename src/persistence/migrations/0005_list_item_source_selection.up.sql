-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE list_items ADD COLUMN selected_audio_stream INTEGER;
ALTER TABLE list_items ADD COLUMN codec_subsong_index INTEGER;
UPDATE schema_version SET version = 5;
