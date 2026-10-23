# -*- coding: utf-8 -*-
"""
Quant_Sev — 极智量化启动补全（当日 M1 + Tick）
============================================================
硬性约束：一个策略最多订阅 20 个合约。

用途：打开项目 / 开盘前在极智中运行一次，补 CTP 没有的历史回看：
  - 读 Quant_Sev/data/live/dominant/{TradingDay|latest}.json（无硬编码品种表）
  - 先 M1 再 Tick，分批 ReloadStrategy（每批最多 20 个，慢）
  - 只写入 Quant_Sev/data/live/ep_raw/{YYYYMMDD}/m1|tick/<InstrumentID>.csv
  - live_update=False：全量完成后退出（极智不负责盘中）
  - 盘中 Tick + K 线只由本地 CTP --live-ticks 续写

部署：tools/ep/sync_to_jizhi.ps1 → 极智「用户策略」目录后运行本文件。
导入：python tools/import_ep_to_store.py --day YYYYMMDD
"""

import csv
import glob
import json
import os
import time
from datetime import datetime, timedelta

# ======================== 可调参数 ========================
# 仓库根默认 C:/Quant_Sev；可设环境变量 QUANT_SEV_ROOT 覆盖
_SEV_ROOT = os.environ.get('QUANT_SEV_ROOT', 'C:/Quant_Sev')
_DATA = _SEV_ROOT + '/data'
_DOMINANT_DIR = _DATA + '/live/dominant'
_INSTR_DOM_DIR = _DATA + '/live/instruments/dominant'
_EP_RAW = _DATA + '/live/ep_raw'
g_params['contract_list'] = ''  # unused; load dominant JSON
g_params['dominant_glob'] = _DOMINANT_DIR + '/*.json'
g_params['out_dir_m1'] = ''
g_params['out_dir_tick'] = ''
g_params['progress_file'] = _EP_RAW + '/export_progress.json'

g_params['periods'] = 'M1,TICK'
g_params['m1_bars'] = 1200
# Tick 是「最近 N 笔」快照，不是按交易时段切。隔夜品种（ag/au 到 02:30）
# 必须在夜盘收完后再导，否则会停在导出当时（例如周五 23:15），00:00–02:30 永远进不来。
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
LIVE_UPDATE = False
FORCE_PERIODS = 'M1,TICK'
DEFAULT_M1_BARS = 1200
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
    """极智 DateTimeStamp 对 Tick 常为 TradingDay+HHmmss，不是日历日。

    例：交易日 20260824（周一）的 21:00 = 上周五 08/21 21:00；
    00:15 = 周六 08/22 00:15。若按字符串直接 strptime，会变成周一 21:00 / 周一 00:15，
    过 24 点后字典序还会回绕，00:00–02:30 被当成旧 Tick 丢掉。
    M1 的 DateTimeStamp 日期若已是日历日（≠ TradeDate），原样保留。
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


def _split_month(symbol, product):
    s, p = symbol.strip(), product.strip()
    if s.upper().startswith(p.upper()):
        return s[len(p):]
    i = len(s)
    while i > 0 and s[i - 1].isdigit():
        i -= 1
    return s[i:]


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


def load_contract_rows():
    """Read dominant JSON: prefer live/dominant products; fallback instruments/dominant list."""
    day = g_params.get('_trading_day') or _trading_day_ymd()
    candidates = [
        os.path.join(_DOMINANT_DIR, '%s.json' % day),
        os.path.join(_DOMINANT_DIR, 'latest.json'),
        os.path.join(_INSTR_DOM_DIR, '%s.json' % day),
        os.path.join(_INSTR_DOM_DIR, 'latest.json'),
    ]
    for root in (_DOMINANT_DIR, _INSTR_DOM_DIR):
        files = sorted(glob.glob(os.path.join(root, '*.json')), reverse=True)
        for f in files:
            if os.path.basename(f) == 'latest.json':
                continue
            if f not in candidates:
                candidates.append(f)

    path = None
    data = None
    for p in candidates:
        if not os.path.isfile(p):
            continue
        try:
            with open(p, 'r', encoding='utf-8-sig') as fh:
                obj = json.load(fh) or {}
            if obj.get('products') or obj.get('instruments'):
                path, data = p, obj
                if obj.get('products'):
                    break
                # keep searching for products map if only instruments list
                if data.get('products'):
                    break
        except Exception as e:
            LogError('读 dominant 失败 %s: %s' % (p, e))

    # Prefer any products map found; else use instruments list from last hit
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
        except Exception:
            continue

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
            ep = ctp_to_ep_code(symbol, exchange, product)
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'product_key': pk,
                'ep_code': ep,
            })
        LogInfo('合约列表(products): %s  n=%d' % (path, len(rows)))
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
            ep = ctp_to_ep_code(symbol, exchange, product)
            rows.append({
                'ctp_symbol': symbol,
                'exchange_ctp': exchange,
                'product': product,
                'product_key': pk,
                'ep_code': ep,
            })
        LogInfo('合约列表(instruments): %s  n=%d' % (path, len(rows)))
        return rows

    LogError('未找到 dominant JSON（%s 或 %s）' % (_DOMINANT_DIR, _INSTR_DOM_DIR))
    return []


def parse_periods():
    raw = str(g_params.get('periods', FORCE_PERIODS)).upper().replace(' ', '')
    out = []
    for p in raw.split(','):
        if not p:
            continue
        if p in ('M1', '1M', 'MIN1'):
            out.append(('M', 1, 'M1', int(g_params['m1_bars']), False))
        elif p in ('T', 'T0', 'TICK'):
            out.append(('T', 0, 'T0', int(g_params['tick_bars']), True))
        elif p.startswith('M') and p[1:].isdigit():
            n = int(p[1:])
            out.append(('M', n, 'M%d' % n, int(g_params['m1_bars']), False))
        else:
            LogError('未知周期忽略: %s' % p)
    return out


def _csv_has_data(path, min_bytes=180):
    """表头空壳不算导出成功（Tick 曾写出仅 HEADER 的文件导致假完成）。"""
    try:
        return os.path.isfile(path) and os.path.getsize(path) >= int(min_bytes)
    except Exception:
        return False


def _exported_counts():
    m1_dir = _norm_dir(g_params['out_dir_m1'])
    tick_dir = _norm_dir(g_params['out_dir_tick'])
    m1 = [p for p in glob.glob(os.path.join(m1_dir, '*.csv')) if _csv_has_data(p)]
    tick = [p for p in glob.glob(os.path.join(tick_dir, '*.csv')) if _csv_has_data(p, 200)]
    return len(m1), len(tick)


def _progress_invalid_vs_files(phase_idx, done, phase_list):
    if not phase_list:
        return False
    past_end = done or phase_idx >= len(phase_list)
    if not past_end:
        return False
    m1_n, tick_n = _exported_counts()
    return not _export_complete_enough(m1_n, tick_n, phase_list)


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

    phase_idx = int(data.get('phase_idx', 0) or 0)
    next_index = int(data.get('next_index', 0) or 0)
    done = bool(data.get('done', False))
    live_phase_idx = int(data.get('live_phase_idx', 0) or 0)
    live_next_index = int(data.get('live_next_index', 0) or 0)

    if data and _progress_invalid_vs_files(phase_idx, done, phase_list):
        m1_n, tick_n = _exported_counts()
        LogInfo('进度无效（done 但目录空: 1minute=%d tick=%d）→ 归零全量重跑'
                % (m1_n, tick_n))
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
    """覆盖极智参数面板可能改回的 live_update / periods / bars。"""
    g_params['live_update'] = bool(LIVE_UPDATE)
    g_params['periods'] = FORCE_PERIODS
    g_params['overwrite'] = True
    g_params['m1_bars'] = max(100, int(g_params.get('m1_bars') or DEFAULT_M1_BARS))
    g_params['tick_bars'] = max(DEFAULT_TICK_BARS, int(g_params.get('tick_bars') or DEFAULT_TICK_BARS))
    LogInfo('强制一次性模式 live_update=%s periods=%s m1_bars=%s tick_bars=%s'
            % (g_params['live_update'], g_params['periods'],
               g_params['m1_bars'], g_params['tick_bars']))


def _trading_day_ymd():
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
    # ep_raw 已有日目录
    try:
        for name in sorted(os.listdir(_EP_RAW), reverse=True):
            if len(name) == 8 and name.isdigit():
                return name
    except Exception:
        pass
    return datetime.now().strftime('%Y%m%d')


def _apply_day_output_dirs():
    """写入 data/live/ep_raw/YYYYMMDD/m1 与 tick。"""
    _force_once_a_day_settings()
    day = _trading_day_ymd()
    g_params['_trading_day'] = day
    day_root = os.path.join(_EP_RAW, day)
    g_params['out_dir_m1'] = os.path.join(day_root, 'm1')
    g_params['out_dir_tick'] = os.path.join(day_root, 'tick')
    for d in (_norm_dir(g_params['out_dir_m1']), _norm_dir(g_params['out_dir_tick'])):
        if d and not os.path.isdir(d):
            try:
                os.makedirs(d)
            except Exception:
                pass
    LogInfo('一次性导出目录 m1=%s tick=%s day=%s'
            % (g_params['out_dir_m1'], g_params['out_dir_tick'], day))


def _day_done_path():
    day = g_params.get('_trading_day') or _trading_day_ymd()
    return os.path.join(_EP_RAW, day, '_export_done.json')


def _tick_phase_idx(phase_list=None):
    pl = phase_list or _phase_list or parse_periods()
    for i, p in enumerate(pl):
        if p[4]:
            return i
    return -1


def _export_complete_enough(m1_n=None, tick_n=None, phase_list=None):
    """M1+Tick 均需有实质文件；Tick 至少达到 M1 的一半（允许个别合约无成交）。"""
    pl = phase_list or _phase_list or parse_periods()
    if m1_n is None or tick_n is None:
        m1_n, tick_n = _exported_counts()
    need_m1 = any(not p[4] for p in pl)
    need_tick = any(p[4] for p in pl)
    if need_m1 and m1_n < 1:
        return False
    if need_tick:
        if tick_n < 1:
            return False
        if m1_n > 0 and tick_n < max(1, int(m1_n * 0.5)):
            return False
    return True


def _clear_day_done_marker(reason=''):
    path = _day_done_path()
    if os.path.isfile(path):
        try:
            os.remove(path)
            LogInfo('清除假完成标记 %s%s' % (path, (' (%s)' % reason) if reason else ''))
        except Exception as e:
            LogError('清除完成标记失败: %s' % e)


def _mark_day_export_done(n_files):
    m1_n, tick_n = _exported_counts()
    if not _export_complete_enough(m1_n, tick_n):
        LogError('拒绝标记完成：m1=%d tick=%d（Tick 未补全）' % (m1_n, tick_n))
        _clear_day_done_marker('incomplete')
        return False
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
        'tick_bars': int(g_params.get('tick_bars') or 0),
        'm1_files': int(m1_n),
        'tick_files': int(tick_n),
        'files': int(n_files if n_files else m1_n),
        'ts': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
    }
    try:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write('\n')
    except Exception as e:
        LogError('写完成标记失败: %s' % e)
        return False
    LogInfo('已标记今日导出完成 m1=%d tick=%d → %s' % (m1_n, tick_n, path))
    return True


def _is_day_export_done():
    path = _day_done_path()
    m1_n, tick_n = _exported_counts()
    if not _export_complete_enough(m1_n, tick_n):
        if os.path.isfile(path):
            _clear_day_done_marker('files incomplete m1=%d tick=%d' % (m1_n, tick_n))
        return False
    if not os.path.isfile(path):
        return False
    try:
        with open(path, 'r', encoding='utf-8-sig') as f:
            obj = json.load(f)
        day = g_params.get('_trading_day') or _trading_day_ymd()
        if str(obj.get('trading_day') or '') != str(day):
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
    LogInfo('请手动【停止】本策略；然后运行: python tools/import_ep_to_store.py')


def _out_dir_for_period():
    if _period is None:
        return _norm_dir(g_params['out_dir_m1'])
    if _period[4]:
        return _norm_dir(g_params['out_dir_tick'])
    return _norm_dir(g_params['out_dir_m1'])


def _csv_path(item):
    return os.path.join(_out_dir_for_period(), '%s.csv' % item['ctp_symbol'])


def _read_last_ts_from_csv(path, is_tick):
    """取文件中最大时间戳（不能只看末行：乱序/重复时末行可能不是最新）。"""
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
                # 日历 datetime 在第 3 列；Tick 的 DateTimeStamp 是交易日拼钟，不能当顺序键。
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


def _tick_aggressor(d):
    """优先用极智现手内外；否则用 LastQty 近似。"""
    try:
        ti = d.get('TickInnerQty', None)
        to = d.get('TickOuterQty', None)
        if ti is not None or to is not None:
            return ti if ti is not None else '', to if to is not None else ''
    except Exception:
        pass
    last_qty = d.get('LastQty', '') or ''
    return last_qty, ''


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
    """HisBarsInfo 常返回 numpy；不能用 if info:（会 ValueError），也不能只认 dict。"""
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
    """hisover 时用 HisBarsInfo 补全；Tick 必须经 _as_bar_list，否则 numpy 行被跳过。"""
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
            if is_tick:
                _prepare_file_meta(item)
                LogInfo('HisBarsInfo %s Tick 无成交，仍留空表（不剔除）' % item['ctp_symbol'])
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


def _cap_batch_size():
    batch_size = int(g_params.get('batch_size', MAX_CONTRACTS) or MAX_CONTRACTS)
    if batch_size <= 0 or batch_size > MAX_CONTRACTS:
        LogInfo('batch_size 超出上限，改为 %d' % MAX_CONTRACTS)
        batch_size = MAX_CONTRACTS
    return batch_size


def _subscribe_batch(batch, period, bars_n):
    bar_type, interval, file_tag = period[0], period[1], period[2]
    is_tick = bool(period[4]) if len(period) > 4 else (bar_type == 'T' and int(interval) == 0)
    for item in batch:
        if is_tick:
            n = max(int(bars_n), DEFAULT_TICK_BARS)
            rc = SetBarInterval(item['ep_code'], 'T', 0, 'A', n, True)
            LogInfo('订阅 Tick(全日 A) %s barsLen=%d → %s rc=%s'
                    % (item['ctp_symbol'], n, item['ep_code'], rc))
        else:
            rc = SetBarInterval(item['ep_code'], bar_type, interval, bars_n, bars_n, True)
            LogInfo('订阅 %s %s bars=%d → %s rc=%s'
                    % (item['ctp_symbol'], file_tag, bars_n, item['ep_code'], rc))


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

    _apply_day_output_dirs()  # 强制 live_update=False / periods=M1,TICK / bars

    _phase_list = parse_periods()
    if not _phase_list:
        LogError('periods 为空')
        return

    _all_rows = load_contract_rows()
    if not _all_rows:
        LogError('合约列表为空')
        return

    # 今日 M1+Tick 均已实质落盘 → 空闲
    if _is_day_export_done() and not LIVE_UPDATE:
        m1_n, tick_n = _exported_counts()
        LogInfo('今日导出已完成（m1=%d tick=%d），本策略空闲。请【停止】本策略。'
                % (m1_n, tick_n))
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

    # ---- 进度声称全量完成：仍须核对 Tick ----
    m1_n, tick_n = _exported_counts()
    if not _export_complete_enough(m1_n, tick_n):
        ti = _tick_phase_idx()
        if ti < 0:
            LogError('需要 Tick 但 periods 无 TICK：m1=%d tick=%d' % (m1_n, tick_n))
            return
        LogError('进度 done 但 Tick 不足（m1=%d tick=%d）→ 重跑 Tick 阶段' % (m1_n, tick_n))
        _clear_day_done_marker('rewind tick')
        save_progress(ti, 0, len(_all_rows), done=False)
        _phase_idx, _start_index = ti, 0
        _bulk_done = False
        _start_bulk_batch()
        return

    LogInfo('全量导出已完成（m1=%d tick=%d）' % (m1_n, tick_n))
    save_progress(len(_phase_list), 0, len(_all_rows), done=True,
                  live_phase_idx=live_phase_idx, live_next_index=live_next_index)
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
        # 历史 'H' 与实时 'K' 均落盘；hisover 再 HisBarsInfo 补全去重
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
        m1_n, tick_n = _exported_counts()
        if not _export_complete_enough(m1_n, tick_n):
            ti = _tick_phase_idx()
            LogError('全部周期跑完但 Tick 不足（m1=%d tick=%d）→ 重跑 Tick' % (m1_n, tick_n))
            _clear_day_done_marker('hisover incomplete')
            if ti >= 0:
                save_progress(ti, 0, len(_all_rows), done=False)
            else:
                save_progress(0, 0, len(_all_rows), done=False)
            _schedule_reload()
            return
        save_progress(phase_idx, 0, len(_all_rows), done=True,
                      live_phase_idx=0, live_next_index=0)
        _mark_day_export_done(m1_n)
        LogInfo('全部主力 M1+Tick 全量完成（m1=%d tick=%d），停止下载' % (m1_n, tick_n))
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
    LogInfo('VV_SevBootstrap 退出 mode=%s' % _mode)
