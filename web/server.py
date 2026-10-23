# -*- coding: utf-8 -*-
"""
Quant_Sev web server: quote / analyze / equity / backtest pages + APIs.

  cd C:\\Quant_Sev
  python web/server.py
  http://127.0.0.1:8765/

Requires: pandas, pyarrow
Optional: lib/ for AI backtest analyze/simulate (import bridge)
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
WEB = Path(__file__).resolve().parent
STATIC = WEB / "static"
DATA = ROOT / "data"
KLINES = DATA / "klines"
DOMINANT = DATA / "live" / "dominant"
INSTRUMENTS = DATA / "live" / "instruments"
EP_RAW = DATA / "live" / "ep_raw"
TICKS = DATA / "ticks"
QUOTE = DATA / "live" / "quote"
OPTION_SCREEN = DATA / "live" / "option_screen"
LIB_ROOT = ROOT / "lib"
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

_CN_NAMES_PATH = WEB / "product_names.json"
_download_tasks: dict[str, dict] = {}
_sessions: dict[str, dict] = {}


def _load_cn_names() -> dict[str, str]:
    if _CN_NAMES_PATH.exists():
        try:
            return json.loads(_CN_NAMES_PATH.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {}


CN_NAMES = _load_cn_names()


def product_name(symbol: str) -> str:
    base = re.sub(r"\d+$", "", symbol)
    return CN_NAMES.get(base) or CN_NAMES.get(symbol) or base


def dominant_title(pk: str, dominant: str) -> str:
    """Watchlist primary label: 中证2609 / 铅2609 (display only, not a universe)."""
    cn = product_name(pk)
    m = re.search(r"(\d+)$", dominant or "")
    tag = m.group(1) if m else ""
    if "股指" in cn:
        body = cn.replace("股指", "")
        short = "中证1" if "1000" in body else body[:2]
    elif "国债" in cn:
        short = cn.replace("年期国债", "债").replace("期国债", "债")
    else:
        short = cn
    return f"{short}·{tag}" if tag and short[-1].isdigit() else (f"{short}{tag}" if tag else (dominant or short))


def normalize_symbol(symbol: str) -> str:
    m = re.search(r"@([A-Z]+\.\w+)", symbol)
    return m.group(1) if m else symbol


def ensure_lib_path() -> bool:
    if not LIB_ROOT.is_dir():
        return False
    s = str(LIB_ROOT)
    if s not in sys.path:
        sys.path.insert(0, s)
    return True


def load_dominant_map() -> dict[str, dict]:
    if not DOMINANT.exists():
        return {}
    files = [f for f in sorted(DOMINANT.glob("*.json")) if f.name != "latest.json"]
    latest = DOMINANT / "latest.json"
    path = files[-1] if files else (latest if latest.exists() else None)
    if path is None:
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    out = {}
    for pk, row in (data.get("products") or {}).items():
        if isinstance(row, dict):
            out[pk] = {
                "dominant": row.get("dominant", ""),
                "second": row.get("second", ""),
                "dominant_oi": row.get("dominant_oi", 0),
                "trading_day": data.get("trading_day", ""),
            }
    return out


def _hms_sec(s: str) -> int:
    parts = str(s or "00:00:00").split(":")
    try:
        hh = int(parts[0]) if parts else 0
        mm = int(parts[1]) if len(parts) > 1 else 0
        ss = int(parts[2]) if len(parts) > 2 else 0
    except ValueError:
        return 0
    return hh * 3600 + mm * 60 + ss


_product_sess: dict[str, dict] | None = None


def load_product_session_info() -> dict[str, dict]:
    """Per-product open/close from data/live/sessions + instruments/dominant (not a hardcoded 21:00)."""
    global _product_sess
    if _product_sess is not None:
        return _product_sess
    out: dict[str, dict] = {}

    def apply_segs(pk: str, segs: list) -> dict:
        row = out.setdefault(
            pk,
            {
                "sessions": [],
                "session_night_open": "",
                "session_night_close": "",
                "session_day_open": "",
                "session_day_close": "",
            },
        )
        if segs:
            row["sessions"] = segs
        for s in segs or []:
            b = s.get("time_begin") or ""
            e = s.get("time_end") or ""
            if _hms_sec(b) >= 20 * 3600:
                if not row["session_night_open"]:
                    row["session_night_open"] = b
                    row["session_night_close"] = e
            else:
                if not row["session_day_open"]:
                    row["session_day_open"] = b
                if e:
                    row["session_day_close"] = e
        return row

    sess_path = DATA / "live" / "sessions" / "latest.json"
    if sess_path.is_file():
        try:
            raw = json.loads(sess_path.read_text(encoding="utf-8"))
            for pk, segs in (raw.get("products") or {}).items():
                if isinstance(segs, list):
                    apply_segs(pk, segs)
        except Exception:
            pass

    inst_dir = INSTRUMENTS / "dominant"
    cand = []
    if inst_dir.is_dir():
        cand = [p for p in sorted(inst_dir.glob("*.json")) if p.name != "latest.json"]
        latest = inst_dir / "latest.json"
        if latest.is_file():
            cand.append(latest)
    if cand:
        try:
            data = json.loads(cand[-1].read_text(encoding="utf-8"))
            for inst in data.get("instruments") or []:
                pk = inst.get("product_key") or ""
                if not pk:
                    continue
                segs = inst.get("sessions") or []
                row = apply_segs(pk, segs if isinstance(segs, list) else [])
                if inst.get("session_night_open"):
                    row["session_night_open"] = inst.get("session_night_open") or ""
                    row["session_night_close"] = inst.get("session_night_close") or row["session_night_close"]
                if inst.get("session_day_open"):
                    row["session_day_open"] = inst.get("session_day_open") or ""
                if inst.get("session_day_close"):
                    row["session_day_close"] = inst.get("session_day_close") or ""
        except Exception:
            pass

    _product_sess = out
    return out


def session_open_ts(last, info: dict):
    import pandas as pd

    segs = (info or {}).get("sessions") or []
    night_b = -1
    night_e = -1
    day_b = -1
    for s in segs:
        b = _hms_sec(s.get("time_begin") or "")
        e = _hms_sec(s.get("time_end") or "")
        if b >= 20 * 3600:
            if night_b < 0:
                night_b, night_e = b, e
        elif day_b < 0:
            day_b = b
    if night_b < 0 and (info or {}).get("session_night_open"):
        night_b = _hms_sec(info.get("session_night_open") or "")
        night_e = _hms_sec(info.get("session_night_close") or "23:00:00")
    if day_b < 0 and (info or {}).get("session_day_open"):
        day_b = _hms_sec(info.get("session_day_open") or "")
    sod = int(last.hour) * 3600 + int(last.minute) * 60 + int(getattr(last, "second", 0) or 0)
    if night_b >= 0:
        overnight = night_e >= 0 and night_e < night_b
        in_night = sod >= night_b or (overnight and sod < night_e)
        d = last.normalize()
        if in_night:
            if sod < night_b:
                d = d - pd.Timedelta(days=1)
        else:
            d = d - pd.Timedelta(days=1)
        return d + pd.Timedelta(seconds=int(night_b))
    if day_b < 0:
        day_b = 9 * 3600
    return last.normalize() + pd.Timedelta(seconds=int(day_b))


def list_klines_products() -> dict[str, dict]:
    by: dict[str, dict] = {}
    if not KLINES.exists():
        return by
    for product_dir in sorted(KLINES.iterdir()):
        if not product_dir.is_dir():
            continue
        intervals, years = [], []
        for interval_dir in sorted(product_dir.iterdir()):
            if not interval_dir.is_dir():
                continue
            try:
                iv = int(interval_dir.name.rstrip("m"))
            except ValueError:
                continue
            intervals.append(iv)
            years.extend(pf.stem for pf in interval_dir.glob("*.parquet"))
        if intervals:
            by[product_dir.name] = {
                "intervals": sorted(set(intervals)),
                "years": sorted(set(years)),
                "bar_count": 0,
            }
    return by


def session_slice(df, symbol: str = ""):
    """Current product session from data/live/sessions (openctp /times).

    Night product: last print's night-open (e.g. 21:00) through now. Untraded
    daytime minutes stay empty. Never rewind to the previous 21:00→15:00 session.
    """
    import pandas as pd

    if df is None or df.empty:
        return df
    last = pd.to_datetime(df["datetime"].iloc[-1])
    info = load_product_session_info().get(normalize_symbol(symbol), {}) if symbol else {}
    start = session_open_ts(last, info)
    dt = pd.to_datetime(df["datetime"])
    sl = df[dt >= start]
    if len(sl) >= 1:
        return sl
    return df.tail(240)


_ep_bar_cache: dict[tuple, object] = {}


def _latest_ep_raw_day() -> str:
    if not EP_RAW.is_dir():
        return ""
    days = sorted(p.name for p in EP_RAW.iterdir() if p.is_dir() and p.name.isdigit())
    return days[-1] if days else ""


def _inst_ids_for_product(product: str) -> list[str]:
    row = load_dominant_map().get(normalize_symbol(product), {})
    out: list[str] = []
    for k in ("dominant", "second"):
        v = str(row.get(k) or "").strip()
        if v and v not in out:
            out.append(v)
    return out


def _quote_tape_dir() -> Path | None:
    if not QUOTE.is_dir():
        return None
    day = ""
    latest = QUOTE / "latest.json"
    if latest.is_file():
        try:
            day = str(json.loads(latest.read_text(encoding="utf-8")).get("trading_day") or "")
        except Exception:
            day = ""
    if day:
        d = QUOTE / day
        if d.is_dir():
            return d
    days = sorted(p for p in QUOTE.iterdir() if p.is_dir() and p.name.isdigit())
    return days[-1] if days else None


def load_quote_index() -> dict:
    d = _quote_tape_dir()
    if d is None:
        return {}
    p = d / "index.json"
    if not p.is_file():
        return {}
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {}


def load_quote_product(symbol: str) -> dict | None:
    d = _quote_tape_dir()
    if d is None:
        return None
    p = d / f"{normalize_symbol(symbol)}.json"
    if not p.is_file():
        return None
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None
    return data if isinstance(data, dict) else None


def _pick_ep_csv(dirpath: Path, product: str) -> Path | None:
    """Dominant (then second) InstrumentID only — never the fattest other month."""
    if not dirpath.is_dir():
        return None
    for inst in _inst_ids_for_product(product):
        p = dirpath / f"{inst}.csv"
        if p.is_file():
            return p
    return None


def _read_ep_m1_csv(path: Path):
    import pandas as pd
    from klines_datetime import ensure_shanghai_datetime

    key = ("m1", str(path), path.stat().st_mtime, path.stat().st_size)
    hit = _ep_bar_cache.get(key)
    if hit is not None:
        return hit
    df = pd.read_csv(path, encoding="utf-8-sig")
    empty = pd.DataFrame(columns=["datetime", "open", "high", "low", "close", "volume", "time_unix"])
    if df.empty or "datetime" not in df.columns:
        _ep_bar_cache[key] = empty
        return empty
    df["datetime"] = pd.to_datetime(df["datetime"], errors="coerce")
    df = df.dropna(subset=["datetime"])
    for c in ("open", "high", "low", "close"):
        df[c] = pd.to_numeric(df.get(c), errors="coerce")
    df["volume"] = pd.to_numeric(df.get("volume"), errors="coerce").fillna(0)
    df = df[(df["close"] > 0) | (df["high"] > 0)].copy()
    if df.empty:
        _ep_bar_cache[key] = empty
        return empty
    ts = pd.to_datetime(df["datetime"]).dt.tz_localize("Asia/Shanghai")
    df["time_unix"] = (ts.astype("int64") // 10**9) + 60
    df = df[["datetime", "open", "high", "low", "close", "volume", "time_unix"]]
    ensure_shanghai_datetime(df)
    _ep_bar_cache[key] = df
    return df


def _tick_csv_to_m1(path: Path):
    """Aggregate 极智/CTP tick CSV to 1-minute OHLC (volume = sum LastQty)."""
    import pandas as pd
    from klines_datetime import ensure_shanghai_datetime

    key = ("tick1m", str(path), path.stat().st_mtime, path.stat().st_size)
    hit = _ep_bar_cache.get(key)
    if hit is not None:
        return hit
    empty = pd.DataFrame(columns=["datetime", "open", "high", "low", "close", "volume", "time_unix"])
    try:
        df = pd.read_csv(path, encoding="utf-8-sig", on_bad_lines="skip")
    except Exception:
        _ep_bar_cache[key] = empty
        return empty
    if df.empty:
        _ep_bar_cache[key] = empty
        return empty
    cols = {c.lower(): c for c in df.columns}
    dt_col = cols.get("datetime")
    last_col = cols.get("lastprice") or cols.get("last_price")
    qty_col = cols.get("lastqty") or cols.get("last_qty")
    vol_col = cols.get("volume") or cols.get("totalqty")
    if not dt_col or not last_col:
        _ep_bar_cache[key] = empty
        return empty
    if qty_col:
        qty = pd.to_numeric(df[qty_col], errors="coerce").fillna(0)
    elif vol_col:
        cum = pd.to_numeric(df[vol_col], errors="coerce").fillna(0)
        qty = cum.diff().clip(lower=0)
        if len(qty):
            qty.iloc[0] = 0
    else:
        qty = 0
    tmp = pd.DataFrame(
        {
            "datetime": pd.to_datetime(df[dt_col], errors="coerce"),
            "last": pd.to_numeric(df[last_col], errors="coerce"),
            "qty": qty,
        }
    ).dropna(subset=["datetime"])
    tmp = tmp[tmp["last"] > 0]
    if tmp.empty:
        _ep_bar_cache[key] = empty
        return empty
    tmp["bar"] = tmp["datetime"].dt.floor("min")
    g = tmp.groupby("bar", sort=True)
    out = g.agg(
        open=("last", "first"),
        high=("last", "max"),
        low=("last", "min"),
        close=("last", "last"),
        volume=("qty", "sum"),
    ).reset_index().rename(columns={"bar": "datetime"})
    ts = pd.to_datetime(out["datetime"]).dt.tz_localize("Asia/Shanghai")
    out["time_unix"] = (ts.astype("int64") // 10**9) + 60
    ensure_shanghai_datetime(out)
    _ep_bar_cache[key] = out
    return out


def fallback_ep_raw_m1(product: str):
    """Only 极智 M1 CSV. Never fold Tick/CTP into OHLC (dirty wicks / volume spikes)."""
    import pandas as pd
    from klines_datetime import ensure_shanghai_datetime

    extra = []

    def _safe_read(path: Path):
        try:
            return _read_ep_m1_csv(path)
        except Exception:
            return None

    insts = _inst_ids_for_product(product)
    inst = insts[0] if insts else None
    if inst and EP_RAW.is_dir():
        for day_dir in sorted(p for p in EP_RAW.iterdir() if p.is_dir() and p.name.isdigit()):
            m1p = day_dir / "m1" / f"{inst}.csv"
            if m1p.is_file():
                extra.append(_safe_read(m1p))
    extra = [x for x in extra if x is not None and not getattr(x, "empty", True)]
    if not extra:
        return pd.DataFrame(columns=["datetime", "open", "high", "low", "close", "volume", "time_unix"])
    out = pd.concat(extra, ignore_index=True)
    ensure_shanghai_datetime(out)
    return out.drop_duplicates(subset=["datetime"], keep="last").sort_values("datetime").reset_index(drop=True)


def overlay_ep_raw_1m(df, product: str):
    """Kept for callers: if parquet empty, fill from 极智 M1 only."""
    import pandas as pd

    if df is not None and not getattr(df, "empty", True):
        return keep_session_minutes(df, product)
    return keep_session_minutes(fallback_ep_raw_m1(product), product)


def keep_session_minutes(df, product: str):
    """Drop bars whose clock is outside openctp /times segments (e.g. 20:26 pre-open)."""
    if df is None or getattr(df, "empty", True):
        return df
    info = load_product_session_info().get(normalize_symbol(product), {})
    segs = info.get("sessions") or []
    if not segs:
        return df
    import pandas as pd

    dt = pd.to_datetime(df["datetime"])
    sod = dt.dt.hour * 3600 + dt.dt.minute * 60 + dt.dt.second
    mask = None
    for s in segs:
        b = _hms_sec(s.get("time_begin") or "")
        e = _hms_sec(s.get("time_end") or "")
        if e < b:
            part = (sod >= b) | (sod <= e)
        else:
            part = (sod >= b) & (sod <= e)
        mask = part if mask is None else (mask | part)
    if mask is None:
        return df
    return df.loc[mask].reset_index(drop=True)


def _downsample(vals: list, n: int) -> list:
    if n <= 0 or len(vals) <= n:
        return vals
    if n == 1:
        return [vals[-1]]
    out = []
    last = len(vals) - 1
    for i in range(n):
        out.append(vals[round(i * last / (n - 1))])
    return out


def session_slot_sods(info: dict) -> list[int]:
    """Minute slots for one product session, same order as quote.html buildSessionSlots."""
    segs = (info or {}).get("sessions") or []
    out: list[int] = []

    def add(b: int, e: int) -> None:
        if b < 0 or e < 0:
            return
        if e < b:
            out.extend(range(b, 86400, 60))
            out.extend(range(0, e, 60))
        else:
            out.extend(range(b, e, 60))

    if segs:
        night, day = [], []
        for s in segs:
            b = _hms_sec(s.get("time_begin") or "")
            e = _hms_sec(s.get("time_end") or "")
            if b >= 20 * 3600:
                night.append((b, e))
            else:
                day.append((b, e))
        for b, e in night:
            add(b, e)
        for b, e in sorted(day, key=lambda x: x[0]):
            add(b, e)
        return out
    night = _hms_sec((info or {}).get("session_night_open") or "") if (info or {}).get("session_night_open") else -1
    nend = _hms_sec((info or {}).get("session_night_close") or "23:00:00") if night >= 0 else -1
    day = _hms_sec((info or {}).get("session_day_open") or "09:00:00")
    close = _hms_sec((info or {}).get("session_day_close") or "15:00:00")
    if night >= 0:
        add(night, nend if nend >= 0 else 23 * 3600)
    add(day, 11 * 3600 + 30 * 60)
    if close > 13 * 3600:
        add(13 * 3600 + 30 * 60, close)
    else:
        add(13 * 3600, close)
    return out


def _hold_short_gaps(aligned: list, max_hold: int = 2) -> list:
    """Fill 1–2 empty minutes (thin tape). Longer holes stay None so the line breaks."""
    n = len(aligned)
    i = 0
    while i < n:
        if aligned[i] is not None:
            i += 1
            continue
        j = i
        while j < n and aligned[j] is None:
            j += 1
        prev = aligned[i - 1] if i > 0 else None
        if prev is not None and 0 < (j - i) <= max_hold and j < n and aligned[j] is not None:
            for k in range(i, j):
                aligned[k] = prev
        i = j
    return aligned


def session_aligned_sparkline(df, product: str, n: int = 64) -> list:
    """Closes on the full session axis; missing minutes stay None (left 简图断线)."""
    import pandas as pd

    if df is None or df.empty:
        return []
    info = load_product_session_info().get(normalize_symbol(product), {})
    slots = session_slot_sods(info)
    closes_raw = [round(float(x), 4) for x in df["close"].tolist()]
    if not slots:
        return _downsample(closes_raw, n)
    dt = pd.to_datetime(df["datetime"])
    sod = (dt.dt.hour * 3600 + dt.dt.minute * 60).astype(int)
    by_sod: dict[int, float] = {}
    for s, c in zip(sod.tolist(), closes_raw):
        by_sod[int(s)] = c
    aligned = [by_sod.get(int(s)) for s in slots]
    _hold_short_gaps(aligned)
    return _downsample(aligned, n)


def quote_tape_m1_df(product: str):
    """Current session 1m from QuoteTapeBuilder (极智 ep_raw + CTP ticks)."""
    tape = load_quote_product(product)
    if not tape:
        return None
    rows = tape.get("klines") or []
    if not rows:
        return None
    import pandas as pd
    from klines_datetime import ensure_shanghai_datetime

    recs = []
    for r in rows:
        if not isinstance(r, dict):
            continue
        dt = r.get("datetime_str") or r.get("datetime")
        close = r.get("close")
        try:
            close_f = float(close)
        except (TypeError, ValueError):
            continue
        if not dt or not (close_f > 0):
            continue
        try:
            tu = int(r.get("time_unix") or r.get("time") or 0)
        except (TypeError, ValueError):
            tu = 0
        recs.append(
            {
                "datetime": dt,
                "open": float(r.get("open") or close_f),
                "high": float(r.get("high") or close_f),
                "low": float(r.get("low") or close_f),
                "close": close_f,
                "volume": float(r.get("volume") or 0),
                "time_unix": tu,
            }
        )
    if not recs:
        return None
    df = pd.DataFrame(recs)
    df["datetime"] = pd.to_datetime(df["datetime"], errors="coerce")
    df = df.dropna(subset=["datetime"])
    if df.empty:
        return None
    ensure_shanghai_datetime(df)
    df["datetime"] = pd.to_datetime(df["datetime"]).dt.floor("min")
    missing = df["time_unix"].fillna(0).astype("int64") <= 0
    if missing.any():
        ts = pd.to_datetime(df.loc[missing, "datetime"]).dt.tz_localize("Asia/Shanghai")
        df.loc[missing, "time_unix"] = (ts.astype("int64") // 10**9) + 60
    return df.drop_duplicates(subset=["datetime"], keep="last").sort_values("datetime").reset_index(drop=True)


def last_quote(symbol: str, interval_min: int, sparkline_n: int = 0) -> dict | None:
    tape = load_quote_product(symbol)
    if tape and isinstance(tape.get("quote"), dict):
        q = dict(tape["quote"])
        if sparkline_n <= 0:
            q.pop("sparkline", None)
        return q
    try:
        import pandas as pd
    except ImportError:
        return None
    product = normalize_symbol(symbol)
    interval_dir = KLINES / product / f"{interval_min}m"
    files = sorted(interval_dir.glob("*.parquet")) if interval_dir.is_dir() else []
    if not files:
        return None
    try:
        frames = [pd.read_parquet(str(f)) for f in files[-2:]]
        df = pd.concat(frames, ignore_index=True)
    except Exception:
        return None
    if df.empty:
        return None
    from klines_datetime import ensure_shanghai_datetime

    ensure_shanghai_datetime(df)
    df = df.drop_duplicates(subset=["datetime"]).sort_values("datetime").reset_index(drop=True)
    if interval_min == 1:
        df = overlay_ep_raw_1m(df, product)
    else:
        df = keep_session_minutes(df, product)
    sess = session_slice(df, product)
    if sess is None or sess.empty:
        sess = df.tail(240)
    last = sess.iloc[-1]
    last_px = float(last["close"])
    sess_open = float(sess.iloc[0]["open"])
    sess_high = float(sess["high"].max())
    sess_low = float(sess["low"].min())
    sess_vol = float(sess["volume"].sum())
    before = df[pd.to_datetime(df["datetime"]) < pd.to_datetime(sess["datetime"].iloc[0])]
    if len(before) > 0:
        prev_px = float(before.iloc[-1]["close"])
    else:
        prev_px = sess_open
    chg = last_px - prev_px
    chg_pct = (chg / prev_px * 100.0) if prev_px else 0.0
    out = {
        "last": round(last_px, 4),
        "open": round(sess_open, 4),
        "high": round(sess_high, 4),
        "low": round(sess_low, 4),
        "close": round(last_px, 4),
        "volume": sess_vol,
        "prev_close": round(prev_px, 4),
        "change": round(chg, 4),
        "change_pct": round(chg_pct, 4),
        "time_unix": int(last["time_unix"]) if "time_unix" in sess.columns and pd.notna(last.get("time_unix")) else 0,
        "interval_min": interval_min,
    }
    if sparkline_n > 0:
        out["sparkline"] = session_aligned_sparkline(sess, product, sparkline_n)
    return out


def build_watchlist(with_sparklines: bool = True) -> dict:
    products = list_klines_products()
    dominant = load_dominant_map()
    # Universe = DominantSelector JSON (step06), not whatever happens to be on disk.
    if dominant:
        keys = list(dominant.keys())
        trading_day = next(iter(dominant.values())).get("trading_day", "")
    else:
        keys = list(products.keys())
        trading_day = ""
    watch = []
    tape_idx = load_quote_index()
    tape_prods = tape_idx.get("products") or {}
    tape_day = str(tape_idx.get("trading_day") or "")
    if tape_day:
        trading_day = tape_day
    for pk in keys:
        meta = products.get(pk) or {"intervals": [], "years": [], "bar_count": 0}
        intervals = meta.get("intervals") or []
        prefer = 1 if 1 in intervals else (5 if 5 in intervals else (15 if 15 in intervals else (intervals[0] if intervals else 1)))
        dom = dominant.get(pk, {})
        quote = None
        trow = tape_prods.get(pk) if isinstance(tape_prods, dict) else None
        if isinstance(trow, dict) and isinstance(trow.get("quote"), dict):
            quote = trow["quote"]
        elif intervals:
            try:
                quote = last_quote(pk, prefer, sparkline_n=64 if with_sparklines else 0)
            except Exception:
                quote = None
        sess = load_product_session_info().get(pk, {})
        watch.append(
            {
                "symbol": pk,
                "display_name": product_name(pk),
                "title": dominant_title(pk, dom.get("dominant", "")),
                "dominant": dom.get("dominant", ""),
                "second": dom.get("second", ""),
                "dominant_oi": dom.get("dominant_oi", 0),
                "trading_day": tape_day or dom.get("trading_day") or trading_day,
                "intervals": intervals,
                "years": meta.get("years") or [],
                "bar_count": meta.get("bar_count") or 0,
                "quote": quote,
                "label": dom.get("dominant") or pk.split(".", 1)[-1],
                "sessions": sess.get("sessions") or [],
                "session_night_open": sess.get("session_night_open") or "",
                "session_night_close": sess.get("session_night_close") or "",
                "session_day_open": sess.get("session_day_open") or "",
                "session_day_close": sess.get("session_day_close") or "",
            }
        )

    def rank(w: dict) -> tuple:
        ex = w["symbol"].split(".", 1)[0]
        order = {"CFFEX": 0, "SHFE": 1, "INE": 2, "DCE": 3, "CZCE": 4, "GFEX": 5}
        return (order.get(ex, 9), w["symbol"])

    watch.sort(key=rank)
    return {
        "watchlist": watch,
        "count": len(watch),
        "dominant_count": len(dominant),
        "klines_root": str(KLINES.resolve()),
        "has_dominant": bool(dominant),
        "data_root": str(DATA.resolve()),
        "trading_day": trading_day,
    }


def load_klines(symbol: str, interval_min: int, limit: int = 2000) -> dict:
    try:
        import pandas as pd
    except ImportError as e:
        return {"error": f"pandas required: {e}", "klines": [], "total": 0}

    product = normalize_symbol(symbol)
    m1_dir = KLINES / product / "1m"
    interval_dir = KLINES / product / f"{interval_min}m"
    source_iv = interval_min
    source = "parquet"
    frames = []

    def _read_parquets(d):
        out = []
        if not d.is_dir():
            return out
        for f in sorted(d.glob("*.parquet")):
            try:
                out.append(pd.read_parquet(str(f)))
            except Exception:
                pass
        return out

    native = _read_parquets(interval_dir)
    if native:
        frames = native
        source_iv = interval_min
    elif interval_min not in (1, 1440):
        frames = _read_parquets(m1_dir)
        if frames:
            source_iv = 1
            source = "parquet_1m_resample"
    elif interval_min == 1:
        frames = _read_parquets(m1_dir)

    from klines_datetime import ensure_shanghai_datetime

    if frames:
        df = pd.concat(frames, ignore_index=True)
        ensure_shanghai_datetime(df)
        df = df.drop_duplicates(subset=["datetime"], keep="last").sort_values("datetime").reset_index(drop=True)
    else:
        df = pd.DataFrame(columns=["datetime", "open", "high", "low", "close", "volume", "time_unix"])

    if (df is None or df.empty) and interval_min in (1, 5):
        df = fallback_ep_raw_m1(product)
        source = "ep_raw_m1"
        source_iv = 1

    if interval_min == 1 or source_iv == 1:
        tape_df = quote_tape_m1_df(product)
        if tape_df is not None and not tape_df.empty:
            if df is None or df.empty:
                df = tape_df
                source = "quote_tape"
                source_iv = 1
            else:
                df = pd.concat([df, tape_df], ignore_index=True)
                ensure_shanghai_datetime(df)
                df["datetime"] = pd.to_datetime(df["datetime"]).dt.floor("min")
                df = df.drop_duplicates(subset=["datetime"], keep="last").sort_values("datetime").reset_index(
                    drop=True
                )
                if "quote_tape" not in source:
                    source = f"{source}+quote_tape"

    if interval_min != 1440:
        df = keep_session_minutes(df, product)

    if df is None or df.empty:
        return {
            "error": f"missing {interval_dir} (and no 1m / ep_raw fallback)",
            "klines": [],
            "total": 0,
            "symbol": product,
        }

    if source_iv == 1 and interval_min > 1:
        step = interval_min
        df = df.copy()
        df["_bucket"] = df["datetime"].dt.floor(f"{step}min")
        g = df.groupby("_bucket", as_index=False)
        df = g.agg(
            datetime=("_bucket", "first"),
            open=("open", "first"),
            high=("high", "max"),
            low=("low", "min"),
            close=("close", "last"),
            volume=("volume", "sum"),
            time_unix=("time_unix", "last"),
        ).sort_values("datetime").reset_index(drop=True)

    df = df.tail(limit).reset_index(drop=True)
    closes = df["close"].astype(float)
    ema20 = closes.ewm(span=20, adjust=False).mean().round(2)
    df_out = df.copy()
    for col in ("open", "high", "low", "close"):
        df_out[col] = df_out[col].astype(float).round(2)
    df_out["volume"] = df_out["volume"].astype(float).fillna(0).astype(int)
    df_out["ema20"] = ema20.values
    df_out["time_unix"] = df_out["time_unix"].astype(int)
    df_out["time"] = df_out["time_unix"]
    df_out["datetime_str"] = pd.to_datetime(df_out["datetime"]).dt.strftime("%Y-%m-%d %H:%M:%S")
    records = df_out[
        ["time", "time_unix", "open", "high", "low", "close", "volume", "ema20", "datetime_str"]
    ].to_dict("records")
    sess_info = load_product_session_info().get(product, {})
    return {
        "klines": records,
        "total": len(records),
        "symbol": product,
        "interval_minutes": interval_min,
        "source_interval_minutes": source_iv,
        "source": source,
        "session_sliced": False,
        "tz": "Asia/Shanghai",
        "sessions": sess_info.get("sessions") or [],
        "session_night_open": sess_info.get("session_night_open") or "",
        "session_night_close": sess_info.get("session_night_close") or "",
        "session_day_open": sess_info.get("session_day_open") or "",
        "session_day_close": sess_info.get("session_day_close") or "",
    }


def _product_code(sym: str) -> str:
    s = normalize_symbol(sym.strip())
    if "." in s:
        s = s.split(".", 1)[1]
    i = len(s)
    while i > 0 and s[i - 1].isdigit():
        i -= 1
    return s[:i]


def _resolve_tick_instrument(symbol: str, trading_day: str):
    """Map product key or instrument id to ticks/{day}/{InstrumentID}.csv.

    Returns (instrument_id, path_or_None).
    """
    day_dir = TICKS / trading_day
    if not day_dir.is_dir():
        return "", None
    sym = symbol.strip()
    # direct instrument file
    for cand in (day_dir / f"{sym}.csv",):
        if cand.is_file():
            return cand.stem, cand
    # product key -> dominant instrument only (never fattest other month)
    dominant = load_dominant_map()
    row = dominant.get(normalize_symbol(sym), {})
    inst = (row.get("dominant") or "").strip()
    if inst:
        p = day_dir / f"{inst}.csv"
        if p.is_file():
            return inst, p
    return "", None


def load_ticks(symbol: str, trading_day: str = "", limit: int = 20000) -> dict:
    day = trading_day.strip()
    if not day:
        cands = []
        if EP_RAW.is_dir():
            for p in EP_RAW.iterdir():
                tdir = p / "tick"
                if p.is_dir() and p.name.isdigit() and tdir.is_dir() and any(tdir.glob("*.csv")):
                    cands.append(p.name)
        if TICKS.is_dir():
            for p in TICKS.iterdir():
                if p.is_dir() and p.name.isdigit() and any(p.glob("*.csv")):
                    cands.append(p.name)
        if cands:
            day = sorted(cands, reverse=True)[0]
        if not day:
            dominant = load_dominant_map()
            for v in dominant.values():
                if v.get("trading_day"):
                    day = str(v["trading_day"])
                    break
    if not day:
        return {"error": "no trading_day", "ticks": [], "total": 0}
    inst, path = _resolve_tick_instrument(symbol, day)
    ep_rich = None
    tick_dir = EP_RAW / day / "tick"
    if tick_dir.is_dir():
        if inst:
            cand = tick_dir / f"{inst}.csv"
            if cand.is_file():
                ep_rich = cand
        if ep_rich is None:
            prod = _product_code(symbol)
            hits = [
                p for p in tick_dir.glob("*.csv")
                if _product_code(p.stem).lower() == prod.lower()
            ]
            if hits:
                ep_rich = max(hits, key=lambda p: p.stat().st_size)
                inst = ep_rich.stem
    if ep_rich is not None:
        path = ep_rich
    if path is None:
        return {
            "error": f"no tick file for {symbol} on {day}",
            "ticks": [],
            "total": 0,
            "trading_day": day,
        }
    rows = []
    try:
        import csv

        with path.open("r", encoding="utf-8-sig", newline="") as f:
            for r in csv.DictReader(f):
                def _tf(*keys):
                    for k in keys:
                        v = r.get(k)
                        if v is None or v == "":
                            continue
                        try:
                            return float(v)
                        except (TypeError, ValueError):
                            continue
                    return 0.0

                last = _tf("LastPrice", "last_price")
                if last == 0 and not (r.get("LastPrice") or r.get("last_price")):
                    continue
                ms = int(_tf("update_millisec"))
                raw_ts = str(r.get("DateTimeStamp") or "")
                digits = "".join(ch for ch in raw_ts if ch.isdigit())
                if ms == 0 and len(digits) > 14:
                    try:
                        ms = int(digits[14:17].ljust(3, "0")[:3])
                    except ValueError:
                        ms = 0
                rows.append(
                    {
                        "datetime": r.get("datetime") or "",
                        "last": last,
                        "volume": _tf("TotalQty", "volume"),
                        "last_qty": _tf("LastQty", "last_qty"),
                        "inner_qty": _tf("InnerSideQty", "inner_qty"),
                        "outer_qty": _tf("OutSideQty", "outer_qty"),
                        "tick_inner": _tf("TickInnerQty", "tick_inner_qty"),
                        "tick_outer": _tf("TickOuterQty", "tick_outer_qty"),
                        "open_interest": _tf("PositionQty", "open_interest"),
                        "bid": _tf("BuyPrice", "bid_price1"),
                        "ask": _tf("SellPrice", "ask_price1"),
                        "bid_qty": _tf("BuyQty", "bid_volume1"),
                        "ask_qty": _tf("SellQty", "ask_volume1"),
                        "update_millisec": ms,
                    }
                )
    except Exception as e:
        return {"error": str(e), "ticks": [], "total": 0, "trading_day": day}
    if limit > 0:
        rows = rows[-limit:]
    return {
        "ticks": rows,
        "total": len(rows),
        "instrument_id": inst,
        "trading_day": day,
        "symbol": normalize_symbol(symbol),
        "path": str(path),
    }


def quant_sev_exe() -> Path | None:
    for p in (
        ROOT / "build" / "Release" / "quant_sev.exe",
        ROOT / "build" / "RelWithDebInfo" / "quant_sev.exe",
        ROOT / "build" / "Debug" / "quant_sev.exe",
        ROOT / "build" / "Release" / "quant_sev",
        ROOT / "build" / "quant_sev.exe",
        ROOT / "build" / "quant_sev",
    ):
        if p.is_file():
            return p
    return None


def _invoke_chip(
    symbol: str, mode: str, interval_min: int, lookback: int, session_only: bool = False
) -> dict:
    exe = quant_sev_exe()
    if exe is None:
        return {
            "ok": False,
            "error": "quant_sev.exe 未编译（cmake --build build --config Release）",
            "levels": [],
        }
    cmd = [str(exe)]
    cfg = ROOT / "config" / "live.env"
    if cfg.is_file():
        cmd += ["--config", str(cfg)]
    cmd += ["--chip", "--chip-mode", mode, "--chip-symbol", normalize_symbol(symbol)]
    if session_only:
        cmd += ["--chip-session"]
    tmp_path = None
    try:
        if mode == "kline":
            limit = lookback if lookback > 0 else 12000
            kl = load_klines(symbol, interval_min, limit=limit)
            bars = kl.get("klines") or []
            if not bars:
                return {"ok": False, "error": kl.get("error") or "无 K 线", "levels": []}
            fd, tmp_path = tempfile.mkstemp(prefix="chip_bars_", suffix=".csv")
            os.close(fd)
            with open(tmp_path, "w", encoding="utf-8", newline="") as f:
                w = csv.writer(f)
                w.writerow(["datetime", "open", "high", "low", "close", "volume", "open_interest"])
                for b in bars:
                    w.writerow(
                        [
                            b.get("datetime_str") or "",
                            b.get("open") or 0,
                            b.get("high") or 0,
                            b.get("low") or 0,
                            b.get("close") or 0,
                            b.get("volume") or 0,
                            b.get("open_interest") or 0,
                        ]
                    )
            cmd += ["--chip-bars-csv", tmp_path]
        r = subprocess.run(
            cmd,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=90,
        )
        text = (r.stdout or "").strip()
        line = text
        if "\n" in text:
            for ln in reversed(text.splitlines()):
                s = ln.strip()
                if s.startswith("{"):
                    line = s
                    break
        if not line.startswith("{"):
            err = ((r.stderr or "").strip() or text or f"exit {r.returncode}")[:800]
            return {"ok": False, "error": err, "levels": []}
        data = json.loads(line)
        if r.returncode != 0 and not data.get("ok"):
            data["error"] = data.get("error") or (r.stderr or "")[:400]
        return data
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "chip timeout", "levels": []}
    except Exception as e:
        return {"ok": False, "error": str(e), "levels": []}
    finally:
        if tmp_path:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass


def load_chip(
    symbol: str, mode: str = "tick", interval_min: int = 1, lookback: int = 0, session_only: bool = False
) -> dict:
    """C++ ChipProfile. Python only locates files / dumps bars; no chip math here."""
    mode = "kline" if str(mode).lower() == "kline" else "tick"
    data = _invoke_chip(
        symbol, mode, interval_min, lookback, session_only=bool(session_only)
    )
    # 分时默认 Tick；极智若只写下了报价 stub（如 IC 1 行），改用本交易时段 K 线。
    if mode == "tick" and not (data.get("levels") or []):
        fb = _invoke_chip(
            symbol,
            "kline",
            max(1, interval_min),
            lookback if lookback > 0 else 0,
            session_only=True,
        )
        if fb.get("levels"):
            fb["fallback_from"] = "tick"
            return fb
        if not data.get("error"):
            data["error"] = "Tick 不足（需要相邻成交），无法画筹码"
    return data


def load_option_screen(day: str = "") -> dict:
    """Read-only: C++ --option-screen output under data/live/option_screen/."""
    root = OPTION_SCREEN
    if not root.is_dir():
        return {"error": "no option_screen dir", "near": [], "cheap": []}
    path = None
    if day:
        cand = root / f"{day}.json"
        if cand.is_file():
            path = cand
    if path is None:
        latest = root / "latest.json"
        if latest.is_file():
            path = latest
        else:
            dated = sorted(
                [p for p in root.glob("*.json") if p.stem.isdigit()],
                key=lambda p: p.stem,
                reverse=True,
            )
            path = dated[0] if dated else None
    if path is None:
        return {"error": "run quant_sev --option-screen first", "near": [], "cheap": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        return {"error": str(e), "near": [], "cheap": []}
    data["path"] = str(path)
    return data


def bootstrap_status() -> dict:
    days = []
    if EP_RAW.is_dir():
        days = sorted(
            [p.name for p in EP_RAW.iterdir() if p.is_dir() and p.name.isdigit()],
            reverse=True,
        )
    day = days[0] if days else ""
    export_done = False
    import_done = False
    export_meta: dict = {}
    import_meta: dict = {}
    if day:
        ed = EP_RAW / day / "_export_done.json"
        im = EP_RAW / day / "_import_done.json"
        if ed.is_file():
            export_done = True
            try:
                export_meta = json.loads(ed.read_text(encoding="utf-8"))
            except Exception:
                pass
        if im.is_file():
            import_done = True
            try:
                import_meta = json.loads(im.read_text(encoding="utf-8"))
            except Exception:
                pass
    tick_days = []
    if TICKS.is_dir():
        tick_days = sorted(
            [p.name for p in TICKS.iterdir() if p.is_dir() and p.name.isdigit()],
            reverse=True,
        )
    return {
        "ep_raw_day": day,
        "export_done": export_done,
        "import_done": import_done,
        "export": export_meta,
        "import": import_meta,
        "tick_days": tick_days[:5],
        "data_root": str(DATA.resolve()),
        "hint": (
            "run VV_SevBootstrap then python tools/import_ep_to_store.py"
            if not import_done
            else "ok"
        ),
    }


def list_symbols_detailed() -> list[dict]:
    out = []
    for sym, meta in list_klines_products().items():
        for iv in meta["intervals"]:
            for year in meta["years"]:
                out.append(
                    {
                        "symbol": sym,
                        "interval_min": iv,
                        "interval": f"{iv}m",
                        "year": year,
                        "bar_count": 0,
                        "display_name": product_name(sym),
                    }
                )
    return out


def load_equity_curve(model_id: str = "") -> dict:
    curve: list[dict] = []
    models: list[str] = []
    if not DATA.exists():
        return {"curve": [], "models": []}
    files = sorted(DATA.glob("equity_*.jsonl"))
    models = [fp.stem.replace("equity_", "") for fp in files]
    targets = files
    if model_id:
        targets = [DATA / f"equity_{model_id}.jsonl"]
        models = [model_id] if (DATA / f"equity_{model_id}.jsonl").exists() else []
    for fp in targets:
        if not fp.exists():
            continue
        mid = fp.stem.replace("equity_", "")
        try:
            for line in fp.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                try:
                    row = json.loads(line)
                    row.setdefault("model", mid)
                    curve.append(row)
                except Exception:
                    pass
        except Exception:
            pass
    curve.sort(key=lambda x: x.get("time", ""))
    return {"curve": curve, "models": models}


def builtin_symbols() -> dict:
    result = []
    for sym, name in sorted(CN_NAMES.items()):
        if "." not in sym:
            continue
        ex, _ = sym.split(".", 1)
        result.append({"exchange": ex, "symbol": sym, "name": name})
    return {"symbols": result}


def strategies_list() -> dict:
    if ensure_lib_path():
        try:
            from strategies import list_strategies  # type: ignore

            return {"strategies": list_strategies()}
        except Exception as e:
            return {"strategies": [], "warning": str(e)}
    return {"strategies": [], "warning": "lib/ not found"}


def offline_status() -> dict:
    return {
        "contracts": {},
        "active_symbol": "",
        "available_models": [],
        "system_prompt": "",
        "max_concurrent": 0,
        "logs": [
            "Quant_Sev web offline mode: live CTP/AI loop not attached. "
            "Use /quote /backtest /equity. Live analyze UI is migrated for layout; APIs stubbed."
        ],
        "mode": "quant_sev_web",
    }


DOCS_HTML = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Quant_Sev Web</title>
<style>body{font-family:sans-serif;background:#0a1018;color:#dde8f0;padding:32px;line-height:1.7}
a{color:#f59e0b} code{background:#13202f;padding:2px 6px;border-radius:4px}</style></head>
<body>
<h1>Quant_Sev Web</h1>
<ul>
<li><a href="/">Quote</a></li>
<li><a href="/analyze">Analyze (offline stub)</a></li>
<li><a href="/equity">Equity</a></li>
<li><a href="/backtest">Backtest</a></li>
</ul>
<p>Data root: <code>Quant_Sev/data</code></p>
<p>Start: <code>python web/server.py</code></p>
</body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code: int, body: bytes, content_type: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self._send(code, body, "application/json; charset=utf-8")

    def _file(self, path: Path, content_type: str) -> None:
        if not path.exists():
            self._send(404, b"not found", "text/plain")
            return
        self._send(200, path.read_bytes(), content_type)

    def _read_json(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            return {}

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:  # noqa: N802
        u = urlparse(self.path)
        path = u.path
        qs = parse_qs(u.query)

        pages = {
            "/": "quote.html",
            "/quote": "quote.html",
            "/quote.html": "quote.html",
            "/equity": "equity.html",
            "/equity.html": "equity.html",
            "/backtest": "backtest.html",
            "/backtest.html": "backtest.html",
            "/analyze": "analyze.html",
            "/analyze.html": "analyze.html",
            "/app": "analyze.html",
            "/option-screen": "option_screen.html",
            "/option_screen.html": "option_screen.html",
        }
        if path in pages:
            self._file(STATIC / pages[path], "text/html; charset=utf-8")
            return
        if path == "/docs":
            self._send(200, DOCS_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return
        if path.startswith("/static/"):
            rel = STATIC / path[len("/static/") :]
            ct = "application/octet-stream"
            suf = rel.suffix.lower()
            if suf == ".js":
                ct = "application/javascript; charset=utf-8"
            elif suf == ".css":
                ct = "text/css; charset=utf-8"
            elif suf == ".json":
                ct = "application/json; charset=utf-8"
            elif suf == ".md":
                ct = "text/plain; charset=utf-8"
            elif suf in (".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico"):
                ct = {
                    ".png": "image/png",
                    ".jpg": "image/jpeg",
                    ".jpeg": "image/jpeg",
                    ".gif": "image/gif",
                    ".webp": "image/webp",
                    ".ico": "image/x-icon",
                }[suf]
            self._file(rel, ct)
            return

        # --- quote APIs ---
        if path == "/api/watchlist":
            try:
                self._json(200, build_watchlist())
            except Exception as e:
                self._json(500, {"error": str(e)})
            return
        if path in ("/api/klines", "/backtest/api/klines"):
            symbol = (qs.get("symbol") or [""])[0]
            if not symbol:
                self._json(400, {"error": "symbol required"})
                return
            try:
                iv = int((qs.get("interval_minutes") or ["5"])[0])
                limit = int((qs.get("limit") or ["12000"])[0])
                self._json(200, load_klines(symbol, iv, limit))
            except Exception as e:
                self._json(500, {"error": str(e)})
            return
        if path == "/api/ticks":
            symbol = (qs.get("symbol") or [""])[0]
            if not symbol:
                self._json(400, {"error": "symbol required"})
                return
            try:
                day = (qs.get("trading_day") or [""])[0]
                limit = int((qs.get("limit") or ["20000"])[0])
                self._json(200, load_ticks(symbol, day, limit))
            except Exception as e:
                self._json(500, {"error": str(e)})
            return
        if path == "/api/option-screen":
            try:
                day = (qs.get("trading_day") or qs.get("day") or [""])[0]
                self._json(200, load_option_screen(day))
            except Exception as e:
                self._json(500, {"error": str(e), "near": [], "cheap": []})
            return
        if path == "/api/chip":
            symbol = (qs.get("symbol") or [""])[0]
            if not symbol:
                self._json(400, {"error": "symbol required", "levels": []})
                return
            try:
                mode = (qs.get("mode") or ["tick"])[0]
                iv = int((qs.get("interval_minutes") or ["1"])[0])
                lookback = int((qs.get("lookback") or ["0"])[0])
                session_only = (qs.get("session_only") or ["0"])[0].lower() in ("1", "true", "yes")
                self._json(200, load_chip(symbol, mode, iv, lookback, session_only))
            except Exception as e:
                self._json(500, {"error": str(e), "levels": []})
            return
        if path == "/api/bootstrap":
            self._json(200, bootstrap_status())
            return
        if path == "/api/health":
            self._json(
                200,
                {
                    "ok": True,
                    "root": str(ROOT),
                    "data": str(DATA),
                    "pages": sorted(p.name for p in STATIC.glob("*.html")),
                    "lib": LIB_ROOT.exists(),
                },
            )
            return

        # --- equity ---
        if path == "/api/equity_curve":
            model = (qs.get("model") or [""])[0]
            self._json(200, load_equity_curve(model))
            return

        # --- backtest ---
        if path == "/backtest/api/symbols":
            self._json(200, {"symbols": list_symbols_detailed()})
            return
        if path == "/backtest/api/strategies":
            self._json(200, strategies_list())
            return
        if path == "/backtest/api/builtin-symbols":
            self._json(200, builtin_symbols())
            return
        if path == "/backtest/api/simulations":
            self._json(200, {"simulations": []})
            return
        if path == "/backtest/api/history_trades":
            self._json(200, {"trades": []})
            return
        if path == "/backtest/api/stats":
            self._json(200, {"stats": {}})
            return
        if path.startswith("/backtest/api/download/progress/"):
            tid = path.rsplit("/", 1)[-1]
            self._json(200, _download_tasks.get(tid, {"status": "unknown"}))
            return

        # --- analyze (offline stubs so UI boots) ---
        if path == "/api/status":
            self._json(200, offline_status())
            return
        if path == "/api/summary":
            self._json(200, {"items": [], "mode": "offline"})
            return
        if path == "/api/settings":
            self._json(200, {"ok": True, "settings": {}})
            return
        if path == "/api/prices":
            self._json(200, {"symbol": (qs.get("symbol") or [""])[0], "quote": {}, "error": "offline"})
            return
        if path == "/api/analysis":
            self._json(200, {"analysis": None, "mode": "offline"})
            return
        if path == "/api/trading":
            self._json(200, {"positions": [], "orders": [], "mode": "offline"})
            return
        if path == "/api/orders":
            self._json(200, {"orders": []})
            return
        if path == "/api/trades":
            self._json(200, {"trades": []})
            return
        if path == "/api/history":
            self._json(200, {"history": []})
            return
        if path == "/api/stream/prices":
            # minimal SSE heartbeat
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            try:
                self.wfile.write(b": offline\n\n")
                self.wfile.flush()
            except Exception:
                pass
            return

        if path.startswith("/api/") or path.startswith("/backtest/api/"):
            self._json(404, {"error": "not found", "path": path, "levels": []})
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:  # noqa: N802
        u = urlparse(self.path)
        path = u.path
        body = self._read_json()

        if path == "/api/settings":
            self._json(200, {"ok": True, "settings": body})
            return
        if path in (
            "/api/contracts/add",
            "/api/contracts/remove",
            "/api/set_active",
            "/api/trigger",
            "/api/start",
            "/api/stop",
            "/api/close_position",
            "/api/cancel_order",
            "/api/guard",
        ):
            self._json(200, {"ok": False, "message": "offline stub ?? live trading is Quant_Sev C++ path", "echo": body})
            return

        if path == "/backtest/api/download":
            self._json(
                400,
                {
                    "error": "tqsdk_download_disabled",
                    "message": "天勤历史下载已废弃。请用极智 VV_SevBootstrap + import_ep_to_store。",
                    "steps": [
                        "powershell tools/ep/sync_to_jizhi.ps1",
                        "在极智运行 VV_SevBootstrap",
                        "python tools/import_ep_to_store.py",
                    ],
                },
            )
            return

        if path in ("/backtest/api/precompute", "/backtest/api/analyze", "/backtest/api/simulate"):
            if not ensure_lib_path():
                self._json(501, {"error": "AI backtest needs ../lib on PYTHONPATH (strategies/analyzers)"})
                return
            try:
                # Lazy bridge: call into lib engine for structure only when possible
                from backtest import engine as bt_engine  # type: ignore

                symbol = body.get("symbol", "")
                iv = int(body.get("interval_minutes") or 5)
                df = bt_engine.load_klines(symbol, duration_seconds=iv * 60, data_dir=KLINES)
                self._json(
                    200,
                    {
                        "ok": True,
                        "mode": "partial",
                        "message": "lib engine loaded klines; full AI analyze still requires lib FastAPI stack",
                        "bars": len(df),
                        "symbol": symbol,
                        "request": body,
                    },
                )
            except Exception as e:
                self._json(501, {"error": str(e), "hint": "Install lib deps or use historical chart only"})
            return

        self._json(404, {"error": "not found"})

    def do_PATCH(self) -> None:  # noqa: N802
        self.do_POST()

        u = urlparse(self.path)
        path = u.path
        if path.startswith("/backtest/api/session"):
            self._json(200, {"ok": True})
            return
        if path.startswith("/backtest/api/download/"):
            tid = path.rsplit("/", 1)[-1]
            _download_tasks.pop(tid, None)
            self._json(200, {"ok": True})
            return
        self._json(404, {"error": "not found"})


class ExclusiveHTTPServer(ThreadingHTTPServer):
    # Windows SO_REUSEADDR lets two processes bind 8765 → ERR_EMPTY_RESPONSE.
    allow_reuse_address = False

    def server_bind(self) -> None:
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def main() -> None:
    ap = argparse.ArgumentParser(description="Quant_Sev web server")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    missing = [n for n in ("quote.html", "equity.html", "backtest.html", "analyze.html") if not (STATIC / n).exists()]
    if missing:
        print("missing pages:", missing, file=sys.stderr)
        print("run: python web/_migrate_pages.py", file=sys.stderr)
        sys.exit(1)
    print(f"Quant_Sev web  http://{args.host}:{args.port}/")
    print(f"  quote=/  analyze=/analyze  equity=/equity  backtest=/backtest")
    print(f"  data={DATA}  lib={LIB_ROOT.exists()}")
    try:
        httpd = ExclusiveHTTPServer((args.host, args.port), Handler)
    except OSError as e:
        print(f"bind failed {args.host}:{args.port}: {e}", file=sys.stderr)
        sys.exit(1)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
