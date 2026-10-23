#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Import 极智 ep_raw into parquet klines only.

  极智原始 Tick/M1/M5/D 永远留在 data/live/ep_raw/，不要拷进 data/ticks/。
  data/ticks/{TradingDay}/ 只由 CTP TickDayStore（--live-ticks）按交易日写。
  K 线 OHLC 只信极智 M1/M5/D，不要用 Tick 合成去覆盖。

  cd C:\\Quant_Sev
  python tools/import_ep_to_store.py
  python tools/import_ep_to_store.py --day 20260821

Sources:
  data/live/ep_raw/{YYYYMMDD}/m1|m5|d/<InstrumentID>.csv

Targets:
  data/klines/{EXCHANGE.product}/1m|5m|1440m/{year}.parquet
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from klines_parquet_write import write_merge  # noqa: E402

DATA = ROOT / "data"
EP_RAW = DATA / "live" / "ep_raw"
DOMINANT = DATA / "live" / "dominant"
KLINES = DATA / "klines"
TICKS = DATA / "ticks"

CTP_TICK_HEADER = (
    "datetime,exchange_id,instrument_id,trading_day,action_day,update_time,update_millisec,"
    "last_price,volume,turnover,open_interest,pre_open_interest,"
    "bid_price1,bid_volume1,bid_price2,bid_volume2,bid_price3,bid_volume3,bid_price4,"
    "bid_volume4,bid_price5,bid_volume5,"
    "ask_price1,ask_volume1,ask_price2,ask_volume2,ask_price3,ask_volume3,ask_price4,"
    "ask_volume4,ask_price5,ask_volume5,"
    "last_qty,inner_qty,outer_qty,tick_inner_qty,tick_outer_qty"
)


def latest_ep_day() -> str | None:
    if not EP_RAW.is_dir():
        return None
    days = sorted(
        [p.name for p in EP_RAW.iterdir() if p.is_dir() and len(p.name) == 8 and p.name.isdigit()],
        reverse=True,
    )
    return days[0] if days else None


def _product_from_symbol(symbol: str) -> str:
    s = symbol.strip()
    i = len(s)
    while i > 0 and s[i - 1].isdigit():
        i -= 1
    return s[:i]


def meta_from_csv_header_row(path: Path, id_map: dict[str, dict]) -> dict | None:
    """Resolve instrument -> product_key using dominant map or CSV exchange column."""
    inst = path.stem
    meta = id_map.get(inst) or next(
        (v for k, v in id_map.items() if k.upper() == inst.upper()), None
    )
    if meta:
        return meta
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as f:
            reader = csv.DictReader(f)
            row = next(reader, None)
            if not row:
                return None
            exchange = (row.get("exchange") or "").strip().upper()
            if exchange == "ZCE":
                exchange = "CZCE"
            product = _product_from_symbol(inst)
            if not exchange or not product:
                return None
            return {
                "exchange": exchange,
                "product": product,
                "product_key": f"{exchange}.{product}",
            }
    except Exception:
        return None


def stage_from_quant_vv(vv_root: Path, day: str = "") -> str:
    """Copy Quant_VV CSV/{day}/m1_ep|tick into Quant_Sev ep_raw/{day}/m1|tick."""
    vv_csv = vv_root / "CSV"
    if not vv_csv.is_dir():
        raise SystemExit(f"not found: {vv_csv}")
    if not day:
        days = sorted(
            [p.name for p in vv_csv.iterdir() if p.is_dir() and len(p.name) == 8 and p.name.isdigit()],
            reverse=True,
        )
        if not days:
            raise SystemExit(f"no YYYYMMDD under {vv_csv}")
        day = days[0]
    src_m1 = vv_csv / day / "m1_ep"
    src_tick = vv_csv / day / "tick"
    if not src_m1.is_dir() and not src_tick.is_dir():
        raise SystemExit(f"no m1_ep/tick under {vv_csv / day}")
    dst_m1 = EP_RAW / day / "m1"
    dst_tick = EP_RAW / day / "tick"
    import shutil

    if src_m1.is_dir():
        dst_m1.mkdir(parents=True, exist_ok=True)
        n = 0
        for f in src_m1.glob("*.csv"):
            shutil.copy2(f, dst_m1 / f.name)
            n += 1
        print(f"staged m1 {n} files -> {dst_m1}")
    if src_tick.is_dir():
        dst_tick.mkdir(parents=True, exist_ok=True)
        n = 0
        for f in src_tick.glob("*.csv"):
            shutil.copy2(f, dst_tick / f.name)
            n += 1
        print(f"staged tick {n} files -> {dst_tick}")
    done = EP_RAW / day / "_export_done.json"
    done.write_text(
        json.dumps(
            {
                "trading_day": day,
                "source": "quant_vv",
                "vv_root": str(vv_root),
                "ts": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"wrote {done}")
    return day


def load_dominant_map(day: str) -> dict[str, dict]:
    """instrument_id -> {exchange, product, product_key}"""
    out: dict[str, dict] = {}
    cands = [DOMINANT / f"{day}.json", DOMINANT / "latest.json"]
    if DOMINANT.is_dir():
        cands.extend(sorted(DOMINANT.glob("*.json"), reverse=True))
    seen: set[Path] = set()
    for cand in cands:
        if cand in seen or not cand.is_file():
            continue
        seen.add(cand)
        try:
            data = json.loads(cand.read_text(encoding="utf-8-sig"))
        except Exception:
            continue
        for pk, row in (data.get("products") or {}).items():
            if not isinstance(row, dict):
                continue
            parts = str(pk).split(".", 1)
            if len(parts) != 2:
                continue
            ex, prod = parts[0], parts[1]
            for key in ("dominant",):
                sym = (row.get(key) or "").strip()
                if sym:
                    out[sym] = {
                        "exchange": ex,
                        "product": prod,
                        "product_key": pk,
                    }
        if out:
            break
    return out


def parse_ep_datetime(s: str) -> datetime | None:
    s = (s or "").strip().replace("-", "/")
    for fmt in ("%Y/%m/%d %H:%M:%S", "%Y-%m-%d %H:%M:%S", "%Y%m%d%H%M%S"):
        try:
            digits = "".join(ch for ch in s if ch.isdigit())
            if fmt == "%Y%m%d%H%M%S":
                if len(digits) < 14:
                    continue
                return datetime.strptime(digits[:14], fmt)
            return datetime.strptime(s[:19], fmt.replace("-", "/"))
        except Exception:
            continue
    digits = "".join(ch for ch in s if ch.isdigit())
    if len(digits) >= 14:
        try:
            return datetime.strptime(digits[:14], "%Y%m%d%H%M%S")
        except Exception:
            return None
    return None


def import_period(day: str, id_map: dict[str, dict], src_subdir: str, interval_min: int) -> dict:
    """Import 极智 K-line CSV → data/klines/{product}/{Nm}/{year}.parquet.

    Same-minute duplicates keep last (极智 wins if imported after a dirty merge).
    """
    src_dir = EP_RAW / day / src_subdir
    stats = {"files": 0, "rows": 0, "skipped": 0, "products": set()}
    if not src_dir.is_dir():
        return {**stats, "products": []}
    close_sec = int(interval_min) * 60
    for path in sorted(src_dir.glob("*.csv")):
        if path.name.startswith("_"):
            continue
        inst = path.stem
        meta = meta_from_csv_header_row(path, id_map)
        if not meta:
            print(f"skip {src_subdir} {inst}: cannot resolve product")
            stats["skipped"] += 1
            continue
        product_key = meta["product_key"]
        rows = []
        with path.open("r", encoding="utf-8-sig", newline="") as f:
            reader = csv.DictReader(f)
            for r in reader:
                dt = parse_ep_datetime(r.get("datetime") or "")
                if dt is None:
                    continue
                try:
                    o = float(r.get("open") or 0)
                    h = float(r.get("high") or 0)
                    l = float(r.get("low") or 0)
                    c = float(r.get("close") or 0)
                    v = float(r.get("volume") or 0)
                except (TypeError, ValueError):
                    continue
                import pandas as pd

                open_ts = pd.Timestamp(dt).tz_localize("Asia/Shanghai")
                time_unix = int(open_ts.timestamp()) + close_sec
                rows.append(
                    {
                        "datetime": int(open_ts.value),
                        "open": o,
                        "high": h,
                        "low": l,
                        "close": c,
                        "volume": v,
                        "time_unix": time_unix,
                    }
                )
        if not rows:
            continue
        import pandas as pd

        by_year: dict[int, list] = {}
        for row in rows:
            y = int(pd.Timestamp(row["datetime"], unit="ns", tz="UTC").tz_convert("Asia/Shanghai").year)
            by_year.setdefault(y, []).append(row)
        for y, yrows in by_year.items():
            out = KLINES / product_key / f"{interval_min}m" / f"{y}.parquet"
            n = write_merge(out, yrows)
            stats["rows"] += n
            print(f"{src_subdir} {inst} -> {out} rows={n}")
        stats["files"] += 1
        stats["products"].add(product_key)
    stats["products"] = sorted(stats["products"])
    return stats


def import_m1(day: str, id_map: dict[str, dict]) -> dict:
    return import_period(day, id_map, "m1", 1)


def parse_wall(s: str, stamp: str = "") -> datetime | None:
    """Parse CTP or 极智 datetime, keeping milliseconds."""
    for raw in (stamp, s):
        digits = "".join(ch for ch in (raw or "") if ch.isdigit())
        if len(digits) < 14:
            continue
        try:
            dt = datetime.strptime(digits[:14], "%Y%m%d%H%M%S")
        except ValueError:
            continue
        ms = 0
        if len(digits) > 14:
            try:
                ms = int(digits[14:17].ljust(3, "0")[:3])
            except ValueError:
                ms = 0
        return dt.replace(microsecond=ms * 1000)
    return parse_ep_datetime(s) or parse_ep_datetime(stamp)


def _ticks_path_for(day: str, inst: str) -> Path | None:
    day_dir = TICKS / day
    if not day_dir.is_dir():
        return None
    direct = day_dir / f"{inst}.csv"
    if direct.is_file():
        return direct
    want = inst.upper()
    for p in day_dir.glob("*.csv"):
        if p.stem.upper() == want:
            return p
    return None


def _ep_to_ctp_row(r: dict, inst: str, exchange: str, day: str, header: list[str]) -> list[str] | None:
    dt = parse_wall(r.get("datetime") or "", r.get("DateTimeStamp") or "")
    if dt is None:
        return None
    ms = dt.microsecond // 1000
    trade_date = (r.get("TradeDate") or day).replace("/", "").replace("-", "")[:8] or day
    action_day = dt.strftime("%Y%m%d")
    exch = (exchange or r.get("exchange") or "").strip()
    if exch.upper() == "ZCE":
        exch = "CZCE"
    values = {
        "datetime": dt.strftime("%Y-%m-%d %H:%M:%S") + f".{ms:03d}",
        "exchange_id": exch,
        "instrument_id": inst,
        "trading_day": trade_date,
        "action_day": action_day,
        "update_time": dt.strftime("%H:%M:%S"),
        "update_millisec": str(ms),
        "last_price": r.get("LastPrice") or "0",
        "volume": r.get("TotalQty") or "0",
        "turnover": "0",
        "open_interest": r.get("PositionQty") or "0",
        "pre_open_interest": "0",
        "bid_price1": r.get("BuyPrice") or "0",
        "bid_volume1": r.get("BuyQty") or "0",
        "ask_price1": r.get("SellPrice") or "0",
        "ask_volume1": r.get("SellQty") or "0",
        "last_qty": r.get("LastQty") or "0",
        "inner_qty": r.get("InnerSideQty") or "0",
        "outer_qty": r.get("OutSideQty") or "0",
        "tick_inner_qty": r.get("TickInnerQty") or "0",
        "tick_outer_qty": r.get("TickOuterQty") or "0",
    }
    return [values.get(col, "0") for col in header]


def fill_tick_holes(day: str, id_map: dict[str, dict], gap_sec: float = 60.0) -> dict:
    """Insert 极智 ticks into CTP holes only. Existing CTP rows are kept as-is.

    Does not create new data/ticks files. Rewrites matching CSVs in datetime order.
    """
    tick_dir = EP_RAW / day / "tick"
    stats = {"files": 0, "rows": 0, "skipped": 0, "missing_ctp": 0}
    if not tick_dir.is_dir():
        print(f"no ep_raw tick dir {tick_dir}")
        return stats
    for path in sorted(tick_dir.glob("*.csv")):
        if path.name.startswith("_"):
            continue
        inst = path.stem
        out_path = _ticks_path_for(day, inst)
        if out_path is None:
            stats["missing_ctp"] += 1
            continue
        meta = id_map.get(inst) or next(
            (v for k, v in id_map.items() if k.upper() == inst.upper()), None
        )
        exchange = (meta or {}).get("exchange", "")
        with out_path.open("r", encoding="utf-8-sig", newline="") as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if not header:
                stats["skipped"] += 1
                continue
            i_dt = header.index("datetime") if "datetime" in header else 0
            ctp_items: list[tuple[datetime, list[str]]] = []
            for row in reader:
                if not row or len(row) < 2:
                    continue
                dt = parse_wall(row[i_dt] if i_dt < len(row) else "")
                if dt is None:
                    continue
                ctp_items.append((dt, row))
        if not ctp_items:
            stats["skipped"] += 1
            continue
        ctp_items.sort(key=lambda x: x[0])
        gaps: list[tuple[datetime, datetime]] = []
        for (t0, _), (t1, _) in zip(ctp_items, ctp_items[1:]):
            if (t1 - t0).total_seconds() >= gap_sec:
                gaps.append((t0, t1))
        if not gaps:
            stats["skipped"] += 1
            continue
        inserts: list[tuple[datetime, list[str]]] = []
        with path.open("r", encoding="utf-8-sig", newline="") as f:
            for r in csv.DictReader(f):
                dt = parse_wall(r.get("datetime") or "", r.get("DateTimeStamp") or "")
                if dt is None:
                    continue
                in_gap = any(t0 < dt < t1 for t0, t1 in gaps)
                if not in_gap:
                    continue
                row = _ep_to_ctp_row(r, inst, exchange, day, header)
                if row is None:
                    continue
                inserts.append((dt, row))
        if not inserts:
            stats["skipped"] += 1
            continue
        merged = ctp_items + inserts
        merged.sort(key=lambda x: x[0])
        tmp = out_path.with_suffix(".csv.tmp")
        with tmp.open("w", encoding="utf-8", newline="") as f:
            f.write("\ufeff")
            w = csv.writer(f)
            w.writerow(header)
            for _, row in merged:
                w.writerow(row)
        try:
            tmp.replace(out_path)
        except OSError:
            out_path.write_bytes(tmp.read_bytes())
            tmp.unlink(missing_ok=True)
        stats["files"] += 1
        stats["rows"] += len(inserts)
        print(f"tick-hole {inst} -> {out_path.name} +{len(inserts)} (gaps={len(gaps)})")
    return stats


def import_ticks(day: str, id_map: dict[str, dict]) -> dict:
    """Hole-fill only. See fill_tick_holes."""
    return fill_tick_holes(day, id_map)


def _load_session_segments() -> dict[str, list]:
    path = DATA / "live" / "sessions" / "latest.json"
    if not path.is_file():
        return {}
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    out = {}
    for pk, segs in (raw.get("products") or {}).items():
        if isinstance(segs, list):
            out[pk] = segs
    return out


def _hms_sec_imp(s: str) -> int:
    parts = str(s or "00:00:00").split(":")
    try:
        hh = int(parts[0]) if parts else 0
        mm = int(parts[1]) if len(parts) > 1 else 0
        ss = int(parts[2]) if len(parts) > 2 else 0
    except ValueError:
        return 0
    return hh * 3600 + mm * 60 + ss


def _sod_in_segments(sod: int, segs: list) -> bool:
    if not segs:
        return True
    for s in segs:
        b = _hms_sec_imp(s.get("time_begin") or "")
        e = _hms_sec_imp(s.get("time_end") or "")
        if e < b:
            if sod >= b or sod <= e:
                return True
        elif b <= sod <= e:
            return True
    return False


def import_tick_m1(day: str, id_map: dict[str, dict]) -> dict:
    """Deprecated: do not fold Tick into OHLC. Kept behind --fold-ticks."""
    import pandas as pd

    tick_dir = EP_RAW / day / "tick"
    stats = {"files": 0, "rows": 0, "skipped": 0}
    if not tick_dir.is_dir():
        return stats
    session_map = _load_session_segments()
    for path in sorted(tick_dir.glob("*.csv")):
        if path.name.startswith("_"):
            continue
        inst = path.stem
        meta = id_map.get(inst) or next(
            (v for k, v in id_map.items() if k.upper() == inst.upper()), None
        )
        if not meta:
            meta = meta_from_csv_header_row(path, id_map)
        if not meta:
            stats["skipped"] += 1
            continue
        product_key = meta["product_key"]
        try:
            df = pd.read_csv(path, encoding="utf-8-sig")
        except Exception:
            stats["skipped"] += 1
            continue
        if df.empty:
            continue
        cols = {c.lower(): c for c in df.columns}
        dt_col = cols.get("datetime")
        last_col = cols.get("lastprice") or cols.get("last_price")
        qty_col = cols.get("lastqty") or cols.get("last_qty")
        if not dt_col or not last_col:
            stats["skipped"] += 1
            continue
        tmp = pd.DataFrame(
            {
                "datetime": pd.to_datetime(df[dt_col], errors="coerce"),
                "last": pd.to_numeric(df[last_col], errors="coerce"),
                "qty": pd.to_numeric(df[qty_col], errors="coerce").fillna(0) if qty_col else 0,
            }
        ).dropna(subset=["datetime"])
        tmp = tmp[tmp["last"] > 0]
        if tmp.empty:
            continue
        tmp["bar"] = tmp["datetime"].dt.floor("min")
        g = tmp.groupby("bar", sort=True)
        bars = g.agg(
            open=("last", "first"),
            high=("last", "max"),
            low=("last", "min"),
            close=("last", "last"),
            volume=("qty", "sum"),
        ).reset_index()
        segs = session_map.get(product_key) or []
        if segs:
            sod = (
                bars["bar"].dt.hour * 3600
                + bars["bar"].dt.minute * 60
                + bars["bar"].dt.second
            )
            keep = sod.map(lambda x: _sod_in_segments(int(x), segs))
            bars = bars.loc[keep].reset_index(drop=True)
        if bars.empty:
            continue
        rows = []
        for r in bars.itertuples(index=False):
            open_ts = pd.Timestamp(r.bar).tz_localize("Asia/Shanghai")
            rows.append(
                {
                    "datetime": int(open_ts.value),
                    "open": float(r.open),
                    "high": float(r.high),
                    "low": float(r.low),
                    "close": float(r.close),
                    "volume": float(r.volume),
                    "time_unix": int(open_ts.timestamp()) + 60,
                }
            )
        by_year: dict[int, list] = {}
        for row in rows:
            y = int(pd.Timestamp(row["datetime"], unit="ns", tz="UTC").tz_convert("Asia/Shanghai").year)
            by_year.setdefault(y, []).append(row)
        for y, yrows in by_year.items():
            out = KLINES / product_key / "1m" / f"{y}.parquet"
            n = write_merge(out, yrows)
            stats["rows"] += n
            print(f"tick-m1 {inst} -> {out} merged_rows={n}")
        stats["files"] += 1
    return stats


def write_import_marker(day: str, stats: dict) -> None:
    path = EP_RAW / day / "_import_done.json"
    path.write_text(
        json.dumps(
            {
                "trading_day": day,
                **{k: (list(v) if isinstance(v, set) else v) for k, v in stats.items()},
                "ts": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"marker {path}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Import 极智 ep_raw M1/M5/D into parquet klines (not data/ticks)")
    ap.add_argument("--day", default="", help="YYYYMMDD (default: latest ep_raw)")
    ap.add_argument("--from-vv", default="", help="Quant_VV root; stage CSV/{day}/m1_ep+tick then import")
    ap.add_argument("--skip-m1", action="store_true")
    ap.add_argument("--skip-m5", action="store_true")
    ap.add_argument("--skip-d", action="store_true")
    ap.add_argument(
        "--fold-ticks",
        action="store_true",
        help="DANGER: fold 极智 tick into 1m OHLC (will dirty wicks/volume). Off by default.",
    )
    ap.add_argument(
        "--fill-tick-holes",
        action="store_true",
        help="Insert 极智 ep_raw ticks into CTP data/ticks gaps only (keep CTP rows).",
    )
    ap.add_argument("--gap-sec", type=float, default=60.0, help="Min CTP hole (seconds) to fill")
    args = ap.parse_args()
    day = args.day.strip()
    if args.from_vv:
        day = stage_from_quant_vv(Path(args.from_vv), day)
    else:
        day = day or (latest_ep_day() or "")
    if not day:
        print("no ep_raw day found under", EP_RAW)
        return 1
    done = EP_RAW / day / "_export_done.json"
    if not done.is_file():
        print(f"warn: missing {done} (import anyway)")
    id_map = load_dominant_map(day)
    if args.fill_tick_holes:
        print(f"day={day} fill-tick-holes gap_sec={args.gap_sec}")
        stats = fill_tick_holes(day, id_map, gap_sec=args.gap_sec)
        print(json.dumps({"ok": True, "day": day, "fill_tick_holes": stats}, ensure_ascii=False))
        return 0
    if not id_map:
        print("error: dominant map empty; run --select-dominant first")
        return 2
    print(f"day={day} instruments_in_map={len(id_map)}")
    out_stats = {
        "m1": {"files": 0, "rows": 0, "skipped": 0, "products": []},
        "m5": {"files": 0, "rows": 0, "skipped": 0, "products": []},
        "d": {"files": 0, "rows": 0, "skipped": 0, "products": []},
    }
    if not args.skip_m1:
        out_stats["m1"] = import_period(day, id_map, "m1", 1)
    if not args.skip_m5:
        out_stats["m5"] = import_period(day, id_map, "m5", 5)
    if not args.skip_d:
        out_stats["d"] = import_period(day, id_map, "d", 1440)
    if args.fold_ticks:
        out_stats["tick_m1"] = import_tick_m1(day, id_map)
    write_import_marker(day, out_stats)
    print(json.dumps({"ok": True, "day": day, **out_stats}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
