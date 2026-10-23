#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Preview a klines parquet as a readable table (do not open .parquet as text in the IDE).

  python tools/preview_parquet.py data/klines/CZCE.AP/5m/2025.parquet
  python tools/preview_parquet.py data/klines/CZCE.AP/5m/2025.parquet --tail 5
"""
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", type=Path)
    ap.add_argument("--head", type=int, default=5)
    ap.add_argument("--tail", type=int, default=0)
    args = ap.parse_args()
    import pandas as pd

    df = pd.read_parquet(args.path)
    print("path:", args.path.resolve())
    print("rows:", len(df))
    print("dtypes:")
    print(df.dtypes.to_string())
    print("--- head ---")
    print(df.head(args.head).to_string())
    if args.tail > 0:
        print("--- tail ---")
        print(df.tail(args.tail).to_string())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
