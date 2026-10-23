"""
price_action.py — Brooks / 价格行为策略插件。

按 base.py + prompts.py 重建（原文件缺失）。纯函数：无 I/O，不接 CTP / 天勤。
实盘发单只走 C++ OMS；本模块给只读侧车做特征与 Prompt。
"""

from __future__ import annotations

import pandas as pd

from strategies.base import BaseStrategy
from strategies import prompts as P


_DEFAULTS: dict = {
    "近期波段高点": [],
    "近期波段低点": [],
    "测量目标向上": [],
    "测量目标向下": [],
    "回调腿标签": "未知",
    "回调深度%": 0.0,
    "回调根数": 0,
    "出现顺势实体棒": False,
    "交易区间上沿": None,
    "交易区间下沿": None,
    "价格在区间位置": None,
    "是否窄幅区间": False,
    "前时段高点": None,
    "前时段低点": None,
    "主趋势线价位": None,
    "主趋势线突破": False,
    "趋势末期警告": False,
    "回调形态": None,
    "is_limit_up": False,
    "is_limit_down": False,
    "区间中点": None,
    "最近突破价位": None,
    "突破后K线数": 0,
    "突破方向": None,
    "杯柄形态": False,
    "柄深比例%": None,
    "缺口方向": None,
    "缺口磁铁价位": None,
    "缺口幅度%": None,
    "区间突破失败率%": None,
    "楔形反转类型": None,
    "趋势方向": "震荡",
    "_ema斜率_10根": None,
    "_ema斜率_5根": None,
    "_趋势阈值": None,
    "_价格EMA偏离%": None,
}


def _local_extrema(series: pd.Series, kind: str, win: int = 5, keep: int = 3) -> list:
    if series is None or len(series) < win * 2:
        return []
    v = series.astype(float)
    out = []
    for i in range(win, len(v) - win):
        w = v.iloc[i - win : i + win + 1]
        x = float(v.iloc[i])
        if kind == "high" and x >= float(w.max()) - 1e-12:
            out.append(round(x, 4))
        if kind == "low" and x <= float(w.min()) + 1e-12:
            out.append(round(x, 4))
    if len(out) > keep:
        out = out[-keep:]
    return out


class BrooksStrategy(BaseStrategy):
    @property
    def strategy_id(self) -> str:
        return "brooks"

    @property
    def price_structure_defaults(self) -> dict:
        return _DEFAULTS

    def build_price_structure(self, df: pd.DataFrame) -> dict:
        out = {k: (list(v) if isinstance(v, list) else v) for k, v in _DEFAULTS.items()}
        out["ema20"] = {}
        if df is None or len(df) < 20 or "close" not in df.columns:
            return out

        close = df["close"].astype(float)
        high = df["high"].astype(float) if "high" in df.columns else close
        low = df["low"].astype(float) if "low" in df.columns else close
        ema = close.ewm(span=20, adjust=False).mean()
        last = float(close.iloc[-1])
        ema_last = float(ema.iloc[-1])
        dist_pct = (last - ema_last) / ema_last * 100.0 if ema_last else 0.0
        slope10 = float(ema.iloc[-1] - ema.iloc[-10]) if len(ema) >= 10 else 0.0
        slope5 = float(ema.iloc[-1] - ema.iloc[-5]) if len(ema) >= 5 else 0.0

        out["ema20"] = {
            "EMA20": round(ema_last, 4),
            "距EMA幅度%": round(dist_pct, 4),
            "价格在EMA附近": abs(dist_pct) <= 0.5,
        }
        out["_ema_values"] = [None if pd.isna(x) else float(x) for x in ema.tolist()]
        out["_ema斜率_10根"] = round(slope10, 6)
        out["_ema斜率_5根"] = round(slope5, 6)
        out["_价格EMA偏离%"] = round(dist_pct, 4)
        out["_趋势阈值"] = 0.5

        rng = float(high.tail(20).max() - low.tail(20).min())
        out["是否窄幅区间"] = bool(last > 0 and rng / last < 0.012)
        out["交易区间上沿"] = round(float(high.tail(20).max()), 4)
        out["交易区间下沿"] = round(float(low.tail(20).min()), 4)
        mid = (out["交易区间上沿"] + out["交易区间下沿"]) / 2.0
        out["区间中点"] = round(mid, 4)
        span = out["交易区间上沿"] - out["交易区间下沿"]
        if span > 0:
            out["价格在区间位置"] = round((last - out["交易区间下沿"]) / span, 3)

        out["近期波段高点"] = _local_extrema(high, "high")
        out["近期波段低点"] = _local_extrema(low, "low")
        if out["近期波段高点"]:
            h = out["近期波段高点"][-1]
            out["测量目标向上"] = [round(h + (h - last), 4)] if last < h else []
        if out["近期波段低点"]:
            lo = out["近期波段低点"][-1]
            out["测量目标向下"] = [round(lo - (last - lo), 4)] if last > lo else []

        if dist_pct > 0.8 and slope10 > 0:
            out["趋势方向"] = "上升"
            out["趋势强度分类"] = "强上升趋势" if dist_pct > 1.5 else "弱上升趋势"
        elif dist_pct < -0.8 and slope10 < 0:
            out["趋势方向"] = "下降"
            out["趋势强度分类"] = "强下降趋势" if dist_pct < -1.5 else "弱下降趋势"
        else:
            out["趋势方向"] = "震荡"
            out["趋势强度分类"] = "交易区间"
        return out

    def build_features(
        self,
        market_data: dict,
        lang: str,
        klines_completed: list[dict] | None = None,
    ) -> dict:
        bars = klines_completed or market_data.get("klines") or []
        last_bar = bars[-1] if bars else {}
        prev = bars[-2] if len(bars) > 1 else None
        rating = self.rate_bar(last_bar, prev) if last_bar else "十字星"
        feat = {
            "趋势强度分类": market_data.get("趋势强度分类") or market_data.get("趋势方向") or "交易区间",
            "趋势方向": market_data.get("趋势方向"),
            "距EMA幅度%": (market_data.get("ema20") or {}).get("距EMA幅度%"),
            "价格在EMA附近": (market_data.get("ema20") or {}).get("价格在EMA附近"),
            "回调腿标签": market_data.get("回调腿标签"),
            "是否窄幅区间": market_data.get("是否窄幅区间"),
            "_last_bar_rating": rating,
        }
        if lang == "en":
            feat["trend"] = feat["趋势方向"]
        return feat

    def get_skip_struct(self) -> frozenset[str]:
        keys = set(_DEFAULTS.keys())
        keys.update({"ema20", "is_limit_up", "is_limit_down", "趋势强度分类"})
        return frozenset(keys)

    def rate_bar(self, bar: dict, prev_bar: dict | None = None) -> str:
        try:
            o = float(bar.get("open") or 0)
            h = float(bar.get("high") or 0)
            l = float(bar.get("low") or 0)
            c = float(bar.get("close") or 0)
        except (TypeError, ValueError):
            return "十字星"
        rng = max(h - l, 1e-12)
        body = abs(c - o)
        ratio = float(bar.get("body_ratio") or body / rng)
        if ratio < 0.12:
            return "十字星"
        bull = c > o
        if bull:
            if ratio >= 0.62:
                tag = "强多头棒"
            elif ratio >= 0.35:
                tag = "中多头棒"
            else:
                tag = "弱多头棒"
        else:
            if ratio >= 0.62:
                tag = "强空头棒"
            elif ratio >= 0.35:
                tag = "中空头棒"
            else:
                tag = "弱空头棒"
        if prev_bar:
            try:
                pc = float(prev_bar.get("close") or 0)
                ph = float(prev_bar.get("high") or 0)
                pl = float(prev_bar.get("low") or 0)
                if bull and c <= pc and h <= ph:
                    tag = "弱多头棒" if tag != "十字星" else tag
                if (not bull) and c >= pc and l >= pl:
                    tag = "弱空头棒" if tag != "十字星" else tag
            except (TypeError, ValueError):
                pass
        return tag

    @property
    def prompt_head_zh(self) -> str:
        return P.HEAD_ZH

    @property
    def prompt_entry_block_zh(self) -> str:
        return P.ENTRY_BLOCK_ZH

    @property
    def prompt_trail_mgmt_zh(self) -> str:
        return P.TRAIL_MGMT_ZH

    @property
    def prompt_decision_no_pos_zh(self) -> str:
        return P.DECISION_NO_POS_ZH

    @property
    def prompt_decision_has_pos_zh(self) -> str:
        return P.DECISION_HAS_POS_ZH

    @property
    def prompt_output_zh(self) -> str:
        return P.OUTPUT_ZH

    @property
    def prompt_head_en(self) -> str:
        return P.HEAD_EN

    @property
    def prompt_entry_block_en(self) -> str:
        return P.ENTRY_BLOCK_EN

    @property
    def prompt_trail_mgmt_en(self) -> str:
        return P.TRAIL_MGMT_EN

    @property
    def prompt_decision_no_pos_en(self) -> str:
        return P.DECISION_NO_POS_EN

    @property
    def prompt_decision_has_pos_en(self) -> str:
        return P.DECISION_HAS_POS_EN

    @property
    def prompt_output_en(self) -> str:
        return P.OUTPUT_EN
