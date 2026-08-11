"""ATOVCD FastAPI server: REST API + MJPEG stream + tablet dashboard."""

import asyncio
import json
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import JSONResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import __version__, report
from .camera import build_camera
from .config import store as settings_store
from .db import db
from .engine import Engine

WEB_DIR = Path(__file__).resolve().parent.parent / "web"
TICK_HZ = 4

engine = Engine(db, settings_store)
camera = build_camera()


@asynccontextmanager
async def lifespan(_: FastAPI):
    task = asyncio.create_task(_tick_loop())
    try:
        yield
    finally:
        task.cancel()


async def _tick_loop() -> None:
    while True:
        engine.step()
        await asyncio.sleep(1 / TICK_HZ)


app = FastAPI(title="ATOVCD", version=__version__, lifespan=lifespan)


@app.get("/api/status")
async def api_status() -> JSONResponse:
    return JSONResponse(engine.status())


@app.get("/api/settings")
async def api_get_settings() -> JSONResponse:
    return JSONResponse(vars(settings_store.get()))


@app.put("/api/settings")
async def api_put_settings(request: Request) -> JSONResponse:
    patch = await request.json()
    if not isinstance(patch, dict):
        raise HTTPException(status_code=400, detail="expected a JSON object")
    return JSONResponse(vars(settings_store.update(patch)))


@app.post("/api/session/start")
async def api_start_session(request: Request) -> JSONResponse:
    raw = await request.body()
    label = ""
    if raw:
        body = json.loads(raw)
        label = str(body.get("label", "")) if isinstance(body, dict) else ""
    return JSONResponse({"session_id": engine.start_session(label)})


@app.post("/api/session/stop")
async def api_stop_session() -> JSONResponse:
    engine.stop_session()
    return JSONResponse({"session_id": None})


@app.get("/api/sessions")
async def api_sessions() -> JSONResponse:
    return JSONResponse([_session_summary(int(row["id"])) for row in db.sessions()])


@app.get("/api/history")
async def api_history(
    session_id: int | None = Query(default=None), limit: int = Query(default=200, ge=1, le=1000)
) -> JSONResponse:
    target_session = session_id if session_id is not None else engine.session_id
    if target_session is None:
        return JSONResponse([])
    return JSONResponse(_events(target_session, limit))


@app.get("/api/report")
async def api_report(
    session_id: int | None = Query(default=None),
    format: str = Query(default="json", pattern="^(json|text|csv|pdf)$"),
):
    target_session = session_id if session_id is not None else engine.session_id
    if target_session is None:
        raise HTTPException(status_code=404, detail="no session to report on")
    summary = _session_summary(target_session)
    if summary is None:
        raise HTTPException(status_code=404, detail=f"session {target_session} not found")
    events = list(reversed(_events(target_session, 1000)))
    stem = f"atovcd-session-{target_session:03d}"
    if format == "text":
        return Response(
            "\n".join(report.session_lines(summary, events)), media_type="text/plain; charset=utf-8"
        )
    if format == "csv":
        return Response(
            report.session_csv(summary, events),
            media_type="text/csv",
            headers={"Content-Disposition": f'attachment; filename="{stem}.csv"'},
        )
    if format == "pdf":
        return Response(
            report.session_pdf(summary, events),
            media_type="application/pdf",
            headers={"Content-Disposition": f'attachment; filename="{stem}.pdf"'},
        )
    return JSONResponse({"session": summary, "events": events})


@app.get("/api/stream.mjpg")
async def api_stream() -> StreamingResponse:
    return StreamingResponse(
        _mjpeg_frames(),
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/api/snapshot.jpg")
async def api_snapshot() -> Response:
    frame = await asyncio.to_thread(camera.jpeg, settings_store.get(), engine.targets())
    return Response(frame, media_type="image/jpeg", headers={"Cache-Control": "no-store"})


async def _mjpeg_frames():
    while True:
        settings = settings_store.get()
        frame = await asyncio.to_thread(camera.jpeg, settings, engine.targets())
        yield b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
        yield str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n"
        await asyncio.sleep(1 / max(1, settings.frame_rate))


def _events(session_id: int, limit: int) -> list[dict]:
    return [
        {
            "id": int(row["id"]),
            "session_id": int(row["session_id"]),
            "ts": float(row["ts"]),
            "target": row["target"],
            "change": row["change"],
            "confidence": float(row["confidence"]),
            "bbox": row["bbox"],
        }
        for row in db.events(session_id, limit)
    ]


def _session_summary(session_id: int) -> dict | None:
    row = db.session(session_id)
    if row is None:
        return None
    ended = row["ended_at"]
    counts = db.counts(session_id)
    return {
        "id": int(row["id"]),
        "label": row["label"],
        "started_at": float(row["started_at"]),
        "ended_at": float(ended) if ended is not None else None,
        "duration_s": (float(ended) if ended is not None else time.time()) - float(row["started_at"]),
        "running": ended is None,
        "counts": {
            "new": counts.get("NEW", 0),
            "old": counts.get("OLD", 0),
            "uncertain": counts.get("UNCERTAIN", 0),
            "total": sum(counts.values()),
        },
    }


app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")
