"""Runtime settings for the ATOVCD server, persisted as JSON."""

import json
import os
import threading
from dataclasses import asdict, dataclass, fields
from pathlib import Path

DATA_DIR = Path(os.environ.get("ATOVCD_DATA_DIR", Path(__file__).resolve().parent.parent / "data"))
SETTINGS_PATH = DATA_DIR / "settings.json"


@dataclass
class Settings:
    """Operator-tunable settings exposed through the SETTINGS tab."""

    camera_width: int = 960
    camera_height: int = 540
    frame_rate: int = 12
    detection_confidence: float = 0.55
    change_sensitivity: float = 0.50
    storage_limit_mb: int = 2048
    wifi_ssid: str = "ATOVCD_FIELD"
    wifi_channel: int = 6
    battery_monitoring: bool = True
    language: str = "ms"

    def merge(self, patch: dict) -> None:
        """Apply a partial update, ignoring unknown keys and bad types."""
        types = {f.name: f.type for f in fields(self)}
        for key, value in patch.items():
            if key not in types:
                continue
            caster = {int: int, float: float, bool: _as_bool, str: str}[types[key]]
            try:
                setattr(self, key, caster(value))
            except (TypeError, ValueError):
                continue
        self.frame_rate = max(1, min(30, self.frame_rate))
        self.detection_confidence = min(0.99, max(0.05, self.detection_confidence))
        self.change_sensitivity = min(0.99, max(0.05, self.change_sensitivity))
        self.camera_width = max(320, min(1920, self.camera_width))
        self.camera_height = max(180, min(1080, self.camera_height))


def _as_bool(value) -> bool:
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return bool(value)


class SettingsStore:
    """Thread-safe accessor that writes every change back to disk."""

    def __init__(self, path: Path = SETTINGS_PATH) -> None:
        self._path = path
        self._lock = threading.Lock()
        self._settings = Settings()
        self._load()

    def _load(self) -> None:
        if not self._path.exists():
            return
        try:
            self._settings.merge(json.loads(self._path.read_text()))
        except (OSError, json.JSONDecodeError):
            pass

    def get(self) -> Settings:
        with self._lock:
            return Settings(**asdict(self._settings))

    def update(self, patch: dict) -> Settings:
        with self._lock:
            self._settings.merge(patch)
            snapshot = Settings(**asdict(self._settings))
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._path.write_text(json.dumps(asdict(snapshot), indent=2))
        return snapshot


store = SettingsStore()
