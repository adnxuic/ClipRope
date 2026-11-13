CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at INTEGER NOT NULL DEFAULT (unixepoch())
) STRICT;

CREATE TABLE ropes (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    sort_rank INTEGER NOT NULL DEFAULT 0,
    capacity INTEGER DEFAULT 100 CHECK(capacity >= 1),
    is_main INTEGER NOT NULL DEFAULT 0 CHECK(is_main IN (0,1)),
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    color_value TEXT NOT NULL DEFAULT 'default',
    material_natural INTEGER NOT NULL DEFAULT 1 CHECK(material_natural IN (0,1)),
    tone_deep INTEGER NOT NULL DEFAULT 7295536,
    tone_dark INTEGER NOT NULL DEFAULT 10122311,
    tone_mid INTEGER NOT NULL DEFAULT 13214315,
    tone_light INTEGER NOT NULL DEFAULT 14532234,
    tone_high INTEGER NOT NULL DEFAULT 15785396,
    texture_seed INTEGER NOT NULL DEFAULT 12648430
) STRICT;

CREATE TABLE snapshots (
    id TEXT PRIMARY KEY,
    captured_at INTEGER NOT NULL DEFAULT (unixepoch()),
    source_hint TEXT NOT NULL DEFAULT '',
    preview_kind TEXT NOT NULL DEFAULT 'text',
    capture_status TEXT NOT NULL DEFAULT 'captured',
    manifest_hash TEXT NOT NULL DEFAULT '',
    screenshot_path TEXT NOT NULL DEFAULT '',
    path_kind INTEGER NOT NULL DEFAULT 0 CHECK(path_kind BETWEEN 0 AND 2),
    snip_directory TEXT NOT NULL DEFAULT '',
    snip_begin INTEGER NOT NULL DEFAULT 0,
    snip_end INTEGER NOT NULL DEFAULT 0
) STRICT;

CREATE TABLE blobs (
    id TEXT PRIMARY KEY CHECK(length(id)=64 AND id NOT GLOB '*[^0-9a-f]*'),
    size INTEGER NOT NULL CHECK(size>=0), inline_bytes BLOB,
    relative_path TEXT,
    CHECK((inline_bytes IS NOT NULL AND length(inline_bytes)=size AND relative_path IS NULL) OR
          (inline_bytes IS NULL AND relative_path IS NOT NULL))
) STRICT;

CREATE TABLE representations (
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK(ordinal>=0),
    standard_id INTEGER,
    format_name TEXT,
    tymed INTEGER NOT NULL DEFAULT 1,
    aspect INTEGER NOT NULL DEFAULT 1,
    lindex INTEGER NOT NULL DEFAULT -1,
    codec_version INTEGER NOT NULL DEFAULT 1,
    priority INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL CHECK(status IN ('saved','failed','skipped')),
    byte_count INTEGER NOT NULL DEFAULT 0 CHECK(byte_count>=0),
    blob_id TEXT REFERENCES blobs(id),
    PRIMARY KEY(snapshot_id,ordinal),
    CHECK((standard_id IS NOT NULL AND standard_id>0 AND format_name IS NULL) OR
          (standard_id IS NULL AND format_name IS NOT NULL AND length(format_name)>0)),
    CHECK((status='saved' AND blob_id IS NOT NULL) OR (status<>'saved' AND blob_id IS NULL))
) STRICT;

CREATE TABLE entries (
    id TEXT PRIMARY KEY,
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id),
    state TEXT NOT NULL CHECK(state IN ('Hanging','Floating')),
    rope_id TEXT REFERENCES ropes(id) ON DELETE RESTRICT,
    sort_key INTEGER,
    revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),
    CHECK(state<>'Hanging' OR (rope_id IS NOT NULL AND sort_key IS NOT NULL))
) STRICT;

CREATE TABLE card_presentation (
    entry_id TEXT PRIMARY KEY REFERENCES entries(id) ON DELETE CASCADE,
    font_size REAL NOT NULL DEFAULT 14 CHECK(font_size>0),
    bold INTEGER NOT NULL DEFAULT 0 CHECK(bold IN (0,1)),
    opacity REAL NOT NULL DEFAULT 1 CHECK(opacity>=0 AND opacity<=1),
    monitor TEXT NOT NULL DEFAULT '',
    x REAL NOT NULL DEFAULT 0,
    y REAL NOT NULL DEFAULT 0,
    width REAL NOT NULL DEFAULT 240 CHECK(width>0),
    height REAL NOT NULL DEFAULT 160 CHECK(height>0),
    visible INTEGER NOT NULL DEFAULT 1 CHECK(visible IN (0,1)),
    width_scale REAL NOT NULL DEFAULT 1 CHECK(width_scale BETWEEN 0.75 AND 2),
    height_scale REAL NOT NULL DEFAULT 1 CHECK(height_scale BETWEEN 0.75 AND 2),
    font_override INTEGER NOT NULL DEFAULT 0 CHECK(font_override IN (0,1)),
    text_ink TEXT NOT NULL DEFAULT 'default',
    paper_tint TEXT NOT NULL DEFAULT 'default'
) STRICT;

CREATE TABLE rope_view_state (
    rope_id TEXT PRIMARY KEY REFERENCES ropes(id) ON DELETE CASCADE,
    anchor_entry_id TEXT REFERENCES entries(id) ON DELETE SET NULL,
    offset REAL NOT NULL DEFAULT 0,
    version INTEGER NOT NULL DEFAULT 0,
    top_dip REAL NOT NULL DEFAULT 24 CHECK(top_dip >= 0),
    compact INTEGER NOT NULL DEFAULT 0 CHECK(compact IN (0,1))
) STRICT;

CREATE TABLE app_settings (
    id INTEGER PRIMARY KEY CHECK(id=1),
    active_rope_id TEXT NOT NULL REFERENCES ropes(id),
    workspace_visible INTEGER NOT NULL DEFAULT 0 CHECK(workspace_visible IN (0,1)),
    default_capacity INTEGER NOT NULL DEFAULT 100 CHECK(default_capacity>=1),
    paste_on_double_click INTEGER NOT NULL DEFAULT 1 CHECK(paste_on_double_click IN (0,1)),
    text_font_size REAL NOT NULL DEFAULT 14 CHECK(text_font_size BETWEEN 6 AND 96),
    hover_resolution INTEGER NOT NULL DEFAULT 1 CHECK(hover_resolution IN (0,1)),
    image_file_previews INTEGER NOT NULL DEFAULT 1 CHECK(image_file_previews IN (0,1)),
    keep_expanded_group INTEGER NOT NULL DEFAULT 1 CHECK(keep_expanded_group IN (0,1))
) STRICT;

CREATE TABLE managed_screenshots (snapshot_id TEXT PRIMARY KEY) STRICT;

CREATE TABLE search_fields (
    snapshot_id TEXT NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    search_text TEXT NOT NULL,
    PRIMARY KEY(snapshot_id,ordinal)
) STRICT, WITHOUT ROWID;

CREATE TABLE appearance_preferences(id INTEGER PRIMARY KEY CHECK(id=1),
    theme INTEGER NOT NULL CHECK(theme BETWEEN 0 AND 2),
    match_value TEXT NOT NULL DEFAULT 'default');

CREATE TABLE recent_colors(color TEXT PRIMARY KEY, rank INTEGER NOT NULL UNIQUE) STRICT;

CREATE UNIQUE INDEX single_main_rope ON ropes(is_main) WHERE is_main=1;

CREATE INDEX representation_blob ON representations(blob_id) WHERE blob_id IS NOT NULL;

CREATE INDEX hanging_order ON entries(rope_id,sort_key,id) WHERE state='Hanging';

CREATE INDEX floating_entries ON entries(id) WHERE state='Floating';

CREATE INDEX snapshot_entries ON entries(snapshot_id);

CREATE TRIGGER keep_main BEFORE DELETE ON ropes WHEN OLD.is_main=1
BEGIN SELECT RAISE(ABORT,'The main rope cannot be deleted'); END;

CREATE TRIGGER preserve_main BEFORE UPDATE OF is_main,id ON ropes
WHEN OLD.is_main=1 AND (NEW.is_main<>1 OR NEW.id<>OLD.id)
BEGIN SELECT RAISE(ABORT,'The main rope identity cannot change'); END;

CREATE TRIGGER new_card AFTER INSERT ON entries
BEGIN INSERT INTO card_presentation(entry_id) VALUES(NEW.id); END;

CREATE TRIGGER new_rope AFTER INSERT ON ropes
BEGIN INSERT INTO rope_view_state(rope_id) VALUES(NEW.id); END;

CREATE TRIGGER floating_shape_insert BEFORE INSERT ON entries
WHEN NEW.state='Floating' AND (NEW.rope_id IS NOT NULL OR NEW.sort_key IS NOT NULL)
BEGIN SELECT RAISE(ABORT,'A Floating entry has no rope or sort key'); END;

CREATE TRIGGER floating_shape_update BEFORE UPDATE OF state,rope_id,sort_key ON entries
WHEN NEW.state='Floating' AND (NEW.rope_id IS NOT NULL OR NEW.sort_key IS NOT NULL)
BEGIN SELECT RAISE(ABORT,'A Floating entry has no rope or sort key'); END;

CREATE TRIGGER keep_main_name BEFORE UPDATE OF name ON ropes
WHEN OLD.is_main=1 AND NEW.name<>'main'
BEGIN SELECT RAISE(ABORT,'The main rope name cannot change'); END;

CREATE TRIGGER require_main_name BEFORE INSERT ON ropes
WHEN NEW.is_main=1 AND NEW.name<>'main'
BEGIN SELECT RAISE(ABORT,'The main rope must be named main'); END;

CREATE TRIGGER keep_rebin BEFORE DELETE ON ropes WHEN OLD.id='system:rebin'
BEGIN SELECT RAISE(ABORT,'ReBin cannot be deleted'); END;

CREATE TRIGGER preserve_rebin BEFORE UPDATE OF id,name,capacity,is_main ON ropes
WHEN OLD.id='system:rebin' AND
     (NEW.id<>OLD.id OR NEW.name<>'ReBin' OR NEW.capacity IS NOT NULL OR NEW.is_main<>0)
BEGIN SELECT RAISE(ABORT,'ReBin has a fixed identity, name and unlimited capacity'); END;

CREATE TRIGGER rope_capacity_insert BEFORE INSERT ON ropes
WHEN (NEW.id='system:rebin')<>(NEW.capacity IS NULL) OR
     (NEW.id='system:rebin' AND (NEW.name<>'ReBin' OR NEW.is_main<>0))
BEGIN SELECT RAISE(ABORT,'Invalid system rope or capacity'); END;

CREATE TRIGGER rope_capacity_update BEFORE UPDATE OF id,capacity ON ropes
WHEN (NEW.id='system:rebin')<>(NEW.capacity IS NULL)
BEGIN SELECT RAISE(ABORT,'Only ReBin has unlimited capacity'); END;

CREATE TRIGGER ordinary_collector_insert BEFORE INSERT ON app_settings
WHEN NEW.active_rope_id='system:rebin'
BEGIN SELECT RAISE(ABORT,'The restart collector must be an ordinary rope'); END;

CREATE TRIGGER ordinary_collector_update BEFORE UPDATE OF active_rope_id ON app_settings
WHEN NEW.active_rope_id='system:rebin'
BEGIN SELECT RAISE(ABORT,'The restart collector must be an ordinary rope'); END;

INSERT INTO ropes(id,name,is_main) VALUES('main','main',1);
INSERT INTO ropes(id,name,capacity) VALUES('system:rebin','ReBin',NULL);
INSERT INTO app_settings(id,active_rope_id) VALUES(1,'main');
INSERT INTO schema_migrations(version) VALUES(11);
PRAGMA application_id=1129467973;
PRAGMA user_version=11;
