-- SPDX-License-Identifier: GPL-3.0-only

CREATE TABLE local_library_roots (
    raw_path BLOB PRIMARY KEY NOT NULL,
    available INTEGER NOT NULL DEFAULT 0 CHECK(available IN (0,1)),
    error TEXT NOT NULL DEFAULT '',
    scan_token TEXT NOT NULL DEFAULT ''
);
CREATE TABLE local_library_tracks (
    raw_path BLOB PRIMARY KEY NOT NULL,
    root BLOB NOT NULL REFERENCES local_library_roots(raw_path) ON DELETE CASCADE,
    revision TEXT NOT NULL,
    title TEXT NOT NULL,
    artist TEXT NOT NULL,
    album TEXT NOT NULL,
    album_key TEXT NOT NULL,
    release_id TEXT NOT NULL,
    date TEXT NOT NULL,
    disc INTEGER NOT NULL,
    track INTEGER NOT NULL,
    search_track TEXT NOT NULL,
    search_album TEXT NOT NULL,
    available INTEGER NOT NULL CHECK(available IN (0,1)),
    seen TEXT NOT NULL
);
CREATE INDEX local_library_artist ON local_library_tracks(artist, album_key, disc, track);
CREATE INDEX local_library_album ON local_library_tracks(album_key, disc, track);
CREATE INDEX local_library_root ON local_library_tracks(root, seen);
UPDATE schema_version SET version = 28;
