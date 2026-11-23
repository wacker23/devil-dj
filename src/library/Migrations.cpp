#include "Migrations.h"

#include "Connection.h"

#include <array>
#include <string>

namespace devildj::library
{

namespace
{
// v1: ARCHITECTURE §7, plus the triggers that keep tracks_fts in sync with tracks (§6).
constexpr std::string_view kSchemaV1 = R"sql(
CREATE TABLE tracks (
  id INTEGER PRIMARY KEY,
  path TEXT NOT NULL UNIQUE,
  content_hash TEXT NOT NULL,
  title TEXT, artist TEXT, album TEXT, genre TEXT, year INTEGER, comment TEXT,
  duration_ms INTEGER, sample_rate INTEGER, bitrate INTEGER, file_size INTEGER,
  bpm REAL, bpm_locked INTEGER DEFAULT 0,
  musical_key INTEGER,
  key_confidence REAL,
  loudness_lufs REAL, auto_gain_db REAL,
  rating INTEGER DEFAULT 0, color INTEGER,
  play_count INTEGER DEFAULT 0, last_played INTEGER,
  date_added INTEGER NOT NULL,
  analysis_version INTEGER DEFAULT 0,
  missing INTEGER DEFAULT 0
);
CREATE INDEX idx_tracks_bpm ON tracks(bpm);
CREATE INDEX idx_tracks_key ON tracks(musical_key);

CREATE TABLE beatgrids (
  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
  anchors_json TEXT NOT NULL,
  locked INTEGER DEFAULT 0
);

CREATE TABLE cues (
  id INTEGER PRIMARY KEY,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  kind INTEGER NOT NULL,
  slot INTEGER,
  start_sample INTEGER NOT NULL, length_samples INTEGER DEFAULT 0,
  label TEXT, color INTEGER
);
CREATE INDEX idx_cues_track ON cues(track_id);

CREATE TABLE playlists (
  id INTEGER PRIMARY KEY,
  parent_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  kind INTEGER NOT NULL,
  rule_json TEXT,
  sort_order INTEGER DEFAULT 0,
  created_at INTEGER NOT NULL
);

CREATE TABLE playlist_tracks (
  playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
  position INTEGER NOT NULL,
  PRIMARY KEY (playlist_id, position)
);

CREATE TABLE tag_categories (id INTEGER PRIMARY KEY, name TEXT NOT NULL, sort_order INTEGER);
CREATE TABLE tags (id INTEGER PRIMARY KEY, category_id INTEGER REFERENCES tag_categories(id), name TEXT NOT NULL);
CREATE TABLE track_tags (track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
                         tag_id INTEGER REFERENCES tags(id) ON DELETE CASCADE,
                         PRIMARY KEY (track_id, tag_id));

CREATE TABLE history (id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL,
                      track_id INTEGER REFERENCES tracks(id), played_at INTEGER NOT NULL, deck INTEGER,
                      previous_track_id INTEGER REFERENCES tracks(id));

CREATE TABLE analysis_artifacts (
  track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,
  path TEXT NOT NULL, version INTEGER NOT NULL,
  PRIMARY KEY (track_id, kind)
);

CREATE VIRTUAL TABLE tracks_fts USING fts5(title, artist, album, comment, content='tracks', content_rowid='id');

CREATE TRIGGER tracks_fts_insert AFTER INSERT ON tracks BEGIN
  INSERT INTO tracks_fts(rowid, title, artist, album, comment)
  VALUES (new.id, new.title, new.artist, new.album, new.comment);
END;
CREATE TRIGGER tracks_fts_delete AFTER DELETE ON tracks BEGIN
  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, comment)
  VALUES ('delete', old.id, old.title, old.artist, old.album, old.comment);
END;
CREATE TRIGGER tracks_fts_update AFTER UPDATE OF title, artist, album, comment ON tracks BEGIN
  INSERT INTO tracks_fts(tracks_fts, rowid, title, artist, album, comment)
  VALUES ('delete', old.id, old.title, old.artist, old.album, old.comment);
  INSERT INTO tracks_fts(rowid, title, artist, album, comment)
  VALUES (new.id, new.title, new.artist, new.album, new.comment);
END;
)sql";

// Schema v2 (P2-24): the Track Matcher's tables (MATCHER_SPEC §11). track_features.energy_features keeps each
// 16-bar window's raw inputs (6 x float32: start, length, loudness dB, onset rate, low ratio, centroid Hz) so the
// library-percentile energy can be recomputed after imports; feature_stats holds the library statistics.
constexpr std::string_view kSchemaV2 = R"sql(
CREATE TABLE track_features (
  track_id INTEGER PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
  version INTEGER NOT NULL,
  energy REAL, energy_intro REAL, energy_outro REAL,
  energy_curve BLOB,
  energy_features BLOB,
  sound_v1 BLOB,
  groove BLOB,
  vocal_intro REAL, vocal_outro REAL,
  intro_beats INTEGER, outro_beats INTEGER
);

CREATE TABLE track_embeddings (
  track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE,
  model TEXT NOT NULL, segment TEXT NOT NULL,
  dim INTEGER NOT NULL, vec BLOB NOT NULL,
  PRIMARY KEY (track_id, model, segment)
);

CREATE TABLE choice_log (
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL, created_at INTEGER NOT NULL,
  context_track INTEGER, chosen_track INTEGER,
  intent INTEGER, preset TEXT, scope TEXT,
  shown_json TEXT NOT NULL,
  chosen_rank INTEGER,
  outcome INTEGER,
  model_version TEXT
);

CREATE TABLE matcher_models (
  name TEXT PRIMARY KEY, version INTEGER NOT NULL,
  weights_json TEXT NOT NULL, trained_at INTEGER, n_impressions INTEGER
);

CREATE TABLE track_neighbors (
  track_id INTEGER, neighbor_id INTEGER, score REAL,
  PRIMARY KEY (track_id, neighbor_id)
);

CREATE TABLE feature_stats (name TEXT PRIMARY KEY, data BLOB);

CREATE INDEX history_previous ON history(previous_track_id, track_id);
)sql";

// Schema v3 (P4-19): saved transitions, one per track pair (the curves as JSON: a 0..1 shape and its length in beats).
constexpr std::string_view kSchemaV3 = R"sql(
CREATE TABLE transitions (
  from_track INTEGER NOT NULL, to_track INTEGER NOT NULL,
  preset TEXT, beats REAL NOT NULL, out_beat REAL NOT NULL, in_beat REAL NOT NULL,
  curves_json TEXT NOT NULL, updated_at INTEGER,
  PRIMARY KEY (from_track, to_track)
);
)sql";

constexpr std::array<std::string_view, 3> kMigrations{kSchemaV1, kSchemaV2, kSchemaV3};
} // namespace

std::span<const std::string_view> migrations() noexcept
{
    return kMigrations;
}

int latestSchemaVersion() noexcept
{
    return static_cast<int>(kMigrations.size());
}

int schemaVersion(Connection& db)
{
    return static_cast<int>(db.queryInt("PRAGMA user_version").value_or(0));
}

util::Result<int> migrate(Connection& db)
{
    using R = util::Result<int>;
    int version = schemaVersion(db);
    if (version > latestSchemaVersion())
        return R::fail("The library was saved by a newer Devil-DJ (schema " + std::to_string(version) +
                       ", this version knows up to " + std::to_string(latestSchemaVersion()) + ")");
    while (version < latestSchemaVersion())
    {
        const auto script = kMigrations[static_cast<std::size_t>(version)];
        const int next = version + 1;
        const bool done = db.transaction(
            [&] { return db.exec(script) && db.exec("PRAGMA user_version = " + std::to_string(next)); });
        if (!done)
            return R::fail("Library migration to schema " + std::to_string(next) + " failed: " + db.lastError());
        version = next;
    }
    return R::ok(version);
}

} // namespace devildj::library
