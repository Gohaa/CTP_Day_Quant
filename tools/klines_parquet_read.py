#!/usr/bin/env python3
"""Read Quant_Sev data/klines parquet → JSON for C++ LoadContext.

Schema: consider.md §0.2
  data/klines/{product}/{Nm}/{year}.parquet
  columns: datetime, open, high, low, close, volume, time_unix

Usage:
  python tools/klines_parquet_read.py --data-root ./data --product SHFE.rb --interval 1 --limit 600
  python tools/klines_parquet_read.py --job job.json
Job JSON: {"data_root","product","interval_min","limit","out"}  (out optional; else stdout)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from klines_datetime import ensure_shanghai_datetime  # noqa: E402

COLS = ["open", "high", "low", "close", "volume", "time_unix"]


def load_recent(data_root: Path, product: str, interval_min: int, limit: int) -> list[dict]:
    interval_dir = data_root / "klines" / product / f"{int(interval_min)}m"
    if not interval_dir.is_dir():
        return []
    files = sorted(interval_dir.glob("*.parquet"))
    if not files:
        return []

    import pandas as pd

    frames = []
    for f in files:
        try:
            frames.append(pd.read_parquet(f))
        except Exception as e:
            print(f"read_parquet failed {f}: {e}", file=sys.stderr)
    if not frames:
        return []

    df = pd.concat(frames, ignore_index=True)
    ensure_shanghai_datetime(df)
    if "time_unix" not in df.columns:
        return []
    df = df.drop_duplicates(subset=["datetime"]).sort_values("datetime").reset_index(drop=True)
    if limit > 0 and len(df) > limit:
        df = df.iloc[-limit:].reset_index(drop=True)

    out: list[dict] = []
    for row in df.itertuples(index=False):
        try:
            out.append(
                {
                    "open": float(row.open),
                    "high": float(row.high),
                    "low": float(row.low),
                    "close": float(row.close),
                    "volume": float(row.volume),
                    "time_unix": int(row.time_unix),
                }
            )
        except Exception:
            continue
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Read data/klines parquet → JSON bars")
    ap.add_argument("--job", default="", help="job JSON path")
    ap.add_argument("--data-root", default="")
    ap.add_argument("--product", default="")
    ap.add_argument("--interval", type=int, default=1)
    ap.add_argument("--limit", type=int, default=600)
    ap.add_argument("--out", default="", help="write JSON file; default stdout")
    args = ap.parse_args()

    data_root = args.data_root
    product = args.product
    interval = args.interval
    limit = args.limit
    out_path = args.out

    if args.job:
        job = json.loads(Path(args.job).read_text(encoding="utf-8"))
        data_root = str(job.get("data_root") or data_root)
        product = str(job.get("product") or product)
        interval = int(job.get("interval_min") or interval)
        limit = int(job.get("limit") or limit)
        out_path = str(job.get("out") or out_path)

    if not data_root or not product:
        print(json.dumps({"ok": False, "error": "need data_root and product"}, ensure_ascii=False))
        return 2

    bars = load_recent(Path(data_root), product, interval, limit)
    payload = {"ok": True, "product": product, "interval_min": interval, "n": len(bars), "bars": bars}
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    if out_path:
        Path(out_path).write_text(text + "\n", encoding="utf-8")
    else:
        sys.stdout.write(text)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
