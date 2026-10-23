"""K-line `datetime` column helpers (Asia/Shanghai wall clock).

Contract (updated 2026-08-21):
  datetime  = timezone-naive pandas/parquet timestamp = bar OPEN in Asia/Shanghai
              displays as e.g. 2025-12-18 09:50:00
  time_unix = int64 Unix UTC seconds of bar CLOSE
              (charts: pass as UTCTimestamp; format Asia/Shanghai via Intl / tickMarkFormatter)

Legacy files may still store datetime as int64 UTC epoch nanoseconds; readers
must call ensure_shanghai_datetime() before use.
"""
from __future__ import annotations

from typing import Any

import pandas as pd


def utc_ns_series_to_shanghai_naive(s: pd.Series) -> pd.Series:
    return (
        pd.to_datetime(s.astype("int64"), unit="ns", utc=True)
        .dt.tz_convert("Asia/Shanghai")
        .dt.tz_localize(None)
    )


def ensure_shanghai_datetime(df: pd.DataFrame, col: str = "datetime") -> pd.DataFrame:
    """In-place normalize `col` to datetime64[ns] Shanghai-naive. Returns df."""
    if col not in df.columns or df.empty:
        return df
    s = df[col]
    if pd.api.types.is_datetime64_any_dtype(s):
        # tz-aware -> Shanghai naive
        if getattr(s.dt, "tz", None) is not None:
            df[col] = s.dt.tz_convert("Asia/Shanghai").dt.tz_localize(None)
        else:
            df[col] = pd.to_datetime(s)
        return df
    # legacy int64 / float UTC ns
    df[col] = utc_ns_series_to_shanghai_naive(s)
    return df


def shanghai_naive_to_utc_ns(s: pd.Series) -> pd.Series:
    """Shanghai-naive timestamps -> UTC epoch nanoseconds (int64)."""
    return s.dt.tz_localize("Asia/Shanghai").astype("int64")


def rows_utc_ns_to_shanghai_records(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Convert job rows with datetime as utc ns int into Shanghai-naive Timestamp values."""
    if not rows:
        return rows
    out: list[dict[str, Any]] = []
    for r in rows:
        rr = dict(r)
        ns = int(rr["datetime"])
        rr["datetime"] = (
            pd.Timestamp(ns, unit="ns", tz="UTC").tz_convert("Asia/Shanghai").tz_localize(None)
        )
        out.append(rr)
    return out
