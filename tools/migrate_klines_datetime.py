#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Migrate klines parquet: datetime int64 UTC-ns -> Shanghai-naive timestamp.

Usage:
  python tools/migrate_klines_datetime.py
  python tools/migrate_klines_datetime.py --root data/klines --dry-run
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from klines_datetime import ensure_shanghai_datetime  # noqa: E402


def migrate_file(path: Path, dry_run: bool) -> str:
    import pandas as pd

    df = pd.read_parquet(path)
    if "datetime" not in df.columns:
        return "skip-no-datetime"
    before = str(df["datetime"].dtype)
    if pd.api.types.is_datetime64_any_dtype(df["datetime"]) and getattr(df["datetime"].dt, "tz", None) is None:
        sample = str(df["datetime"].iloc[0]) if len(df) else ""
        return f"ok-already {before} sample={sample}"
    ensure_shanghai_datetime(df)
    after = str(df["datetime"].dtype)
    sample = str(df["datetime"].iloc[0]) if len(df) else ""
    if dry_run:
        return f"dry {before}->{after} sample={sample} rows={len(df)}"
    df.to_parquet(path, index=False)
    return f"migrated {before}->{after} sample={sample} rows={len(df)}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=ROOT / "data" / "klines")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    root: Path = args.root
    if not root.is_dir():
        print(f"missing {root}")
        return 1
    n = 0
    for path in sorted(root.rglob("*.parquet")):
        if "_SMOKE" in path.parts:
            continue
        msg = migrate_file(path, args.dry_run)
        print(f"{path.relative_to(root)}: {msg}")
        n += 1
    print(f"done files={n} dry_run={args.dry_run}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
