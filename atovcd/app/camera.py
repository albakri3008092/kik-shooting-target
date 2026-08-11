"""Frame sources for the LIVE view.

Two sources are supported, selected with the ``ATOVCD_CAMERA`` environment
variable:

``synthetic`` (default)
    Renders the tracked scene with Pillow so the whole UI can be driven on a
    laptop with no camera attached.
``picamera2``
    Uses the Raspberry Pi camera stack. Falls back to ``synthetic`` when the
    ``picamera2`` module is unavailable.
"""

import io
import logging
import math
import os
import threading

from PIL import Image, ImageDraw

from .config import Settings

log = logging.getLogger(__name__)

_BG_TOP = (10, 20, 28)
_BG_BOTTOM = (18, 38, 40)
_GRID = (26, 58, 58)


class SyntheticCamera:
    """Draws a berm-and-targets scene, so the operator UI has something real to show."""

    def __init__(self) -> None:
        self._frame_index = 0
        self._lock = threading.Lock()

    def jpeg(self, settings: Settings, targets: list[dict]) -> bytes:
        with self._lock:
            self._frame_index += 1
            index = self._frame_index
        image = self._render(settings.camera_width, settings.camera_height, index, targets)
        buffer = io.BytesIO()
        image.save(buffer, format="JPEG", quality=78)
        return buffer.getvalue()

    def _render(self, width: int, height: int, index: int, targets: list[dict]) -> Image.Image:
        image = Image.new("RGB", (width, height))
        draw = ImageDraw.Draw(image)
        for y in range(height):
            blend = y / max(1, height - 1)
            draw.line(
                [(0, y), (width, y)],
                fill=tuple(int(_BG_TOP[c] + (_BG_BOTTOM[c] - _BG_TOP[c]) * blend) for c in range(3)),
            )
        step = max(24, width // 24)
        for x in range(0, width, step):
            draw.line([(x, 0), (x, height)], fill=_GRID)
        for y in range(0, height, step):
            draw.line([(0, y), (width, y)], fill=_GRID)

        horizon = int(height * 0.62)
        draw.rectangle([0, horizon, width, height], fill=(24, 46, 38))
        draw.line([(0, horizon), (width, horizon)], fill=(58, 108, 88), width=2)

        for target in targets:
            self._draw_target(draw, width, height, index, target)
        return image

    def _draw_target(
        self, draw: ImageDraw.ImageDraw, width: int, height: int, index: int, target: dict
    ) -> None:
        cx, cy = target["cx"] * width, target["cy"] * height
        radius = target["size"] * min(width, height) * 0.5
        sway = math.sin((index + target["seed"] * 7) * 0.08) * radius * 0.06
        colour = {
            "NEW": (255, 96, 96),
            "OLD": (150, 160, 170),
            "UNCERTAIN": (255, 208, 84),
        }.get(target["state"], (0, 229, 168))
        box = [cx - radius + sway, cy - radius, cx + radius + sway, cy + radius]
        draw.ellipse(box, outline=colour, width=3)
        draw.ellipse(
            [box[0] + radius * 0.5, box[1] + radius * 0.5, box[2] - radius * 0.5, box[3] - radius * 0.5],
            outline=colour,
            width=2,
        )
        draw.line([cx + sway, cy - radius * 0.25, cx + sway, cy + radius * 0.25], fill=colour)
        draw.line([cx - radius * 0.25 + sway, cy, cx + radius * 0.25 + sway, cy], fill=colour)
        draw.text((box[0], box[1] - 14), target["id"], fill=colour)


class PiCamera2Camera:
    """Thin wrapper over picamera2 that yields JPEG frames at the configured size."""

    def __init__(self) -> None:
        from picamera2 import Picamera2  # imported lazily; only present on the Pi

        self._camera = Picamera2()
        self._configured: tuple[int, int] | None = None
        self._lock = threading.Lock()

    def jpeg(self, settings: Settings, targets: list[dict]) -> bytes:
        size = (settings.camera_width, settings.camera_height)
        with self._lock:
            if self._configured != size:
                self._camera.stop()
                self._camera.configure(self._camera.create_video_configuration({"size": size}))
                self._camera.start()
                self._configured = size
            frame = self._camera.capture_array()
        buffer = io.BytesIO()
        Image.fromarray(frame[:, :, :3]).save(buffer, format="JPEG", quality=78)
        return buffer.getvalue()


def build_camera():
    """Return the configured frame source, degrading to synthetic frames."""
    if os.environ.get("ATOVCD_CAMERA", "synthetic") == "picamera2":
        try:
            return PiCamera2Camera()
        except Exception:  # a hardware failure must not take the console down
            log.warning("picamera2 unavailable, using synthetic frames", exc_info=True)
    return SyntheticCamera()
