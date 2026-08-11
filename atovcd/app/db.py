"""SQLite persistence for sessions and visual-change events."""

import sqlite3
import threading
import time
from pathlib import Path

from .config import DATA_DIR

DB_PATH = DATA_DIR / "atovcd.sqlite3"

SCHEMA = """
CREATE TABLE IF NOT EXISTS sessions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at  REAL NOT NULL,
    ended_at    REAL,
    label       TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER NOT NULL REFERENCES sessions(id),
    ts          REAL NOT NULL,
    target      TEXT NOT NULL,
    change      TEXT NOT NULL,
    confidence  REAL NOT NULL,
    bbox        TEXT NOT NULL DEFAULT '',
    note        TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_events_session_ts ON events(session_id, ts DESC);
"""


class Database:
    """Small synchronous wrapper; one connection guarded by a lock."""

    def __init__(self, path: Path = DB_PATH) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        with self._lock:
            self._conn.executescript(SCHEMA)
            self._conn.commit()

    def start_session(self, label: str = "") -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO sessions (started_at, label) VALUES (?, ?)", (time.time(), label)
            )
            self._conn.commit()
            return int(cur.lastrowid)

    def end_session(self, session_id: int) -> None:
        with self._lock:
            self._conn.execute(
                "UPDATE sessions SET ended_at = ? WHERE id = ? AND ended_at IS NULL",
                (time.time(), session_id),
            )
            self._conn.commit()

    def active_session(self) -> sqlite3.Row | None:
        with self._lock:
            return self._conn.execute(
                "SELECT * FROM sessions WHERE ended_at IS NULL ORDER BY id DESC LIMIT 1"
            ).fetchone()

    def session(self, session_id: int) -> sqlite3.Row | None:
        with self._lock:
            return self._conn.execute("SELECT * FROM sessions WHERE id = ?", (session_id,)).fetchone()

    def sessions(self, limit: int = 50) -> list[sqlite3.Row]:
        with self._lock:
            rows = self._conn.execute("SELECT * FROM sessions ORDER BY id DESC LIMIT ?", (limit,))
            return rows.fetchall()

    def add_event(
        self,
        session_id: int,
        target: str,
        change: str,
        confidence: float,
        bbox: str = "",
        note: str = "",
    ) -> int:
        with self._lock:
            cur = self._conn.execute(
                "INSERT INTO events (session_id, ts, target, change, confidence, bbox, note)"
                " VALUES (?, ?, ?, ?, ?, ?, ?)",
                (session_id, time.time(), target, change, confidence, bbox, note),
            )
            self._conn.commit()
            return int(cur.lastrowid)

    def events(self, session_id: int | None = None, limit: int = 200) -> list[sqlite3.Row]:
        query = "SELECT * FROM events"
        params: tuple = ()
        if session_id is not None:
            query += " WHERE session_id = ?"
            params = (session_id,)
        query += " ORDER BY ts DESC, id DESC LIMIT ?"
        with self._lock:
            return self._conn.execute(query, params + (limit,)).fetchall()

    def counts(self, session_id: int) -> dict[str, int]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT change, COUNT(*) AS n FROM events WHERE session_id = ? GROUP BY change",
                (session_id,),
            ).fetchall()
        return {row["change"]: int(row["n"]) for row in rows}


db = Database()
