#!/usr/bin/env python3
"""Merge-append Quant_Sev klines parquet (consider.md section 0.2).

datetime column: Asia/Shanghai naive timestamp (bar open), human-readable in viewers.
Incoming C++ jobs still pass datetime as UTC epoch nanoseconds; this helper converts.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from klines_datetime import (  # noqa: E402
    ensure_shanghai_datetime,
    rows_utc_ns_to_shanghai_records,
)

COLS = ["datetime", "open", "high", "low", "close", "volume", "time_unix"]
LOCK_STALE_SEC = 8.0
PARQUET_MAGIC = b"PAR1"


def _acquire_lock(lock: Path) -> None:
    """Create lock file; steal if leftover from a killed helper."""
    for _ in range(80):
        try:
            fd = open(lock, "x", encoding="utf-8")
            fd.write(str(os.getpid()))
            fd.close()
            return
        except FileExistsError:
            try:
                age = time.time() - lock.stat().st_mtime
            except OSError:
                continue
            if age > LOCK_STALE_SEC:
                try:
                    lock.unlink()
                    continue
                except OSError:
                    pass
            time.sleep(0.05)
    raise SystemExit(f"lock timeout: {lock}")


def _looks_like_parquet(path: Path) -> bool:
    try:
        size = path.stat().st_size
        if size < 8:
            return False
        with path.open("rb") as f:
            head = f.read(4)
            f.seek(-4, os.SEEK_END)
            tail = f.read(4)
        return head == PARQUET_MAGIC and tail == PARQUET_MAGIC
    except OSError:
        return False


def _quarantine(path: Path) -> None:
    dest = path.with_suffix(path.suffix + ".corrupt")
    n = 1
    while dest.exists():
        dest = path.with_suffix(path.suffix + f".corrupt{n}")
        n += 1
    try:
        path.replace(dest)
        print(f"quarantined corrupt parquet -> {dest}", file=sys.stderr)
    except OSError as e:
        print(f"quarantine failed {path}: {e}", file=sys.stderr)
        try:
            path.unlink()
        except OSError:
            pass


def _read_existing(path: Path):
    import pandas as pd

    if not path.exists() or path.stat().st_size == 0:
        return None
    if not _looks_like_parquet(path):
        _quarantine(path)
        return None
    try:
        old = pd.read_parquet(path)
        ensure_shanghai_datetime(old)
        return old
    except Exception as e:
        print(f"read_parquet failed {path}: {e}", file=sys.stderr)
        _quarantine(path)
        return None


def write_merge(path: Path, rows: list[dict], replace: bool = False) -> int:
    import pandas as pd

    if not rows:
        return 0
    if rows and isinstance(rows[0].get("datetime"), (int, float)):
        rows = rows_utc_ns_to_shanghai_records(rows)
    df = pd.DataFrame(rows)
    missing = [c for c in COLS if c not in df.columns]
    if missing:
        raise SystemExit(f"missing columns: {missing}")
    df = df[COLS].copy()
    ensure_shanghai_datetime(df)
    df["time_unix"] = df["time_unix"].astype("int64")
    for c in ("open", "high", "low", "close", "volume"):
        df[c] = df[c].astype("float64")

    path.parent.mkdir(parents=True, exist_ok=True)
    lock = path.with_suffix(path.suffix + ".lock")
    _acquire_lock(lock)

    try:
        if (not replace) and path.exists():
            old = _read_existing(path)
            if old is not None and not old.empty:
                df = pd.concat([old, df], ignore_index=True)
        df = (
            df.drop_duplicates(subset=["datetime"], keep="last")
            .sort_values("datetime")
            .reset_index(drop=True)
        )
        tmp = path.with_suffix(path.suffix + ".tmp")
        df.to_parquet(tmp, index=False)
        os.replace(tmp, path)
    finally:
        try:
            tmp = path.with_suffix(path.suffix + ".tmp")
            if tmp.exists():
                tmp.unlink()
        except OSError:
            pass
        try:
            lock.unlink(missing_ok=True)
        except TypeError:
            if lock.exists():
                lock.unlink()

    return len(df)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("job_json", help="JSON file: {path, rows:[...]}")
    args = ap.parse_args()
    payload = json.loads(Path(args.job_json).read_text(encoding="utf-8"))
    path = Path(payload["path"])
    rows = payload.get("rows") or []
    write_merge(path, rows, replace=bool(payload.get("replace")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
