"""K-line `datetime` column helpers (Asia/Shanghai wall clock).

Contract (updated 2026-08-21):
  datetime  = timezone-naive pandas/parquet timestamp = bar OPEN in Asia/Shanghai
  time_unix = int64 Unix UTC seconds of bar CLOSE

Legacy: datetime as int64 UTC epoch nanoseconds ¡ª call ensure_shanghai_datetime().
"""
from __future__ import annotations

import pandas as pd


def utc_ns_series_to_shanghai_naive(s: pd.Series) -> pd.Series:
    return (
        pd.to_datetime(s.astype("int64"), unit="ns", utc=True)
        .dt.tz_convert("Asia/Shanghai")
        .dt.tz_localize(None)
    )


def ensure_shanghai_datetime(df: pd.DataFrame, col: str = "datetime") -> pd.DataFrame:
    if col not in df.columns or df.empty:
        return df
    s = df[col]
    if pd.api.types.is_datetime64_any_dtype(s):
        if getattr(s.dt, "tz", None) is not None:
            df[col] = s.dt.tz_convert("Asia/Shanghai").dt.tz_localize(None)
        else:
            df[col] = pd.to_datetime(s)
        return df
    df[col] = utc_ns_series_to_shanghai_naive(s)
    return df


def shanghai_naive_to_utc_ns(s: pd.Series) -> pd.Series:
    return s.dt.tz_localize("Asia/Shanghai").astype("int64")
