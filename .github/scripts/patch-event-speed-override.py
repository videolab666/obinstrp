from pathlib import Path

ROOT = Path('.')


def load(path):
    return (ROOT / path).read_text(encoding='utf-8')


def save(path, text):
    (ROOT / path).write_text(text, encoding='utf-8')


def replace_exact(text, old, new, *, count=1, label='replacement'):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'{label}: expected {count} occurrence(s), found {actual}')
    return text.replace(old, new)


# Event DB public model/schema.
p = 'src/sr-event-db.h'
s = load(p)
s = replace_exact(s, '#define SR_EVENT_DB_SCHEMA_VERSION 2', '#define SR_EVENT_DB_SCHEMA_VERSION 3', label='schema version')
s = replace_exact(
    s,
    '\tdouble speed_percent;\n\tint audio_mode;',
    '\tdouble speed_percent;\n\tbool speed_override; /* false = inherit Global controller in Global mode */\n\tint audio_mode;',
    count=2,
    label='event speed_override fields',
)
save(p, s)

# SQLite migration and persistence.
p = 'src/sr-event-db.c'
s = load(p)
marker = '''static bool migrate_schema(struct sr_event_db *db)\n{'''
migration = '''static bool migrate_v3(struct sr_event_db *db)\n{\n\tif (!begin_transaction(db))\n\t\treturn false;\n\tif (!exec_sql(db,\n\t\t      "ALTER TABLE events ADD COLUMN speed_override INTEGER NOT NULL DEFAULT 0 "\n\t\t      "CHECK(speed_override IN (0, 1))",\n\t\t      "migrate schema v3") ||\n\t    !exec_sql(db, "PRAGMA user_version=3", "set schema v3")) {\n\t\trollback_transaction(db);\n\t\treturn false;\n\t}\n\tif (!commit_transaction(db)) {\n\t\trollback_transaction(db);\n\t\treturn false;\n\t}\n\treturn true;\n}\n\nstatic bool migrate_schema(struct sr_event_db *db)\n{'''
s = replace_exact(s, marker, migration, label='insert migrate_v3')
s = replace_exact(
    s,
    '''\tif (version == 1) {\n\t\tif (!migrate_v2(db))\n\t\t\treturn false;\n\t\tversion = 2;\n\t}\n\n\tdb->schema_version = version;''',
    '''\tif (version == 1) {\n\t\tif (!migrate_v2(db))\n\t\t\treturn false;\n\t\tversion = 2;\n\t}\n\n\tif (version == 2) {\n\t\tif (!migrate_v3(db))\n\t\t\treturn false;\n\t\tversion = 3;\n\t}\n\n\tdb->schema_version = version;''',
    label='wire migrate_v3',
)
s = replace_exact(
    s,
    '''\tif (rc != SQLITE_OK || sqlite3_bind_double(stmt, index++, event->speed_percent) != SQLITE_OK ||\n\t    sqlite3_bind_int(stmt, index++, event->audio_mode) != SQLITE_OK ||''',
    '''\tif (rc != SQLITE_OK || sqlite3_bind_double(stmt, index++, event->speed_percent) != SQLITE_OK ||\n\t    sqlite3_bind_int(stmt, index++, event->speed_override ? 1 : 0) != SQLITE_OK ||\n\t    sqlite3_bind_int(stmt, index++, event->audio_mode) != SQLITE_OK ||''',
    label='bind speed_override',
)
s = replace_exact(
    s,
    '"INSERT INTO events(in_ns,out_ns,preferred_camera_id,speed_percent,audio_mode,protected_event,played,pending,name,tag) "',
    '"INSERT INTO events(in_ns,out_ns,preferred_camera_id,speed_percent,speed_override,audio_mode,protected_event,played,pending,name,tag) "',
    count=2,
    label='insert columns',
)
s = replace_exact(s, '"VALUES(?,?,?,?,?,?,?,?,?,?)"', '"VALUES(?,?,?,?,?,?,?,?,?,?,?)"', count=2, label='insert placeholders')
s = replace_exact(
    s,
    '''\t\t"SELECT id,in_ns,out_ns,preferred_camera_id,speed_percent,audio_mode,protected_event,played,pending,name,tag "\n\t\t"FROM events WHERE id=?";''',
    '''\t\t"SELECT id,in_ns,out_ns,preferred_camera_id,speed_percent,speed_override,audio_mode,protected_event,played,pending,name,tag "\n\t\t"FROM events WHERE id=?";''',
    label='select speed_override',
)
s = replace_exact(
    s,
    '''\t\tevent->speed_percent = sqlite3_column_double(stmt, 4);\n\t\tevent->audio_mode = sqlite3_column_int(stmt, 5);\n\t\tevent->protected_event = sqlite3_column_int(stmt, 6) != 0;\n\t\tevent->played = sqlite3_column_int(stmt, 7) != 0;\n\t\tevent->pending = sqlite3_column_int(stmt, 8) != 0;\n\t\tconst unsigned char *name = sqlite3_column_text(stmt, 9);\n\t\tconst unsigned char *tag = sqlite3_column_text(stmt, 10);''',
    '''\t\tevent->speed_percent = sqlite3_column_double(stmt, 4);\n\t\tevent->speed_override = sqlite3_column_int(stmt, 5) != 0;\n\t\tevent->audio_mode = sqlite3_column_int(stmt, 6);\n\t\tevent->protected_event = sqlite3_column_int(stmt, 7) != 0;\n\t\tevent->played = sqlite3_column_int(stmt, 8) != 0;\n\t\tevent->pending = sqlite3_column_int(stmt, 9) != 0;\n\t\tconst unsigned char *name = sqlite3_column_text(stmt, 10);\n\t\tconst unsigned char *tag = sqlite3_column_text(stmt, 11);''',
    label='decode speed_override',
)
s = replace_exact(
    s,
    '''\t\t"UPDATE events SET in_ns=?,out_ns=?,preferred_camera_id=?,speed_percent=?,audio_mode=?,protected_event=?,"\n\t\t"played=?,pending=?,name=?,tag=?,updated_unix=unixepoch() WHERE id=?";''',
    '''\t\t"UPDATE events SET in_ns=?,out_ns=?,preferred_camera_id=?,speed_percent=?,speed_override=?,audio_mode=?,protected_event=?,"\n\t\t"played=?,pending=?,name=?,tag=?,updated_unix=unixepoch() WHERE id=?";''',
    label='update speed_override column',
)
s = replace_exact(
    s,
    'sqlite3_bind_int64(stmt, 11, (sqlite3_int64)event_id) == SQLITE_OK;',
    'sqlite3_bind_int64(stmt, 12, (sqlite3_int64)event_id) == SQLITE_OK;',
    label='update event id index',
)
save(p, s)

# Preserve override across controller mutations and duplication.
p = 'src/sr-event-controller.c'
s = load(p)
s = replace_exact(
    s,
    '\t\t.speed_percent = record.speed_percent,\n\t\t.audio_mode = record.audio_mode,',
    '\t\t.speed_percent = record.speed_percent,\n\t\t.speed_override = record.speed_override,\n\t\t.audio_mode = record.audio_mode,',
    count=3,
    label='controller preserves speed_override',
)
save(p, s)

# Global playback: explicit Event override wins; -- inherits the controller.
p = 'src/sr-replay-channel.c'
s = load(p)
s = replace_exact(
    s,
    '''\tconst double speed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL\n\t\t\t\t     ? sr_replay_channel_get_controller_speed()\n\t\t\t\t     : event.speed_percent;''',
    '''\tconst bool event_speed_override =\n\t\tsr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL && event.speed_override;\n\tconst double speed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL && !event_speed_override\n\t\t\t\t     ? sr_replay_channel_get_controller_speed()\n\t\t\t\t     : event.speed_percent;''',
    label='cue override speed',
)
save(p, s)

# Operator/Event UI: -- means inherit, numeric value is an explicit override.
p = 'src/sr-event-dock.cpp'
s = load(p)
s = replace_exact(
    s,
    '''\t\tQString second =\n\t\t\tQStringLiteral("%1  ·  %2%").arg(durationText(event)).arg(event.speed_percent, 0, 'f', 0);''',
    '''\t\tconst bool inherited = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&\n\t\t\t\t       !event.speed_override;\n\t\tconst QString speed = inherited ? QStringLiteral("--")\n\t\t\t\t\t\t: QStringLiteral("%1%").arg(event.speed_percent, 0, 'f', 0);\n\t\tQString second = QStringLiteral("%1  ·  %2").arg(durationText(event)).arg(speed);''',
    label='gallery speed label',
)
s = replace_exact(
    s,
    '''\t\t\t\t\tconst double plannedSpeed = sr_config_get_replay_speed_policy() ==\n\t\t\t\t\t\t\t\t\t\t    SR_REPLAY_SPEED_GLOBAL\n\t\t\t\t\t\t\t\t\t    ? sr_replay_channel_get_controller_speed()\n\t\t\t\t\t\t\t\t\t    : event.speed_percent;''',
    '''\t\t\t\t\tconst double plannedSpeed =\n\t\t\t\t\t\tsr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&\n\t\t\t\t\t\t\t\t!event.speed_override\n\t\t\t\t\t\t\t? sr_replay_channel_get_controller_speed()\n\t\t\t\t\t\t\t: event.speed_percent;''',
    label='timeline override speed',
)
s = replace_exact(
    s,
    '''\t\t\ttable->setItem((int)i, 2,\n\t\t\t\t       new QTableWidgetItem(QString::number(event.speed_percent, 'f', 0) +\n\t\t\t\t\t\t\t    QStringLiteral("%")));''',
    '''\t\t\tconst bool inheritedSpeed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&\n\t\t\t\t\t\t    !event.speed_override;\n\t\t\tauto *speedItem = new QTableWidgetItem(\n\t\t\t\tinheritedSpeed ? QStringLiteral("--")\n\t\t\t\t\t       : QString::number(event.speed_percent, 'f', 0) + QStringLiteral("%"));\n\t\t\tspeedItem->setToolTip(T("EventDock.EventSpeed.Tooltip"));\n\t\t\ttable->setItem((int)i, 2, speedItem);''',
    label='table inherited speed label',
)
s = replace_exact(
    s,
    '''\t\tQString speedText = table->item(item->row(), 2)->text();\n\t\tspeedText.remove(QChar('%'));\n\t\tbool speedOk = false;\n\t\tconst double speed = speedText.trimmed().toDouble(&speedOk);\n\t\tconst QByteArray name = table->item(item->row(), 4)->text().trimmed().toUtf8();\n\t\tconst QByteArray tag = table->item(item->row(), 5)->text().trimmed().toUtf8();\n\t\tif (!speedOk || speed < 10.0 || speed > 400.0) {''',
    '''\t\tQString speedText = table->item(item->row(), 2)->text().trimmed();\n\t\tconst bool speedOverride = !speedText.isEmpty() && speedText != QStringLiteral("--");\n\t\tdouble speed = event.speed_percent;\n\t\tbool speedOk = true;\n\t\tif (speedOverride) {\n\t\t\tspeedText.remove(QChar('%'));\n\t\t\tspeed = speedText.trimmed().toDouble(&speedOk);\n\t\t}\n\t\tconst QByteArray name = table->item(item->row(), 4)->text().trimmed().toUtf8();\n\t\tconst QByteArray tag = table->item(item->row(), 5)->text().trimmed().toUtf8();\n\t\tif (!speedOk || (speedOverride && (speed < 10.0 || speed > 400.0))) {''',
    label='parse inherited/override speed',
)
s = replace_exact(
    s,
    '''\t\tupdate.speed_percent = speed;\n\t\tupdate.audio_mode = event.audio_mode;''',
    '''\t\tupdate.speed_percent = speed;\n\t\tupdate.speed_override = speedOverride;\n\t\tupdate.audio_mode = event.audio_mode;''',
    label='persist Event override flag',
)
save(p, s)

# Locale/help text.
p = 'data/locale/en-US.ini'
s = load(p)
s = replace_exact(
    s,
    'EventDock.InvalidSpeed="Speed must be between 10% and 400%"',
    'EventDock.InvalidSpeed="Use -- for Global Speed, or enter a speed between 10% and 400%"',
    label='en invalid speed',
)
s = replace_exact(
    s,
    'Dock.ReplaySpeedPolicyHint="Global controller applies the operator Speed/Shuttle value to the current replay and all following Events/angles. Stored Event speed lets each Event restore its saved Speed when it starts."',
    'Dock.ReplaySpeedPolicyHint="Global controller is the normal operator mode: Events with Speed -- inherit the current Speed/Shuttle value, while a numeric Event Speed is an explicit override for that Event only. Stored Event speed always restores each Event\'s saved numeric Speed."',
    label='en policy hint',
)
s = replace_exact(
    s,
    'EventDock.Speed.Tooltip="Main replay speed controller. Global mode applies it to current and following Events/angles; Stored Event mode changes the current replay but the next Event restores its saved Speed."',
    'EventDock.Speed.Tooltip="Main replay speed controller. In Global mode, Events with Speed -- follow this value; Events with a numeric Speed use their explicit override."\nEventDock.EventSpeed.Tooltip="Event speed: use -- to inherit the Global Speed controller, or enter 10%-400% to save an override for this Event only."',
    label='en speed tooltip',
)
save(p, s)

# Spanish locale: keep behavior discoverable without changing layout.
p = 'data/locale/es-ES.ini'
s = load(p)
if 'EventDock.EventSpeed.Tooltip=' not in s:
    s += '\nEventDock.EventSpeed.Tooltip="Velocidad del evento: use -- para heredar el control de velocidad global, o introduzca 10%-400% para guardar una velocidad propia solo para este evento."\n'
if 'Dock.ReplaySpeedPolicyHint=' in s:
    import re
    s = re.sub(r'^Dock\.ReplaySpeedPolicyHint=.*$', 'Dock.ReplaySpeedPolicyHint="El controlador global es el modo normal: los eventos con Speed -- heredan Speed/Shuttle; un valor numerico de Speed es una excepcion solo para ese evento. Stored Event speed restaura siempre el valor numerico guardado."', s, flags=re.M)
if 'EventDock.InvalidSpeed=' in s:
    import re
    s = re.sub(r'^EventDock\.InvalidSpeed=.*$', 'EventDock.InvalidSpeed="Use -- para velocidad global o introduzca una velocidad entre 10% y 400%"', s, flags=re.M)
save(p, s)

print('Event speed override patch applied successfully')
