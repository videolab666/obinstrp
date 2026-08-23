from pathlib import Path


def insert_before(path: Path, marker: str, block: str, guard: str) -> None:
    text = path.read_text(encoding="utf-8")
    if guard in text:
        return
    if marker not in text:
        raise SystemExit(f"marker not found in {path}: {marker}")
    path.write_text(text.replace(marker, block + marker, 1), encoding="utf-8")


# EventDB: a tri-state-style overlap query (return status + overlap output).
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

# Controller wrapper keeps SQLite ownership out of storage/UI code.
controller_path = Path("src/sr-event-controller.c")
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

'''
insert_before(
    controller_path,
    "void sr_event_controller_free_event(struct sr_event_record *event)\n",
    controller_block,
    "bool sr_event_controller_has_event_overlap(",
)

# Event dock: separate destructive media action from metadata-only delete.
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

		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}

		if (deleteMedia && event.protected_event) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ProtectedMedia");
			return;
		}

		const char *confirmKey = deleteMedia ? "EventDock.DeleteMediaConfirm" : "EventDock.DeleteConfirm";
		if (QMessageBox::question(this, T("EventDock.DeleteTitle"), T(confirmKey)) != QMessageBox::Yes) {
			sr_event_controller_free_event(&event);
			return;
		}

		const uint64_t inNs = event.in_ns;
		const uint64_t outNs = event.out_ns;
		sr_event_controller_free_event(&event);

		if (deleteMedia) {
			for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
				sr_replay_channel_state state = {};
				const auto bus = static_cast<sr_replay_bus>(i);
				if (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
					sr_replay_channel_clear(bus);
			}
		}

		if (!sr_event_controller_delete_event(controller, eventId)) {
			setStatus("EventDock.Failed");
			return;
		}

		if (!deleteMedia) {
			setStatus("EventDock.Deleted");
			refresh();
			return;
		}

		sr_storage_cleanup_result cleanup = {};
		if (!sr_storage_delete_unreferenced_range(controller, inNs, outNs, &cleanup)) {
			setStatus("EventDock.MediaCleanupFailed");
		} else {
			status->setText(T("EventDock.MediaDeleted")
						.arg(cleanup.segments_deleted)
						.arg(cleanup.segments_pinned)
						.arg(cleanup.errors));
		}
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
        'EventDock.MediaCleanupFailed="Event metadata was deleted, but replay media cleanup could not be completed"\n'
        'EventDock.MediaDeleted="Event deleted; removed %1 unreferenced segment(s), kept %2 shared segment(s), %3 cleanup error(s)"\n'
    )
    if anchor not in locale:
        raise SystemExit("locale delete anchor not found")
    locale = locale.replace(anchor, anchor + extra, 1)
locale_path.write_text(locale, encoding="utf-8")
