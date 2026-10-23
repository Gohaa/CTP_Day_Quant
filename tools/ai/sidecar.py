"""
只读 AI 侧车。读 C++ 写出的 MarketSnapshot，写 TradeSignal JSON。
不接 CTP，不下单。默认不调外部模型（无 Key 时用快照四模块分数映射 观望/做多/做空）。
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _num(d: dict, *keys):
    for k in keys:
        if k in d and d[k] is not None:
            try:
                return float(d[k])
            except (TypeError, ValueError):
                continue
    return None


def decide(snap: dict) -> dict:
    mods = snap.get("modules") or {}
    s = int(mods.get("sum") or 0)
    last = _num(snap, "last") or 0.0
    tick = _num(snap, "price_tick") or 1.0
    action = "观望"
    if s >= 60:
        action = "做多"
    elif s <= -60:
        action = "做空"
    entry = last
    sl = None
    tp = None
    if action == "做多" and last > 0:
        sl = last - 10 * tick
        tp = last + 20 * tick
    if action == "做空" and last > 0:
        sl = last + 10 * tick
        tp = last - 20 * tick
    return {
        "action": action,
        "操作建议": action,
        "entry_price": entry,
        "入场价": entry,
        "stop_loss": sl,
        "止损价": sl,
        "take_profit": tp,
        "止盈价": tp,
        "start_pyramid": False,
        "source": "sidecar_local_modules",
        "product_key": snap.get("product_key"),
        "instrument_id": snap.get("instrument_id"),
        "reason": f"modules_sum={s} (no LLM)",
    }


def main() -> int:
    p = argparse.ArgumentParser(description="Quant_Sev read-only AI sidecar")
    p.add_argument("--data-root", default="data")
    p.add_argument("--product", default="", help="e.g. SHFE.rb; empty = latest snapshot")
    args = p.parse_args()
    root = Path(args.data_root) / "live" / "signal"
    if args.product:
        snap_path = root / (args.product + ".snapshot.json")
    else:
        snap_path = root / "latest.snapshot.json"
    if not snap_path.is_file():
        print(f"missing snapshot: {snap_path}", file=sys.stderr)
        return 2
    snap = json.loads(snap_path.read_text(encoding="utf-8"))
    sig = decide(snap)
    out = root / ((args.product or snap.get("product_key") or "latest") + ".signal.json")
    root.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(sig, ensure_ascii=False, indent=2), encoding="utf-8")
    latest = root / "latest.signal.json"
    latest.write_text(json.dumps(sig, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"ok": True, "wrote": str(out), "action": sig["action"]}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
