"""Session report rendering (CSV + a dependency-free PDF writer)."""

import csv
import io
import time

_PAGE = (595, 842)  # A4 at 72 dpi
_LINE_HEIGHT = 15


def timestamp(ts: float) -> str:
    return time.strftime("%d/%m/%Y %H:%M:%S", time.localtime(ts))


def duration(seconds: float) -> str:
    seconds = int(max(0, seconds))
    return f"{seconds // 3600:02d}:{seconds % 3600 // 60:02d}:{seconds % 60:02d}"


def session_csv(session: dict, events: list[dict]) -> str:
    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(["ATOVCD SESSION REPORT"])
    writer.writerow(["session", session["id"]])
    writer.writerow(["started", timestamp(session["started_at"])])
    writer.writerow(["duration", duration(session["duration_s"])])
    writer.writerow([])
    writer.writerow(["time", "target", "change", "confidence", "bbox"])
    for event in events:
        writer.writerow(
            [
                timestamp(event["ts"]),
                event["target"],
                event["change"],
                f"{event['confidence']:.2f}",
                event["bbox"],
            ]
        )
    return buffer.getvalue()


def session_lines(session: dict, events: list[dict]) -> list[str]:
    counts = session["counts"]
    lines = [
        "ATOVCD SESSION REPORT",
        "",
        f"Session       : {session['id']:03d}",
        f"Date          : {timestamp(session['started_at'])}",
        f"Duration      : {duration(session['duration_s'])}",
        "",
        f"Visual changes (NEW)   : {counts.get('new', 0):02d}",
        f"Historical (OLD)       : {counts.get('old', 0):02d}",
        f"Uncertain detections   : {counts.get('uncertain', 0):02d}",
        f"Total events logged    : {counts.get('total', 0):02d}",
        "",
        "TIME                  TARGET    CHANGE      CONF",
        "-" * 52,
    ]
    for event in events:
        lines.append(
            f"{timestamp(event['ts']):<22}{event['target']:<10}"
            f"{event['change']:<12}{event['confidence'] * 100:.0f}%"
        )
    return lines


def session_pdf(session: dict, events: list[dict]) -> bytes:
    """Render the text report as a single-page-per-50-lines PDF."""
    lines = session_lines(session, events)
    pages = [lines[i : i + 48] for i in range(0, len(lines), 48)] or [[""]]
    objects: list[bytes] = []

    font_id = 3 + len(pages) * 2
    kids = " ".join(f"{3 + i * 2} 0 R" for i in range(len(pages)))
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(f"<< /Type /Pages /Kids [{kids}] /Count {len(pages)} >>".encode("latin-1"))
    for index, page_lines in enumerate(pages):
        content = _page_stream(page_lines)
        objects.append(
            (
                f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {_PAGE[0]} {_PAGE[1]}] "
                f"/Resources << /Font << /F1 {font_id} 0 R >> >> /Contents {4 + index * 2} 0 R >>"
            ).encode("latin-1")
        )
        objects.append(
            b"<< /Length " + str(len(content)).encode("latin-1") + b" >>\nstream\n" + content + b"\nendstream"
        )
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>")

    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{number} 0 obj\n".encode("latin-1") + body + b"\nendobj\n"
    xref_offset = len(out)
    out += f"xref\n0 {len(objects) + 1}\n".encode("latin-1")
    out += b"0000000000 65535 f \n"
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode("latin-1")
    trailer = f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref_offset}\n%%EOF\n"
    out += trailer.encode("latin-1")
    return bytes(out)


def _page_stream(lines: list[str]) -> bytes:
    y = _PAGE[1] - 56
    parts = ["BT", "/F1 10 Tf", f"1 0 0 1 48 {y} Tm", f"{_LINE_HEIGHT} TL"]
    for line in lines:
        parts.append(f"({_escape(line)}) Tj")
        parts.append("T*")
    parts.append("ET")
    return "\n".join(parts).encode("latin-1", errors="replace")


def _escape(text: str) -> str:
    return text.replace("\\", r"\\").replace("(", r"\(").replace(")", r"\)")
