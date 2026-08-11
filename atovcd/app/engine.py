"""Target tracking / visual-change state machine.

The prototype drives the UI from a simulated tracker so the operator flow can
be validated before the Hailo inference pipeline lands. Swap
:meth:`Engine.analyse` for a real inference call and the rest of the stack
(API, history, reports) keeps working unchanged.
"""

import math
import random
import threading
import time

from .config import SettingsStore
from .db import Database

CHANGE_STATES = ("DETECTED", "NEW", "OLD", "UNCERTAIN")
TARGET_LAYOUT = [
    {"id": "TGT-01", "cx": 0.22, "cy": 0.42, "size": 0.20},
    {"id": "TGT-02", "cx": 0.50, "cy": 0.36, "size": 0.16},
    {"id": "TGT-03", "cx": 0.78, "cy": 0.46, "size": 0.22},
]


class Engine:
    """Owns target state, per-session telemetry and change logging."""

    def __init__(self, database: Database, settings: SettingsStore) -> None:
        self._db = database
        self._settings = settings
        self._lock = threading.Lock()
        self._rng = random.Random(20260811)
        self._boot_ts = time.time()
        self._tick = 0
        self._running = False
        self._session_id: int | None = None
        self._session_started: float | None = None
        self._targets = [
            dict(spec, state="DETECTED", confidence=0.90, seed=i + 1, changed_at=time.time())
            for i, spec in enumerate(TARGET_LAYOUT)
        ]
        active = database.active_session()
        if active is not None:
            self._session_id = int(active["id"])
            self._session_started = float(active["started_at"])
            self._running = True

    # ---------------------------------------------------------------- session

    def start_session(self, label: str = "") -> int:
        with self._lock:
            if self._session_id is None:
                self._session_id = self._db.start_session(label)
                self._session_started = time.time()
            self._running = True
            return self._session_id

    def stop_session(self) -> None:
        with self._lock:
            session_id = self._session_id
            self._session_id = None
            self._session_started = None
            self._running = False
        if session_id is not None:
            self._db.end_session(session_id)

    @property
    def session_id(self) -> int | None:
        return self._session_id

    # ------------------------------------------------------------------ ticks

    def step(self) -> None:
        """Advance the tracker one logic tick (called ~4x per second)."""
        settings = self._settings.get()
        with self._lock:
            self._tick += 1
            if not self._running or self._session_id is None:
                return
            session_id = self._session_id
            transitions = [t for t in self._targets if self._advance(t, settings)]
            snapshot = [(t["id"], t["state"], t["confidence"], self._bbox_text(t)) for t in transitions]
        for target_id, state, confidence, bbox in snapshot:
            self._db.add_event(session_id, target_id, state, confidence, bbox)

    def _advance(self, target: dict, settings) -> bool:
        """Drift a target and decide whether its visual state changed."""
        target["cx"] = _clamp(target["cx"] + self._rng.uniform(-0.004, 0.004), 0.10, 0.90)
        target["cy"] = _clamp(target["cy"] + self._rng.uniform(-0.003, 0.003), 0.18, 0.72)

        if time.time() - target["changed_at"] < 2.0:
            return False
        if self._rng.random() > settings.change_sensitivity * 0.12:
            return False

        confidence = round(self._rng.uniform(0.42, 0.99), 2)
        if confidence < settings.detection_confidence:
            state = "UNCERTAIN"
        elif target["state"] in ("NEW", "UNCERTAIN"):
            state = "OLD"
        else:
            state = "NEW"
        target["state"] = state
        target["confidence"] = confidence
        target["changed_at"] = time.time()
        return True

    def analyse(self, frame) -> list[dict]:
        """Hook for the real detector: return detections for a captured frame."""
        raise NotImplementedError("wire the Hailo/OpenCV pipeline here")

    # ------------------------------------------------------------- read model

    def targets(self) -> list[dict]:
        with self._lock:
            return [dict(t) for t in self._targets]

    def status(self) -> dict:
        settings = self._settings.get()
        with self._lock:
            tick, session_id, started = self._tick, self._session_id, self._session_started
            targets = [dict(t) for t in self._targets]
        uptime = time.time() - self._boot_ts
        counts = self._db.counts(session_id) if session_id is not None else {}
        primary = max(targets, key=lambda t: t["confidence"])
        return {
            "online": True,
            "server_time": time.time(),
            "uptime_s": round(uptime, 1),
            "session": {
                "id": session_id,
                "running": self._running,
                "duration_s": round(time.time() - started, 1) if started else 0.0,
            },
            "camera": {
                "status": "ONLINE",
                "width": settings.camera_width,
                "height": settings.camera_height,
                "fps": settings.frame_rate,
            },
            "imu": {
                "status": "LOCKED",
                "pitch": round(math.sin(tick * 0.05) * 4.2, 2),
                "roll": round(math.cos(tick * 0.04) * 3.1, 2),
                "yaw": round((tick * 0.35) % 360.0, 2),
            },
            "battery": {
                "percent": max(8, 100 - int(uptime / 90)),
                "monitored": settings.battery_monitoring,
            },
            "primary_target": {
                "id": primary["id"],
                "state": primary["state"],
                "confidence": primary["confidence"],
            },
            "counts": {
                "new": counts.get("NEW", 0),
                "old": counts.get("OLD", 0),
                "uncertain": counts.get("UNCERTAIN", 0),
                "total": sum(counts.values()),
            },
            "targets": [
                {
                    "id": t["id"],
                    "state": t["state"],
                    "confidence": t["confidence"],
                    "bbox": self._bbox(t),
                }
                for t in targets
            ],
        }

    def _bbox(self, target: dict) -> dict:
        half = target["size"] / 2
        return {
            "x": round(_clamp(target["cx"] - half, 0.0, 1.0), 4),
            "y": round(_clamp(target["cy"] - half, 0.0, 1.0), 4),
            "w": round(target["size"], 4),
            "h": round(target["size"], 4),
        }

    def _bbox_text(self, target: dict) -> str:
        box = self._bbox(target)
        return f"{box['x']},{box['y']},{box['w']},{box['h']}"


def _clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))
