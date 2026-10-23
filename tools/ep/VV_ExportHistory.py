# -*- coding: utf-8 -*-
"""
Quant_Sev — 极智量化历史 K 线导出（M1 / M5 / 日线）
============================================================
这是极智里实际在跑的导出策略（不是 VV_SevBootstrap）。
硬性约束：一个策略最多订阅 20 个合约。

K 线用固定根数（不要当日区域，否则历史不够长）：
  M1=10000  → ep_raw/{日}/m1/  → import 成 data/klines/*/1m
  M5=5000   → ep_raw/{日}/m5/  → import 成 data/klines/*/5m
  D =100    → ep_raw/{日}/d/   → import 成 data/klines/*/1440m

Tick 默认不导。盘中缺口回补：FILL_TICK=True（只导当日 Tick，不重导 M1/M5/D）。
Tick 用区域 [上一日 20:00, now]（SetBarInterval sampleConfig 列表，精确到秒），
不要用 'A'（全历史）也不要用固定根数（WriteCSV 示例的 20000 往往不够一天）。
K 线 OHLC 只信极智 M1/M5/D；Tick 只进 ep_raw/{日}/tick/，不要拷进 data/ticks。

输出：Quant_Sev/data/live/ep_raw/{YYYYMMDD}/m1|m5|d|tick/<InstrumentID>.csv
合约名单优先读 Quant_Sev data/live/dominant（无硬编码品种表）。
主力都导，但跳过极智字典没有的合约（如大商所厂库 ProductID 以 `_f` 结尾）。
订到 `DCE|F|L_F|` 这类空月份代码，极智会「合约不存在」并整批退出。
"""

import csv
import glob
import json
import os
import time
from datetime import datetime, timedelta

# ======================== 可调参数 ========================
_SEV_ROOT = os.environ.get('QUANT_SEV_ROOT', 'C:/Quant_Sev')
_DATA = _SEV_ROOT + '/data'
_DOMINANT_DIR = _DATA + '/live/dominant'
_INSTR_DOM_DIR = _DATA + '/live/instruments/dominant'
_EP_RAW = _DATA + '/live/ep_raw'
_VV_ROOT = 'C:/Quant_VV'
_CSV_ROOT = _VV_ROOT + '/CSV'
g_params['contract_list'] = _CSV_ROOT + '/ep_contracts_preview.csv'
g_params['dominant_glob'] = _CSV_ROOT + '/dominant_*.csv'
g_params['out_dir_m1'] = ''
g_params['out_dir_m5'] = ''
g_params['out_dir_d'] = ''
g_params['out_dir_tick'] = ''
g_params['progress_file'] = _EP_RAW + '/export_progress.json'

g_params['periods'] = 'M1,M5,D'
g_params['m1_bars'] = 10000
g_params['m5_bars'] = 5000
g_params['d_bars'] = 100
# Tick 仅筹码/分笔用，不参与 K 线 OHLC。K 线只信极智 M1/M5/D。
g_params['tick_bars'] = 200000
g_params['batch_size'] = 20
g_params['rotate_delay_sec'] = 3
g_params['reset_progress'] = False
g_params['overwrite'] = True
g_params['live_update'] = False
g_params['live_bars_m1'] = 300
g_params['live_bars_tick'] = 2000
g_params['live_rotate_sec'] = 3

# ★ 模块常量：极智参数面板会覆盖 g_params，关键开关不以面板为准
LIVE_UPDATE = False          # 禁止盘中轮询下载
FORCE_PERIODS = 'M1,M5,D'    # 历史 K 线：1 分钟 / 5 分钟 / 日线
# True = 只导当日 Tick 补 CTP 缺口（WriteCSV 同款 'T',0；区域上一日20:00~now）
# 补完后改回 False，次日开盘继续 M1/M5/D
FILL_TICK = False
DEFAULT_M1_BARS = 10000
DEFAULT_M5_BARS = 5000
DEFAULT_D_BARS = 100
DEFAULT_TICK_BARS = 200000

# ======================== 常量 ========================
MAX_CONTRACTS = 20

BAR_HEADERS = [
    'symbol', 'exchange', 'datetime',
    'open', 'high', 'low', 'close',
    'volume', 'turnover', 'open_interest'
]
TICK_HEADERS = [
    'symbol', 'exchange', 'datetime',
    'TradeDate', 'DateTimeStamp',
    'TotalQty', 'PositionQty', 'LastPrice', 'LastQty', 'PositionChg',
    'BuyPrice', 'SellPrice', 'BuyQty', 'SellQty',
    'InnerSideQty', 'OutSideQty',
    'TickInnerQty', 'TickOuterQty',
]

EXCHANGE_MAP = {
    'CZCE': 'ZCE', 'SHFE': 'SHFE', 'DCE': 'DCE',
    'CFFEX': 'CFFEX', 'INE': 'INE', 'GFEX': 'GFEX',
}
EXCHANGE_REV = {v: k for k, v in EXCHANGE_MAP.items()}

# ======================== 运行态 ========================
_all_rows = []
_batch = []
_ep_map = {}
_period = None
_phase_list = []
_phase_idx = 0
_start_index = 0
_writers = {}          # 兼容旧逻辑：现改为缓冲，不再长期占文件句柄
_row_counts = {}
_last_ts = {}
_seen_ts = {}
_pending_reload = False
_mode = 'bulk'          # 'bulk' | 'live'
_bulk_done = False
_row_buffers = {}       # ep -> [row, ...] 内存缓冲，短开短写，避免 Windows 独占锁
_path_by_ep = {}        # ep -> csv path
_write_failed = {}      # ep -> True 打开失败则本轮跳过，避免刷屏/中断策略

# 每积累多少行落盘一次（越小锁越短，越大越省 IO）
FLUSH_EVERY = 200
OPEN_RETRIES = 5
OPEN_RETRY_SLEEP = 0.05


def _prev_business_day(d):
    d = d - timedelta(days=1)
    while d.weekday() >= 5:
        d = d - timedelta(days=1)
    return d


def convert_timestamp(timestamp, trade_date=''):
    """极智 Tick 的 DateTimeStamp 常为 TradingDay+HHmmss，不是日历日。

    交易日 20260824（周一）21:00 = 上周五 08/21 21:00；00:15 = 周六 08/22 00:15。
    若直接 strptime，过 24 点后字典序回绕，00:00–02:30 会被当成旧 Tick 丢掉。
    M1 日期若已是日历日（≠ TradeDate）则原样保留。
    """
    s = str(timestamp).strip()
    digits = ''.join(ch for ch in s if ch.isdigit())
    if len(digits) < 14:
        raise ValueError('bad timestamp: %s' % s)
    naive = datetime.strptime(digits[:14], '%Y%m%d%H%M%S')
    td_s = ''.join(ch for ch in str(trade_date or '') if ch.isdigit())[:8]
    stamp_day = digits[:8]
    if len(td_s) == 8 and stamp_day == td_s:
        td = datetime.strptime(td_s, '%Y%m%d')
        hh = naive.hour
        if hh >= 20:
            cal = _prev_business_day(td)
        elif hh < 3:
            cal = _prev_business_day(td) + timedelta(days=1)
        else:
            cal = td
        naive = naive.replace(year=cal.year, month=cal.month, day=cal.day)
    out = naive.strftime('%Y/%m/%d %H:%M:%S')
    if len(digits) >= 17:
        out += '.' + digits[14:17]
    return out


def _ts_key(timestamp_or_datetime):
    s = str(timestamp_or_datetime).strip()
    digits = ''.join(ch for ch in s if ch.isdigit())
    if len(digits) >= 17:
        return digits[:17]
    if len(digits) >= 14:
        return digits[:14] + '000'
    try:
        dt = datetime.strptime(s.replace('-', '/')[:19], '%Y/%m/%d %H:%M:%S')
        return dt.strftime('%Y%m%d%H%M%S') + '000'
    except Exception:
        return ''


def _fnum(d, key, default=0.0):
    try:
        v = d.get(key, default)
        if v is None or v == '':
            return default
        return float(v)
    except Exception:
        return default


def _tick_aggressor(d):
    """本笔内外盘：外盘=主动买(打卖价)，内盘=主动卖(打买价)。"""
    qty = _fnum(d, 'LastQty')
    last = _fnum(d, 'LastPrice')
    buy = _fnum(d, 'BuyPrice')
    sell = _fnum(d, 'SellPrice')
    if qty <= 0:
        return 0.0, 0.0
    if sell > 0 and last + 1e-12 >= sell:
        return 0.0, qty
    if buy > 0 and last - 1e-12 <= buy:
        return qty, 0.0
    if buy > 0 and sell > 0:
        if last >= (buy + sell) / 2.0:
            return 0.0, qty
        return qty, 0.0
    return 0.0, 0.0


def _split_month(symbol, product):
    s, p = symbol.strip(), product.strip()
    if s.upper().startswith(p.upper()):
        return s[len(p):]
    compact = p.replace('_', '')
    if compact and s.upper().startswith(compact.upper()):
        return s[len(compact):]
    i = len(s)
    while i > 0 and s[i - 1].isdigit():
        i -= 1
    if i > 0 and s[i - 1] in ('F', 'f'):
        j = i - 1
        while j > 0 and s[j - 1].isdigit():
            j -= 1
        return s[j:]
    return s[i:]


def _is_ep_unexportable(row):
    """极智没有大商所厂库合约（CTP ProductID 以 _f 结尾，合约号如 l2611F）。

    编出 DCE|F|L_F|（月份空）后，SetBarInterval 仍可能 rc=0，但数据准备阶段
    报「合约不存在」并杀掉整批策略。空月份 / 缺字段同样跳过。
    """
    product = (row.get('product') or '').strip()
    if product.lower().endswith('_f'):
        return True
    parts = [p.strip() for p in str(row.get('ep_code') or '').split('|')]
    if len(parts) < 4 or not parts[0] or not parts[2] or not parts[3]:
        return True
    return False


def ctp_to_ep_code(symbol, exchange_ctp, product):
    ep_ex = EXCHANGE_MAP.get(exchange_ctp.upper(), exchange_ctp.upper())
    return '%s|F|%s|%s' % (ep_ex, product.strip().upper(), _split_month(symbol, product))


def normalize_ep_code(ep):
    """纠正常见错误：CZCE→ZCE、品种小写→大写。不信任 CSV 原文。"""
    parts = [p.strip() for p in str(ep or '').split('|')]
    if len(parts) < 4:
        return str(ep or '').strip()
    ex = EXCHANGE_MAP.get(parts[0].upper(), parts[0].upper())
    return '%s|%s|%s|%s' % (ex, parts[1], parts[2].upper(), parts[3])


def _norm_dir(path):
    p = str(path or '')
    p = p.replace('\x01minute', '1minute').replace('\x01', '1')
    p = p.replace('\tick', 'tick')
    if '\t' in p and 'CSV' in p:
        root = p.split('CSV')[0] + 'CSV'
        p = os.path.join(root, 'tick')
    return os.path.normpath(p)


def _load_preview_csv(path):
    rows = []
    with open(path, 'r', encoding='utf-8-sig', newline='') as f:
        for r in csv.DictReader(f):
            symbol = (r.get('ctp_symbol') or r.get('symbol') or '').strip()
            exchange = (r.get('exchange') or '').strip()
            product = (r.get('product') or '').strip()
            ep = (r.get('ep_code') or '').strip()
            if not symbol:
                continue
            # 有 exchange+product 时一律重算，避免 CSV 里 CZCE/小写品种导致「合约不存在」
            if exchange and product:
                ep = ctp_to_ep_code(symbol, exchange, product)
            else:
                ep = normalize_ep_code(ep)
            if not ep:
                continue
            if not exchange:
                parts = ep.split('|')
                if parts:
                    exchange = EXCHANGE_REV.get(parts[0], parts[0])
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'ep_code': ep,
            })
    return rows


def _load_dominant_csv(path):
    rows = []
    with open(path, 'r', encoding='utf-8-sig', newline='') as f:
        for r in csv.DictReader(f):
            symbol = (r.get('symbol') or '').strip()
            exchange = (r.get('exchange') or '').strip()
            product = (r.get('product') or '').strip()
            if not symbol or not exchange or not product:
                continue
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'ep_code': ctp_to_ep_code(symbol, exchange, product),
            })
    return rows


def _load_sev_dominant_rows():
    """优先读 Quant_Sev data/live/dominant（无硬编码品种表）。找不到则返回空。"""
    day = g_params.get('_trading_day') or ''
    candidates = []
    if day:
        candidates.append(os.path.join(_DOMINANT_DIR, '%s.json' % day))
        candidates.append(os.path.join(_INSTR_DOM_DIR, '%s.json' % day))
    candidates.extend([
        os.path.join(_DOMINANT_DIR, 'latest.json'),
        os.path.join(_INSTR_DOM_DIR, 'latest.json'),
    ])
    for root in (_DOMINANT_DIR, _INSTR_DOM_DIR):
        for f in sorted(glob.glob(os.path.join(root, '*.json')), reverse=True):
            if os.path.basename(f) == 'latest.json':
                continue
            if f not in candidates:
                candidates.append(f)

    products_data = None
    instruments_data = None
    for p in candidates:
        if not os.path.isfile(p):
            continue
        try:
            with open(p, 'r', encoding='utf-8-sig') as fh:
                obj = json.load(fh) or {}
            if obj.get('products') and products_data is None:
                products_data = (p, obj)
            if obj.get('instruments') and instruments_data is None:
                instruments_data = (p, obj)
        except Exception as e:
            LogError('读 dominant 失败 %s: %s' % (p, e))

    rows = []
    if products_data:
        path, data = products_data
        if data.get('trading_day'):
            g_params['_trading_day'] = str(data['trading_day'])
        for pk, row in sorted((data.get('products') or {}).items()):
            if not isinstance(row, dict):
                continue
            symbol = (row.get('dominant') or '').strip()
            if not symbol:
                continue
            parts = str(pk).split('.', 1)
            if len(parts) != 2:
                LogError('跳过非法 product_key: %s' % pk)
                continue
            exchange, product = parts[0].strip(), parts[1].strip()
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'product_key': pk,
                'ep_code': ctp_to_ep_code(symbol, exchange, product),
            })
        LogInfo('合约列表(Quant_Sev products): %s  n=%d' % (path, len(rows)))
        return rows

    if instruments_data:
        path, data = instruments_data
        if data.get('trading_day'):
            g_params['_trading_day'] = str(data['trading_day'])
        for row in data.get('instruments') or []:
            if not isinstance(row, dict):
                continue
            symbol = (row.get('instrument_id') or '').strip()
            exchange = (row.get('exchange_id') or '').strip()
            product = (row.get('product_id') or '').strip()
            pk = (row.get('product_key') or ('%s.%s' % (exchange, product))).strip()
            if not symbol or not exchange or not product:
                continue
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'product_key': pk,
                'ep_code': ctp_to_ep_code(symbol, exchange, product),
            })
        LogInfo('合约列表(Quant_Sev instruments): %s  n=%d' % (path, len(rows)))
        return rows
    return []


def _keep_exportable_rows(rows):
    kept, skipped = [], []
    for r in rows:
        if _is_ep_unexportable(r):
            skipped.append('%s→%s' % (r.get('ctp_symbol'), r.get('ep_code')))
            continue
        kept.append(r)
    if skipped:
        LogInfo('跳过极智无此合约 %d 个: %s' % (len(skipped), ','.join(skipped)))
    return kept


def load_contract_rows():
    rows = _load_sev_dominant_rows()
    if rows:
        return _keep_exportable_rows(rows)
    path = _norm_dir(g_params['contract_list'])
    if path and os.path.isfile(path):
        LogInfo('合约列表(Quant_VV preview): %s' % path)
        return _keep_exportable_rows(_load_preview_csv(path))
    files = glob.glob(_norm_dir(g_params['dominant_glob']))
    if not files:
        LogError('未找到合约列表（Quant_Sev dominant JSON 与 Quant_VV CSV 都空）')
        return []
    files.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    LogInfo('合约列表(Quant_VV dominant): %s' % files[0])
    return _keep_exportable_rows(_load_dominant_csv(files[0]))


def parse_periods():
    raw = str(g_params.get('periods', 'M1')).upper().replace(' ', '')
    out = []
    for p in raw.split(','):
        if not p:
            continue
        if p in ('M1', '1M', 'MIN1'):
            out.append(('M', 1, 'M1', int(g_params['m1_bars']), False))
        elif p in ('M5', '5M', 'MIN5'):
            out.append(('M', 5, 'M5', int(g_params.get('m5_bars') or DEFAULT_M5_BARS), False))
        elif p in ('D', 'D1', 'DAY', '1D'):
            out.append(('D', 1, 'D1', int(g_params.get('d_bars') or DEFAULT_D_BARS), False))
        elif p in ('T', 'T0', 'TICK'):
            out.append(('T', 0, 'T0', int(g_params['tick_bars']), True))
        elif p.startswith('M') and p[1:].isdigit():
            n = int(p[1:])
            nb = int(g_params.get('m5_bars') or DEFAULT_M5_BARS) if n == 5 else int(g_params['m1_bars'])
            out.append(('M', n, 'M%d' % n, nb, False))
        else:
            LogError('未知周期忽略: %s' % p)
    return out


def _exported_counts():
    m1 = glob.glob(os.path.join(_norm_dir(g_params['out_dir_m1']), '*.csv'))
    tick = glob.glob(os.path.join(_norm_dir(g_params['out_dir_tick']), '*.csv'))
    return len(m1), len(tick)


def _period_out_dir(period):
    if period[4]:
        return _norm_dir(g_params.get('out_dir_tick') or '')
    tag = period[2]
    if tag == 'M5':
        return _norm_dir(g_params.get('out_dir_m5') or '')
    if tag == 'D1':
        return _norm_dir(g_params.get('out_dir_d') or '')
    return _norm_dir(g_params.get('out_dir_m1') or '')


def _period_file_count(period):
    d = _period_out_dir(period)
    if not d:
        return 0
    return len(glob.glob(os.path.join(d, '*.csv')))


def _progress_invalid_vs_files(phase_idx, done, phase_list):
    if not phase_list:
        return False
    past_end = done or phase_idx >= len(phase_list)
    if not past_end:
        return False
    for p in phase_list:
        if _period_file_count(p) < 1:
            return True
    return False


def _read_progress():
    path = _norm_dir(g_params['progress_file'])
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f) or {}
    except Exception:
        return {}


def save_progress(phase_idx, next_index, total, done=False,
                  live_phase_idx=None, live_next_index=None):
    path = _norm_dir(g_params['progress_file'])
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    old = _read_progress()
    tag = _phase_list[phase_idx][2] if 0 <= phase_idx < len(_phase_list) else ''
    if live_phase_idx is None:
        live_phase_idx = int(old.get('live_phase_idx', 0) or 0)
    if live_next_index is None:
        live_next_index = int(old.get('live_next_index', 0) or 0)
    data = {
        'phase_idx': phase_idx,
        'phase_tag': tag,
        'next_index': next_index,
        'total': total,
        'periods': g_params.get('periods', ''),
        'done': done,
        'trading_day': g_params.get('_trading_day') or _trading_day_ymd(),
        'm1_bars': int(g_params.get('m1_bars') or 0),
        'live_phase_idx': live_phase_idx,
        'live_next_index': live_next_index,
        'updated': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    LogInfo('进度 phase=%s idx=%d/%d done=%s | live_phase=%d live_idx=%d → %s'
            % (tag, next_index, total, done, live_phase_idx, live_next_index, path))


def load_progress(phase_list):
    """返回 (phase_idx, next_index, bulk_done, live_phase_idx, live_next_index)"""
    data = _read_progress()
    day = g_params.get('_trading_day') or _trading_day_ymd()
    # 新交易日：必须重新全量下载一次，忽略昨日 done
    if data and str(data.get('trading_day') or '') != str(day):
        LogInfo('新交易日 %s（进度日=%s）→ 归零，准备一次性下载 %s 根 M1'
                % (day, data.get('trading_day'), g_params.get('m1_bars')))
        return 0, 0, False, 0, 0
    want_periods = str(g_params.get('periods') or '')
    if data and str(data.get('periods') or '') != want_periods:
        LogInfo('周期变更 %s → %s → 进度归零' % (data.get('periods'), want_periods))
        return 0, 0, False, 0, 0

    phase_idx = int(data.get('phase_idx', 0) or 0)
    next_index = int(data.get('next_index', 0) or 0)
    done = bool(data.get('done', False))
    live_phase_idx = int(data.get('live_phase_idx', 0) or 0)
    live_next_index = int(data.get('live_next_index', 0) or 0)

    if data and _progress_invalid_vs_files(phase_idx, done, phase_list):
        LogInfo('进度无效（done 但 M1/M5/D 目录缺文件）→ 归零全量重跑')
        return 0, 0, False, 0, 0

    if done and _export_files_stale():
        LogInfo('进度 done 但 CSV 是旧文件（未真正重写）→ 归零全量重跑')
        return 0, 0, False, 0, 0

    if g_params.get('reset_progress'):
        if data and (not done) and (next_index > 0 or phase_idx > 0):
            LogInfo('reset_progress=True 但全量未完成，按续跑处理；请改回 False')
            return phase_idx, next_index, False, live_phase_idx, live_next_index
        LogInfo('reset_progress=True → 全量进度归零')
        return 0, 0, False, 0, 0

    bulk_done = done or phase_idx >= len(phase_list)
    return phase_idx, next_index, bulk_done, live_phase_idx, live_next_index


def _force_once_a_day_settings():
    """覆盖极智参数面板可能改回的路径 / live_update / periods / bars。

    面板会把 progress_file 还原成 C:/Quant_VV/CSV/export_progress.json，
    旧进度 done=true 会让策略空转、CSV 一行都不写。
    """
    g_params['live_update'] = bool(LIVE_UPDATE)
    g_params['periods'] = 'TICK' if FILL_TICK else FORCE_PERIODS
    g_params['overwrite'] = True
    g_params['progress_file'] = _EP_RAW + '/export_progress.json'
    g_params['m1_bars'] = max(DEFAULT_M1_BARS, int(g_params.get('m1_bars') or DEFAULT_M1_BARS))
    g_params['m5_bars'] = max(DEFAULT_M5_BARS, int(g_params.get('m5_bars') or DEFAULT_M5_BARS))
    g_params['d_bars'] = max(DEFAULT_D_BARS, int(g_params.get('d_bars') or DEFAULT_D_BARS))
    g_params['tick_bars'] = max(DEFAULT_TICK_BARS, int(g_params.get('tick_bars') or DEFAULT_TICK_BARS))
    LogInfo('强制一次性模式 live_update=%s FILL_TICK=%s periods=%s m1=%s m5=%s d=%s tick=%s progress=%s'
            % (g_params['live_update'], FILL_TICK, g_params['periods'],
               g_params['m1_bars'], g_params['m5_bars'], g_params['d_bars'],
               g_params['tick_bars'], g_params['progress_file']))


def _day_done_path():
    day = g_params.get('_trading_day') or _trading_day_ymd()
    return os.path.join(_EP_RAW, day, '_export_done.json')


def _tick_done_path():
    day = g_params.get('_trading_day') or _trading_day_ymd()
    return os.path.join(_EP_RAW, day, '_tick_done.json')


def _mark_day_export_done(n_files):
    path = _day_done_path()
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        try:
            os.makedirs(parent)
        except Exception:
            pass
    data = {
        'trading_day': g_params.get('_trading_day') or _trading_day_ymd(),
        'm1_bars': int(g_params.get('m1_bars') or 0),
        'm5_bars': int(g_params.get('m5_bars') or 0),
        'd_bars': int(g_params.get('d_bars') or 0),
        'tick_bars': int(g_params.get('tick_bars') or 0),
        'files': int(n_files),
        'ts': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
    }
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write('\n')
    except Exception as e:
        LogError('写完成标记失败: %s' % e)
    LogInfo('已标记今日导出完成 → %s' % path)


def _newest_csv_mtime(folder):
    newest = 0.0
    d = _norm_dir(folder or '')
    if not os.path.isdir(d):
        return 0.0
    for name in os.listdir(d):
        if not name.lower().endswith('.csv'):
            continue
        try:
            newest = max(newest, os.path.getmtime(os.path.join(d, name)))
        except Exception:
            continue
    return newest


def _export_files_stale():
    """完成标记/进度比 CSV 新，说明上次只空转写了 done，没有真正落盘。"""
    newest = max(
        _newest_csv_mtime(g_params.get('out_dir_m1')),
        _newest_csv_mtime(g_params.get('out_dir_m5')),
        _newest_csv_mtime(g_params.get('out_dir_d')),
        _newest_csv_mtime(g_params.get('out_dir_tick')),
    )
    if newest <= 0:
        return True
    marks = []
    for p in (_day_done_path(), _tick_done_path(), _norm_dir(g_params.get('progress_file', ''))):
        if p and os.path.isfile(p):
            try:
                marks.append(os.path.getmtime(p))
            except Exception:
                pass
    if marks and newest + 3600 < max(marks):
        return True
    return False


def _mark_tick_export_done(n_files):
    path = _tick_done_path()
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        try:
            os.makedirs(parent)
        except Exception:
            pass
    sample = _history_range()
    data = {
        'trading_day': g_params.get('_trading_day') or _trading_day_ymd(),
        'tick_bars': int(g_params.get('tick_bars') or 0),
        'range': sample,
        'files': int(n_files),
        'ts': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
    }
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write('\n')
    except Exception as e:
        LogError('写 Tick 完成标记失败: %s' % e)
    LogInfo('已标记今日 Tick 补全 → %s（请把 FILL_TICK 改回 False 后同步策略）' % path)


def _is_tick_export_done():
    path = _tick_done_path()
    if not os.path.isfile(path):
        return False
    try:
        with open(path, 'r', encoding='utf-8-sig') as f:
            obj = json.load(f)
        day = g_params.get('_trading_day') or _trading_day_ymd()
        if str(obj.get('trading_day') or '') != str(day):
            return False
        if _period_file_count(('T', 0, 'T0', 0, True)) < 1:
            return False
        if _export_files_stale():
            LogInfo('Tick 完成标记存在但 CSV 仍是旧文件 → 将重导')
            return False
        return True
    except Exception:
        return False


def _is_day_export_done():
    if FILL_TICK:
        return _is_tick_export_done()
    path = _day_done_path()
    if not os.path.isfile(path):
        return False
    try:
        with open(path, 'r', encoding='utf-8-sig') as f:
            obj = json.load(f)
        day = g_params.get('_trading_day') or _trading_day_ymd()
        if str(obj.get('trading_day') or '') != str(day):
            return False
        for p in parse_periods():
            if _period_file_count(p) < 1:
                return False
        if _export_files_stale():
            LogInfo('完成标记存在但 CSV 仍是旧文件（mtime 过期）→ 将重导')
            return False
        return True
    except Exception:
        return False


def _try_stop_strategy():
    for name in ('StopStrategy', 'UnloadStrategy', 'ExitStrategy'):
        try:
            fn = globals().get(name)
            if fn is None:
                continue
            fn()
            LogInfo('已调用 %s 结束导出策略' % name)
            return
        except Exception as e:
            LogInfo('%s 不可用: %s' % (name, e))
    LogInfo('请手动【停止】本策略；开仓/风控用 VV_SignalTrader / VV_RiskTrail')


def _trading_day_ymd():
    """优先 Quant_Sev dominant JSON 的 trading_day，再退回 Quant_VV CSV 文件名。"""
    files = []
    for name in ('latest.json',):
        p = os.path.join(_DOMINANT_DIR, name)
        if os.path.isfile(p):
            files.append(p)
    for f in sorted(glob.glob(os.path.join(_DOMINANT_DIR, '*.json')), reverse=True):
        if f not in files:
            files.append(f)
    for f in files:
        try:
            with open(f, 'r', encoding='utf-8-sig') as fh:
                obj = json.load(fh) or {}
            d = str(obj.get('trading_day') or '')
            if len(d) == 8 and d.isdigit():
                return d
            base = os.path.basename(f)
            if base[:8].isdigit() and base.endswith('.json'):
                return base[:8]
        except Exception:
            continue
    try:
        for name in sorted(os.listdir(_EP_RAW), reverse=True):
            if len(name) == 8 and name.isdigit():
                return name
    except Exception:
        pass
    qfiles = sorted(glob.glob(_norm_dir(g_params.get('dominant_glob', ''))), reverse=True)
    for f in qfiles:
        base = os.path.basename(f)
        if base.startswith('dominant_') and base.endswith('.csv') and len(base) >= 21:
            d = base[9:17]
            if d.isdigit():
                return d
    try:
        for name in sorted(os.listdir(_CSV_ROOT), reverse=True):
            p = os.path.join(_CSV_ROOT, name, 'dominant.csv')
            if len(name) == 8 and name.isdigit() and os.path.isfile(p):
                return name
    except Exception:
        pass
    return datetime.now().strftime('%Y%m%d')


def _apply_day_output_dirs():
    """写入 Quant_Sev/data/live/ep_raw/YYYYMMDD/m1|m5|d（本仓库只认这条路径）。"""
    _force_once_a_day_settings()
    day = _trading_day_ymd()
    g_params['_trading_day'] = day
    day_root = os.path.join(_EP_RAW, day)
    g_params['out_dir_m1'] = os.path.join(day_root, 'm1')
    g_params['out_dir_m5'] = os.path.join(day_root, 'm5')
    g_params['out_dir_d'] = os.path.join(day_root, 'd')
    g_params['out_dir_tick'] = os.path.join(day_root, 'tick')
    for d in (_norm_dir(g_params['out_dir_m1']), _norm_dir(g_params['out_dir_m5']),
              _norm_dir(g_params['out_dir_d']), _norm_dir(g_params['out_dir_tick'])):
        if d and not os.path.isdir(d):
            try:
                os.makedirs(d)
            except Exception:
                pass
    LogInfo('导出目录 m1=%s m5=%s d=%s tick=%s day=%s (M1=%s M5=%s D=%s)'
            % (g_params['out_dir_m1'], g_params['out_dir_m5'], g_params['out_dir_d'],
               g_params['out_dir_tick'], day,
               g_params.get('m1_bars'), g_params.get('m5_bars'), g_params.get('d_bars')))


def _out_dir_for_period():
    if _period is None:
        return _norm_dir(g_params['out_dir_m1'])
    if _period[4]:
        return _norm_dir(g_params['out_dir_tick'])
    tag = _period[2]
    if tag == 'M5':
        return _norm_dir(g_params.get('out_dir_m5') or '')
    if tag == 'D1':
        return _norm_dir(g_params.get('out_dir_d') or '')
    return _norm_dir(g_params['out_dir_m1'])


def _csv_path(item):
    return os.path.join(_out_dir_for_period(), '%s.csv' % item['ctp_symbol'])


def _read_last_ts_from_csv(path, is_tick):
    """取文件中最大日历时间（第 3 列 datetime）。Tick 的 DateTimeStamp 是交易日拼钟，不能当顺序键。"""
    if not os.path.isfile(path) or os.path.getsize(path) == 0:
        return ''
    try:
        max_key = ''
        with open(path, 'r', encoding='utf-8-sig', newline='') as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if not header:
                return ''
            for row in reader:
                if len(row) < 3:
                    continue
                key = _ts_key(row[2])
                if not key and is_tick and len(row) >= 5:
                    key = _ts_key(row[4])
                if key and key > max_key:
                    max_key = key
        return max_key
    except Exception as e:
        LogError('读取已有 CSV 最大时间失败 %s: %s' % (path, e))
        return ''


def _prepare_file_meta(item):
    """只准备路径/末时间，不长时间占用文件。"""
    ep = item['ep_code']
    if ep in _path_by_ep:
        return _path_by_ep[ep]

    out_dir = _out_dir_for_period()
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    path = _csv_path(item)
    is_tick = _period[4]
    overwrite = bool(g_params.get('overwrite')) and _mode == 'bulk'

    if overwrite or not os.path.isfile(path) or os.path.getsize(path) == 0:
        # 需要表头：短开写入后立刻关闭
        ok = _open_write_rows(path, 'w', [TICK_HEADERS if is_tick else BAR_HEADERS])
        if not ok:
            _write_failed[ep] = True
            return path
        _last_ts[ep] = ''
    else:
        if ep not in _last_ts:
            _last_ts[ep] = _read_last_ts_from_csv(path, is_tick)

    _path_by_ep[ep] = path
    _row_counts.setdefault(ep, 0)
    _row_buffers.setdefault(ep, [])
    return path


def _open_write_rows(path, mode, rows):
    """短开短写；PermissionError 重试，避免长期锁死。"""
    last_err = None
    for i in range(OPEN_RETRIES):
        try:
            with open(path, mode, encoding='utf-8', newline='') as f:
                w = csv.writer(f)
                for row in rows:
                    w.writerow(row)
                f.flush()
            return True
        except PermissionError as e:
            last_err = e
            time.sleep(OPEN_RETRY_SLEEP * (i + 1))
        except Exception as e:
            last_err = e
            break
    LogError('写入失败 %s: %s（请关闭 Excel/其它占用该文件的程序）' % (path, last_err))
    return False


def _flush_ep(ep, force=False):
    buf = _row_buffers.get(ep) or []
    if not buf:
        return True
    if (not force) and len(buf) < FLUSH_EVERY:
        return True
    path = _path_by_ep.get(ep)
    if not path:
        return False
    rows = list(buf)
    _row_buffers[ep] = []
    ok = _open_write_rows(path, 'a', rows)
    if not ok:
        # 写回缓冲，标记失败，本轮不再强写以免拖垮策略
        _row_buffers[ep] = rows + _row_buffers.get(ep, [])
        _write_failed[ep] = True
        return False
    return True


def _close_all_writers():
    """刷尽缓冲并释放；不再持有打开的文件句柄。"""
    n_files = 0
    for ep in list(_path_by_ep.keys()):
        _flush_ep(ep, force=True)
        n_files += 1
        LogInfo('写出 %s  新增rows=%d  末时间=%s%s'
                % (_path_by_ep.get(ep, ''),
                   _row_counts.get(ep, 0),
                   _last_ts.get(ep, ''),
                   ' [写失败跳过]' if _write_failed.get(ep) else ''))
    _row_buffers.clear()
    _path_by_ep.clear()
    _writers.clear()
    return n_files


def _should_write(ep, calendar_ts):
    """去重键必须是日历时间。DateTimeStamp 过 24 点会从 23:59 跳回 00:00，字典序变小。"""
    key = _ts_key(calendar_ts)
    if not key:
        return False, key
    seen = _seen_ts.setdefault(ep, set())
    if key in seen:
        return False, key
    last = _last_ts.get(ep, '')
    if _mode != 'bulk' and last and key <= last:
        return False, key
    seen.add(key)
    return True, key


def _buffer_row(item, row, key):
    ep = item['ep_code']
    if _write_failed.get(ep):
        return
    try:
        _prepare_file_meta(item)
        if _write_failed.get(ep):
            return
        _row_buffers.setdefault(ep, []).append(row)
        _last_ts[ep] = key
        _row_counts[ep] = _row_counts.get(ep, 0) + 1
        _flush_ep(ep, force=False)
    except Exception as e:
        LogError('缓冲写入异常 %s: %s' % (item.get('ctp_symbol'), e))
        _write_failed[ep] = True


def _write_tick_row(item, d):
    ep = item['ep_code']
    raw = d.get('DateTimeStamp', '')
    try:
        dt = convert_timestamp(raw, d.get('TradeDate', ''))
    except Exception:
        return
    ok, key = _should_write(ep, dt)
    if not ok:
        return
    inner_tick, outer_tick = _tick_aggressor(d)
    _buffer_row(item, [
        item['ctp_symbol'], item['exchange_ctp'], dt,
        d.get('TradeDate', ''), d.get('DateTimeStamp', ''),
        d.get('TotalQty', ''), d.get('PositionQty', ''),
        d.get('LastPrice', ''), d.get('LastQty', ''), d.get('PositionChg', ''),
        d.get('BuyPrice', ''), d.get('SellPrice', ''),
        d.get('BuyQty', ''), d.get('SellQty', ''),
        d.get('InnerSideQty', ''), d.get('OutSideQty', ''),
        inner_tick, outer_tick,
    ], key)


def _as_bar_list(info):
    """HisBarsInfo 在极智里常返回 numpy 数组，不能用 if info:（会 ValueError）。"""
    if info is None:
        return []
    try:
        seq = list(info)
    except Exception:
        return []
    out = []
    for d in seq:
        if isinstance(d, dict):
            out.append(d)
            continue
        if hasattr(d, 'dtype') and getattr(d.dtype, 'names', None):
            try:
                out.append({k: d[k] for k in d.dtype.names})
                continue
            except Exception:
                pass
        try:
            out.append(dict(d))
        except Exception:
            continue
    return out


def _hisbars_list(ep, ktype, kslice, n):
    info = None
    try:
        info = HisBarsInfo(ep, ktype, kslice, n)
    except Exception as e:
        LogError('HisBarsInfo(%s,%s,%s) 失败: %s' % (ep, ktype, kslice, e))
    out = _as_bar_list(info)
    if out:
        return out
    if ktype == 'T' or kslice == 0:
        try:
            info = HisBarsInfo(ep, Enum_Period_Tick(), 0, n)
        except Exception as e:
            LogError('HisBarsInfo Enum_Period_Tick 失败 %s: %s' % (ep, e))
            return []
        return _as_bar_list(info)
    return []


def _dump_hisbars_batch():
    """全量结束时用 HisBarsInfo 补全（Tick 独有现手/买卖/内外盘）。"""
    if _period is None or not _batch:
        return
    bar_type, interval, file_tag, bars_n, is_tick = _period
    n = int(bars_n)
    if is_tick:
        n = max(n, DEFAULT_TICK_BARS)
        ktype, kslice = 'T', 0
    else:
        ktype, kslice = bar_type, interval
    for item in _batch:
        try:
            info = _hisbars_list(item['ep_code'], ktype, kslice, n)
        except Exception as e:
            LogError('HisBarsInfo %s 异常: %s' % (item.get('ctp_symbol'), e))
            continue
        LogInfo('HisBarsInfo %s %s 根=%d' % (item['ctp_symbol'], file_tag, len(info)))
        if not info:
            _prepare_file_meta(item)
            LogInfo('HisBarsInfo %s %s 无成交，仍留空表（不剔除低量主力）'
                    % (item['ctp_symbol'], file_tag))
            continue
        for d in info:
            if not isinstance(d, dict):
                continue
            if is_tick:
                _write_tick_row(item, d)
            else:
                _write_bar_row(item, d)
        _flush_ep(item['ep_code'], force=True)


def _write_bar_row(item, d):
    ep = item['ep_code']
    raw = d.get('DateTimeStamp', '')
    try:
        dt = convert_timestamp(raw, d.get('TradeDate', ''))
    except Exception:
        return
    ok, key = _should_write(ep, dt)
    if not ok:
        return
    _buffer_row(item, [
        item['ctp_symbol'], item['exchange_ctp'], dt,
        d.get('OpeningPrice', ''), d.get('HighPrice', ''),
        d.get('LowPrice', ''), d.get('LastPrice', ''),
        d.get('KLineQty', ''), '', d.get('PositionQty', ''),
    ], key)

def _schedule_reload(delay_sec=None):
    global _pending_reload
    if _pending_reload:
        return
    _pending_reload = True
    if delay_sec is None:
        delay_sec = float(g_params.get('rotate_delay_sec', 3) or 3)
    delay_sec = max(1.0, float(delay_sec))
    LogInfo('%.0f 秒后 ReloadStrategy…' % delay_sec)
    time.sleep(delay_sec)
    try:
        ReloadStrategy()
    except Exception as e:
        LogError('ReloadStrategy 失败，请手动再运行: %s' % e)


def _history_range():
    """极智「区域」样本：sampleConfig=[YYYYMMDDhhmmss, YYYYMMDDhhmmss]（整数）。

    覆盖本交易日夜盘起点（上一交易日 20:00）到当前时刻，避免 'A' 拉全历史 Tick。
    """
    day = g_params.get('_trading_day') or _trading_day_ymd()
    try:
        td = datetime.strptime(str(day), '%Y%m%d')
    except Exception:
        td = datetime.now()
    night = _prev_business_day(td)
    start = int(night.strftime('%Y%m%d') + '200000')
    end = int(datetime.now().strftime('%Y%m%d%H%M%S'))
    if end < start:
        end = int(td.strftime('%Y%m%d') + '150000')
    return [start, end]


def _cap_batch_size():
    batch_size = int(g_params.get('batch_size', MAX_CONTRACTS) or MAX_CONTRACTS)
    if batch_size <= 0 or batch_size > MAX_CONTRACTS:
        LogInfo('batch_size 超出上限，改为 %d' % MAX_CONTRACTS)
        batch_size = MAX_CONTRACTS
    return batch_size


def _subscribe_batch(batch, period, bars_n):
    bar_type, interval, file_tag = period[0], period[1], period[2]
    is_tick = bool(period[4]) if len(period) > 4 else (bar_type == 'T' and int(interval) == 0)
    sample = _history_range()
    for item in batch:
        if is_tick:
            n = max(int(bars_n), DEFAULT_TICK_BARS)
            rc = SetBarInterval(item['ep_code'], 'T', 0, sample, n, True)
            LogInfo('订阅 Tick(区域 %s~%s, 同 WriteCSV 的 T/0) %s barDataLen=%d → %s rc=%s'
                    % (sample[0], sample[1], item['ctp_symbol'], n, item['ep_code'], rc))
        else:
            n = max(int(bars_n), 100)
            # K 线用固定根数（10000/5000/100），不要用当日区域，否则历史不够长
            rc = SetBarInterval(item['ep_code'], bar_type, interval, n, n, True)
            LogInfo('订阅 %s %s 根数=%d → %s rc=%s'
                    % (item['ctp_symbol'], file_tag, n, item['ep_code'], rc))
        if rc not in (0, None, True):
            LogError('SetBarInterval 失败 %s rc=%s'
                     % (item.get('ctp_symbol'), rc))


def _start_bulk_batch():
    global _batch, _ep_map, _period, _mode
    _mode = 'bulk'
    _period = _phase_list[_phase_idx]
    bar_type, interval, file_tag, bars_n, is_tick = _period
    batch_size = _cap_batch_size()
    _batch = _all_rows[_start_index:_start_index + batch_size]
    _ep_map = {it['ep_code']: it for it in _batch}

    LogInfo('=== 全量导出 ===')
    LogInfo('周期=%s 本轮=%d/%d 起始=%d phase=%d/%d'
            % (file_tag, len(_batch), len(_all_rows), _start_index,
               _phase_idx + 1, len(_phase_list)))
    _subscribe_batch(_batch, _period, bars_n)
    SetTriggerType(5)
    SetOrderWay(2)


def _start_live_batch(live_phase_idx, live_next_index):
    global _batch, _ep_map, _period, _mode, _phase_idx, _start_index
    _mode = 'live'

    # 规范 live 下标
    while live_phase_idx < len(_phase_list) and live_next_index >= len(_all_rows):
        live_phase_idx += 1
        live_next_index = 0
    if live_phase_idx >= len(_phase_list):
        live_phase_idx = 0
        live_next_index = 0
        LogInfo('实盘轮换一圈完成，从头继续增量更新')

    _phase_idx = live_phase_idx
    _start_index = live_next_index
    _period = _phase_list[live_phase_idx]
    bar_type, interval, file_tag, _bulk_bars, is_tick = _period

    if is_tick:
        bars_n = int(g_params.get('live_bars_tick', 2000) or 2000)
    else:
        bars_n = int(g_params.get('live_bars_m1', 300) or 300)

    batch_size = _cap_batch_size()
    _batch = _all_rows[live_next_index:live_next_index + batch_size]
    _ep_map = {it['ep_code']: it for it in _batch}

    m1_n, tick_n = _exported_counts()
    LogInfo('=== 实盘增量更新 ===')
    LogInfo('周期=%s 回补bars=%d 本轮=%d/%d 起始=%d live_phase=%d/%d | 已有文件 1m=%d tick=%d'
            % (file_tag, bars_n, len(_batch), len(_all_rows), live_next_index,
               live_phase_idx + 1, len(_phase_list), m1_n, tick_n))
    LogInfo('历史回补后将接收实时K/Tick；约 %ss 后自动换下一批'
            % g_params.get('live_rotate_sec', 3))

    _subscribe_batch(_batch, _period, bars_n)
    SetTriggerType(5)
    SetOrderWay(2)
    # 定时轮换（可在 initialize 外设置 type=3）
    rotate_ms = int(float(g_params.get('live_rotate_sec', 3) or 3) * 1000)
    rotate_ms = max(2000, (rotate_ms // 100) * 100)
    SetTriggerType(3, rotate_ms)


def initialize(context):
    global _all_rows, _batch, _ep_map, _period, _phase_list
    global _phase_idx, _start_index, _writers, _row_counts, _last_ts, _seen_ts
    global _pending_reload, _mode, _bulk_done
    global _row_buffers, _path_by_ep, _write_failed
    _writers = {}
    _row_counts = {}
    _last_ts = {}
    _seen_ts = {}
    _ep_map = {}
    _batch = []
    _period = None
    _pending_reload = False
    _mode = 'bulk'
    _bulk_done = False
    _row_buffers = {}
    _path_by_ep = {}
    _write_failed = {}

    _apply_day_output_dirs()  # 内含强制 live_update / FILL_TICK / bars
    LogInfo('本策略写入 Quant_Sev: %s （若日志仍是 C:/Quant_VV 说明极智没加载到新脚本）'
            % (g_params.get('out_dir_tick') if FILL_TICK else g_params.get('out_dir_m1')))
    if FILL_TICK:
        sample = _history_range()
        LogInfo('FILL_TICK=True → 只导当日 Tick（SetBarInterval T,0 区域 %s~%s），不重导 M1/M5/D'
                % (sample[0], sample[1]))

    _phase_list = parse_periods()
    if not _phase_list:
        LogError('periods 为空')
        return

    _all_rows = load_contract_rows()
    if not _all_rows:
        LogError('合约列表为空')
        return

    # 今日已导出完 → 空闲，绝不 Reload / 订阅
    if _is_day_export_done() and not LIVE_UPDATE:
        m1_n, tick_n = _exported_counts()
        LogInfo('今日 ep_raw %s 已完成（m1=%d tick=%d），本策略空闲，不再下载。请【停止】本策略。'
                % ('Tick' if FILL_TICK else 'M1/M5/D', m1_n, tick_n))
        save_progress(len(_phase_list), 0, len(_all_rows), done=True)
        _try_stop_strategy()
        return

    phase_idx, next_index, bulk_done, live_phase_idx, live_next_index = load_progress(_phase_list)
    _phase_idx, _start_index = phase_idx, next_index
    _bulk_done = bulk_done

    if not bulk_done:
        while _phase_idx < len(_phase_list) and _start_index >= len(_all_rows):
            _phase_idx += 1
            _start_index = 0
        if _phase_idx >= len(_phase_list):
            bulk_done = True
            _bulk_done = True

    if not bulk_done:
        total_batches = (len(_all_rows) + _cap_batch_size() - 1) // _cap_batch_size()
        cur_batch = _start_index // max(1, _cap_batch_size()) + 1
        LogInfo('全量分批下载中：约第 %d/%d 批（每批≤20，属正常；完成后会停止）'
                % (cur_batch, max(1, total_batches)))
        _start_bulk_batch()
        return

    # ---- 全量已完成 ----
    m1_n, tick_n = _exported_counts()
    LogInfo('全量导出已完成（m1=%d tick=%d）' % (m1_n, tick_n))
    save_progress(len(_phase_list), 0, len(_all_rows), done=True,
                  live_phase_idx=live_phase_idx, live_next_index=live_next_index)
    if FILL_TICK:
        _mark_tick_export_done(tick_n)
    else:
        _mark_day_export_done(m1_n)

    if not LIVE_UPDATE:
        LogInfo('LIVE_UPDATE=False → 结束下载，请停止本策略')
        _try_stop_strategy()
        return

    _start_live_batch(live_phase_idx, live_next_index)


def handle_data(context):
    global _pending_reload
    tt = context.triggerType()

    # 实盘定时轮换 —— 仅 LIVE_UPDATE 允许
    if tt == 'C' and _mode == 'live' and _batch and not _pending_reload:
        if not LIVE_UPDATE:
            LogInfo('忽略盘中轮换（LIVE_UPDATE=False）')
            return
        _close_all_writers()
        next_index = _start_index + len(_batch)
        phase_idx = _phase_idx
        if next_index >= len(_all_rows):
            phase_idx += 1
            next_index = 0
        if phase_idx >= len(_phase_list):
            phase_idx = 0
            next_index = 0
        save_progress(len(_phase_list), 0, len(_all_rows), done=True,
                      live_phase_idx=phase_idx, live_next_index=next_index)
        LogInfo('实盘定时轮换 → live_phase=%d live_idx=%d' % (phase_idx, next_index))
        _schedule_reload(1)
        return

    if _period is None or not _batch:
        return
    if tt not in ('H', 'K'):
        return

    ep = context.contractNo()
    item = _ep_map.get(ep)
    if item is None:
        return

    d = context.triggerData()
    is_tick = _period[4]
    if is_tick:
        if context.kLineType() != 'T' or context.kLineSlice() != 0:
            return
        # WriteCSV：历史阶段 'H' 逐笔落盘；HisBarsInfo 在 hisover 再补一次（去重）
        _write_tick_row(item, d)
    else:
        if context.kLineType() == 'T' and context.kLineSlice() == 0:
            return
        _write_bar_row(item, d)


def hisover_callback(context):
    if _period is None or not _batch:
        LogInfo('本轮无订阅任务')
        return

    try:
        _dump_hisbars_batch()
    except Exception as e:
        LogError('_dump_hisbars_batch 异常（继续换批）: %s' % e)
    n_files = _close_all_writers()
    file_tag = _period[2]
    next_index = _start_index + len(_batch)
    remain = max(0, len(_all_rows) - next_index)

    if _mode == 'live':
        LogInfo('实盘回补结束 period=%s 新增文件触及≈%d；等待实时K/Tick，%ss 后换批'
                % (file_tag, n_files, g_params.get('live_rotate_sec', 3)))
        # 不 Reload：留在实盘接收 'K'；轮换由定时器触发
        return

    # ---- 全量模式 ----
    LogInfo('全量本轮完成 period=%s 文件≈%d 下一=%d 剩余=%d'
            % (file_tag, n_files, next_index, remain))
    phase_idx = _phase_idx
    if remain > 0:
        save_progress(phase_idx, next_index, len(_all_rows), done=False)
        _schedule_reload()
        return

    phase_idx += 1
    if phase_idx >= len(_phase_list):
        save_progress(phase_idx, 0, len(_all_rows), done=True,
                      live_phase_idx=0, live_next_index=0)
        m1_n, tick_n = _exported_counts()
        if FILL_TICK:
            _mark_tick_export_done(tick_n)
            LogInfo('全部主力当日 Tick 补全完成（tick文件≈%d）。请【停止】并把 FILL_TICK 改回 False'
                    % tick_n)
        else:
            _mark_day_export_done(m1_n)
            LogInfo('全部主力 M1+Tick 全量完成（m1文件≈%d tick文件≈%d），停止下载' % (m1_n, tick_n))
        if LIVE_UPDATE:
            LogInfo('LIVE_UPDATE=True → 进入盘中增量（当前不应开启）')
            _schedule_reload()
        else:
            _try_stop_strategy()
        return

    save_progress(phase_idx, 0, len(_all_rows), done=False)
    LogInfo('周期切换 → %s' % _phase_list[phase_idx][2])
    _schedule_reload()


def exit_callback(context):
    _close_all_writers()
    LogInfo('VV_ExportHistory 退出 mode=%s' % _mode)
