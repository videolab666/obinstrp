from pathlib import Path


def insert_before(path: Path, marker: str, block: str, guard: str) -> None:
    text = path.read_text(encoding="utf-8")
    if guard in text:
        return
    if marker not in text:
        raise SystemExit(f"marker not found in {path}: {marker}")
    path.write_text(text.replace(marker, block + marker, 1), encoding="utf-8")


# EventDB: return status separately from the overlap output. Storage cleanup
# must never interpret a SQLite failure as "unreferenced" media.
db_path = Path("src/sr-event-db.c")
db_block = r'''bool sr_event_db_has_event_overlap(struct sr_event_db *db, uint64_t start_ns, uint64_t end_ns, bool *overlap)
{
    if (!db || !overlap || end_ns < start_ns || !valid_u64(start_ns) || !valid_u64(end_ns))
        return false;
    *overlap = true;

    pthread_mutex_lock(&db->mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->sql,
                                "SELECT 1 FROM events WHERE in_ns<=? AND out_ns>=? LIMIT 1", -1, &stmt, NULL);
    bool ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)end_ns) == SQLITE_OK &&
              sqlite3_bind_int64(stmt, 2, (sqlite3_int64)start_ns) == SQLITE_OK;
    if (ok) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            *overlap = true;
        else if (rc == SQLITE_DONE)
            *overlap = false;
        else
            ok = false;
    }
    if (!ok)
        log_sql_error(db, "query Event overlap", rc);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db->mutex);
    return ok;
}

'''
insert_before(
    db_path,
    "bool sr_event_db_get_list_events(struct sr_event_db *db, unsigned list_id, uint64_t **event_ids, size_t *count)\n",
    db_block,
    "bool sr_event_db_has_event_overlap(",
)

# The controller owns the serialization boundary. It keeps its mutex held from
# Event deletion through the storage scan, so another mark/hotkey/API call
# cannot create a new Event pointing at a segment between overlap-check and
# unlink.
controller_path = Path("src/sr-event-controller.c")
controller = controller_path.read_text(encoding="utf-8")
if '#include "sr-storage-cleanup.h"' not in controller:
    controller = controller.replace('#include "sr-session.h"\n', '#include "sr-session.h"\n#include "sr-storage-cleanup.h"\n', 1)
    controller_path.write_text(controller, encoding="utf-8")

controller_block = r'''bool sr_event_controller_has_event_overlap(struct sr_event_controller *controller, uint64_t start_ns,
                                           uint64_t end_ns, bool *overlap)
{
    if (!controller || !overlap || end_ns < start_ns)
        return false;

    pthread_mutex_lock(&controller->mutex);
    const bool ok = ensure_db_locked(controller) &&
                    sr_event_db_has_event_overlap(controller->db, start_ns, end_ns, overlap);
    pthread_mutex_unlock(&controller->mutex);
    return ok;
}

bool sr_event_controller_delete_event_with_media(struct sr_event_controller *controller, uint64_t event_id,
                                                 struct sr_storage_cleanup_result *result)
{
    if (!controller || !event_id)
        return false;

    struct sr_storage_cleanup_result local = {0};
    pthread_mutex_lock(&controller->mutex);
    if (!ensure_db_locked(controller)) {
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    struct sr_event_record event = {0};
    if (!sr_event_db_get_event(controller->db, event_id, &event)) {
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }
    if (event.protected_event) {
        sr_event_record_free(&event);
        pthread_mutex_unlock(&controller->mutex);
        return false;
    }

    const uint64_t in_ns = event.in_ns;
    const uint64_t out_ns = event.out_ns;
    sr_event_record_free(&event);

    const bool deleted = sr_event_db_delete_event(controller->db, event_id);
    if (deleted && !sr_storage_delete_unreferenced_range(controller->db, in_ns, out_ns, &local))
        local.errors++;

    pthread_mutex_unlock(&controller->mutex);
    if (result)
        *result = local;
    return deleted;
}

'''
insert_before(
    controller_path,
    "void sr_event_controller_free_event(struct sr_event_record *event)\n",
    controller_block,
    "bool sr_event_controller_delete_event_with_media(",
)

# Event dock: keep metadata-only delete distinct from the destructive physical
# cleanup path. The selected Event must be unprotected; if it is cued on A/B,
# clear those readers before unlinking Windows files.
dock_path = Path("src/sr-event-dock.cpp")
dock = dock_path.read_text(encoding="utf-8")
if '#include "sr-storage-cleanup.h"' not in dock:
    dock = dock.replace('#include "sr-replay-take.h"\n', '#include "sr-replay-take.h"\n#include "sr-storage-cleanup.h"\n', 1)

old = '''\t\tauto *remove = new QPushButton(T("EventDock.Delete"), this);\n\t\tactionBar->addWidget(up);\n\t\tactionBar->addWidget(down);\n\t\tactionBar->addWidget(played);\n\t\tactionBar->addWidget(protect);\n\t\tactionBar->addWidget(remove);\n'''
new = '''\t\tauto *remove = new QPushButton(T("EventDock.Delete"), this);\n\t\tauto *removeMedia = new QPushButton(T("EventDock.DeleteMedia"), this);\n\t\tactionBar->addWidget(up);\n\t\tactionBar->addWidget(down);\n\t\tactionBar->addWidget(played);\n\t\tactionBar->addWidget(protect);\n\t\tactionBar->addWidget(remove);\n\t\tactionBar->addWidget(removeMedia);\n'''
if "EventDock.DeleteMedia" not in dock:
    if old not in dock:
        raise SystemExit("EventDock action-bar marker not found")
    dock = dock.replace(old, new, 1)

old = '''\t\tconnect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(); });\n'''
new = '''\t\tconnect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(false); });\n\t\tconnect(removeMedia, &QPushButton::clicked, this, [this]() { deleteSelected(true); });\n'''
if "deleteSelected(true)" not in dock:
    if old not in dock:
        raise SystemExit("EventDock delete connection marker not found")
    dock = dock.replace(old, new, 1)

start_marker = "\tvoid deleteSelected()\n"
end_marker = "\n\tsr_event_controller *controller = nullptr;"
if "\tvoid deleteSelected(bool deleteMedia)\n" not in dock:
    start = dock.index(start_marker)
    end = dock.index(end_marker, start)
    replacement = r'''	void deleteSelected(bool deleteMedia)
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;

		if (deleteMedia) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventId, &event)) {
				setStatus("EventDock.Failed");
				return;
			}
			const bool protectedEvent = event.protected_event;
			sr_event_controller_free_event(&event);
			if (protectedEvent) {
				setStatus("EventDock.ProtectedMedia");
				return;
			}
		}

		const char *confirmKey = deleteMedia ? "EventDock.DeleteMediaConfirm" : "EventDock.DeleteConfirm";
		if (QMessageBox::question(this, T("EventDock.DeleteTitle"), T(confirmKey)) != QMessageBox::Yes)
			return;

		if (!deleteMedia) {
			if (!sr_event_controller_delete_event(controller, eventId)) {
				setStatus("EventDock.Failed");
				return;
			}
			setStatus("EventDock.Deleted");
			refresh();
			return;
		}

		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			sr_replay_channel_state state = {};
			const auto bus = static_cast<sr_replay_bus>(i);
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
				sr_replay_channel_clear(bus);
		}

		sr_storage_cleanup_result cleanup = {};
		if (!sr_event_controller_delete_event_with_media(controller, eventId, &cleanup)) {
			setStatus("EventDock.MediaCleanupFailed");
			return;
		}

		status->setText(T("EventDock.MediaDeleted")
					.arg(cleanup.segments_deleted)
					.arg(cleanup.segments_pinned)
					.arg(cleanup.errors));
		refresh();
	}
'''
    dock = dock[:start] + replacement + dock[end:]

dock_path.write_text(dock, encoding="utf-8")

locale_path = Path("data/locale/en-US.ini")
locale = locale_path.read_text(encoding="utf-8")
if "EventDock.DeleteMedia=" not in locale:
    anchor = 'EventDock.Delete="Delete"\n'
    extra = (
        'EventDock.DeleteMedia="Delete + Media"\n'
        'EventDock.DeleteMediaConfirm="Delete the selected Event and permanently remove finalized replay segment pairs that are wholly inside this Event and are not referenced by any other saved Event? Boundary segments, active recording files, and shared media are kept. This cannot be undone."\n'
        'EventDock.ProtectedMedia="Unprotect this Event before deleting its recorded media"\n'
        'EventDock.MediaCleanupFailed="Delete + Media could not be completed; check the OBS log before retrying"\n'
        'EventDock.MediaDeleted="Event deleted; removed %1 unreferenced segment(s), kept %2 shared segment(s), %3 cleanup error(s)"\n'
    )
    if anchor not in locale:
        raise SystemExit("locale delete anchor not found")
    locale = locale.replace(anchor, anchor + extra, 1)
locale_path.write_text(locale, encoding="utf-8")
