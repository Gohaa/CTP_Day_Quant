#include "config/app_config.h"
#include "logging/logger.h"
#include "market/bar_builder.h"
#include "market/depth_tick.h"
#include "market/parquet_bar_store.h"
#include "market/quote_tape.h"
#include "market/tick_bus.h"
#include "market/tick_day_store.h"
#include "market/tick_snapshot_book.h"
#include "md/md_gateway.h"
#include "md/subscription_manager.h"
#include "store/chip_profile.h"
#include "store/dominant_selector.h"
#include "store/dominant_table.h"
#include "store/instrument_store.h"
#include "store/liquidity.h"
#include "store/option_screener.h"
#include "store/session_calendar.h"
#include "td/td_gateway.h"
#include "oms/oms.h"
#include "risk/risk_guard.h"
#include "strategy/engine.h"
#include "strategy/pyramid.h"
#include "strategy/selftest.h"
#include "util/ctp_product_class.h"
#include "util/mask.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kBatchSampleMs = 10000;
constexpr int kBatchIdleMs = 1500;

void PrintHelp() {
  std::cout
      << "Quant_Sev - CTP futures quant\n"
      << "Usage:\n"
      << "  quant_sev --config path/to/live.env [options]\n"
      << "\n"
      << "  --run-seconds N         sample window (default 20); --live-ticks: 0 = until Ctrl+C\n"
      << "  --smoke-parquet         step09: write one synthetic bar then exit (no CTP)\n"
      << "  --select-dominant       step06: build dominant JSON from full futures universe\n"
      << "  --sync-instruments      step05: dominant basics -> Quant_Sev/data/live/instruments/dominant/\n"
      << "  --live-ticks            step07-09: Md stream; bars->klines; ticks->data/ticks/{day}/\n"
      << "                          also splice ep_raw+ticks -> data/live/quote/{TradingDay}/\n"
      << "  --quote-tape            splice 极智 ep_raw + CTP ticks → quote JSON (does not replace klines)\n"
      << "  --no-save-ticks         live-ticks: do not write tick CSV\n"
      << "  --dominant-only         live-ticks: dominant only (no second)\n"
      << "  --dominant-file PATH    override dominant JSON (else latest under DATA_ROOT)\n"
      << "  --print-all-ticks       print every tick (default when subscribe_n<=16)\n"
      << "  --allow-product-filter  DEBUG: honor DOMINANT_PRODUCTS (forbidden as default)\n"
      << "  --demo-switch           mid-run drop one (optional UnSub test; OFF by default)\n"
      << "  --chip                  compute chip profile (tick L1 or kline CYQ) then exit\n"
      << "  --chip-selftest         chip unit checks, no CTP / files\n"
      << "  --chip-mode tick|kline  (default tick)\n"
      << "  --chip-symbol KEY       product key e.g. SHFE.pb or instrument id\n"
      << "  --chip-day YYYYMMDD     CTP ticks day, else ep_raw (default latest ticks)\n"
      << "  --chip-dist triangle|uniform  kline day-cost (Tonghuashun default triangle)\n"
      << "  --chip-decay N          history turnover decay (default 1)\n"
      << "  --chip-lookback N       kline bars (0=all)\n"
      << "  --chip-ticks-csv PATH   override tick source\n"
      << "  --chip-bars-csv PATH    override kline source\n"
      << "  --chip-session          kline: only current product session (data/live/sessions)\n"
      << "  --chip-out PATH         write file (default data/live/chip/{id}_{mode}.csv)\n"
      << "  --option-screen         step14: near-expiry options + cheap TheoTV (read-only)\n"
      << "  --option-screen-selftest  DTE / BS / cheap unit checks, no CTP\n"
      << "  --option-screen-live    after file screen, Md-subscribe capped list then rescore\n"
      << "  --option-as-of YYYYMMDD DTE as-of (default Shanghai today)\n"
      << "  --option-dte-mode calendar|trading  (trading = weekdays only)\n"
      << "  --option-hv X           Black-76 sigma (default 0.22)\n"
      << "  --option-cheap-ratio X  MarketTV/TheoTV threshold (default 0.5)\n"
      << "  --option-max-spread N   reject if bid-ask > N ticks (0=skip; default 8)\n"
      << "  --option-max-subscribe N  live/subscribe cap (default 200)\n"
      << "  --trade-selftest        step11-13 + strategy: no CTP (rules/gates/pyramid/slices)\n"
      << "  --oms-dry-run           Td login + qry + compose 1 lot, do not send\n"
      << "  --oms-roundtrip         1 lot open+close (requires --enable-trading)\n"
      << "  --oms-product KEY       dominant product key (default SHFE.rb from JSON)\n"
      << "  --strategy-scan         score AI/manual/fixed on live quote+chip, write snapshot, no send\n"
      << "  --strategy-live-verify  1 lot open + RiskGuard TP flatten (needs --enable-trading)\n"
      << "  --strategy-live         Md all dominants + RuleOpen 1 lot + guard (needs --enable-trading)\n"
      << "  --max-positions N       strategy-live concurrent cap (default 8)\n"
      << "  --enable-trading        CLI only; does not write live.env\n";
}

std::string FormatTick(const qsev::DepthTick& t) {
  std::ostringstream oss;
  oss << t.instrument_id << " " << t.trading_day << " " << t.update_time << "." << t.update_millisec
      << " last=" << t.last_price << " vol=" << t.volume << " oi=" << t.open_interest << " | B1 "
      << t.bid_price[0] << "x" << t.bid_volume[0] << " A1 " << t.ask_price[0] << "x" << t.ask_volume[0];
  return oss.str();
}

std::string FormatBar(const qsev::Bar& b) {
  std::ostringstream oss;
  oss << b.product_key << " " << b.interval_min << "m"
      << " O=" << b.open << " H=" << b.high << " L=" << b.low << " C=" << b.close
      << " V=" << b.volume << " open_ns=" << b.datetime_ns << " close_sec=" << b.time_unix;
  if (!b.trading_day.empty()) oss << " td=" << b.trading_day;
  return oss.str();
}

std::unordered_map<std::string, std::string> BuildInstrumentProductMap(const qsev::DominantTable& table) {
  // K-lines are product-level (SHFE.rb). Only the dominant contract feeds BarBuilder.
  // Second is still subscribed and written to data/ticks/{day}/{id}.csv, but CTP Volume
  // is per-instrument — mixing dominant+second into one series dumps huge dvol.
  std::unordered_map<std::string, std::string> out;
  for (const auto& e : table.Entries()) {
    if (!e.dominant.empty()) out[e.dominant] = e.product_key;
  }
  return out;
}

qsev::SessionCalendar LoadSessionsForBars(const qsev::AppConfig& cfg) {
  namespace fs = std::filesystem;
  const fs::path latest = fs::path(cfg.data_root) / "live" / "sessions" / "latest.json";
  if (fs::exists(latest)) {
    auto cal = qsev::SessionCalendar::LoadFromFile(latest.string());
    if (!cal.Empty()) return cal;
  }
  if (!cfg.sessions_url.empty()) {
    const fs::path cache_dir = fs::path(cfg.data_root) / "live" / "sessions";
    try {
      fs::create_directories(cache_dir);
    } catch (...) {
    }
    auto cal = qsev::SessionCalendar::FetchOpenCtpTimes(cfg.sessions_url, latest.string());
    if (!cal.Empty()) return cal;
  }
  qsev::Logger::Warn("bars", "SessionCalendar empty; minute bars still align, daily uses 09:00-15:00 fallback");
  return {};
}

bool WaitStoreReady(const qsev::InstrumentStore& store, int timeout_sec) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  size_t last_size = 0;
  auto last_grow = std::chrono::steady_clock::now();
  auto last_log = last_grow;
  while (std::chrono::steady_clock::now() < deadline) {
    if (store.Ready() && store.Size() > 0) return true;
    const size_t sz = store.Size();
    const auto now = std::chrono::steady_clock::now();
    if (sz != last_size) {
      last_size = sz;
      last_grow = now;
    }
    if (now - last_log >= std::chrono::seconds(5)) {
      qsev::Logger::Info("dominant", "InstrumentStore loading size=" + std::to_string(sz));
      last_log = now;
    }
    if (sz >= 10000 && now - last_grow >= std::chrono::seconds(20)) {
      qsev::Logger::Warn("dominant",
                         "InstrumentStore stall size=" + std::to_string(sz) +
                             " (no bIsLast yet); continuing");
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return store.Ready() || store.Size() > 0;
}

bool WaitStoreEnriched(const qsev::InstrumentStore& store, int timeout_sec) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);
  auto last_log = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    if (store.Enriched()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(10)) {
      qsev::Logger::Info("instruments",
                         "waiting enrich (DepthMarketData+MarginRate) size=" +
                             std::to_string(store.Size()));
      last_log = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  return store.Enriched();
}

size_t CountCovered(const qsev::TickSnapshotBook& book, const std::vector<std::string>& ids) {
  size_t n = 0;
  for (const auto& id : ids) {
    if (book.Get(id).has_data) ++n;
  }
  return n;
}

void SampleBatch(qsev::TickBus& bus, qsev::TickSnapshotBook& book, std::string& trading_day,
                 size_t& n_ticks, const std::vector<std::string>& batch, int max_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
  size_t last_covered = 0;
  auto last_grow = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    bool got = false;
    while (auto t = bus.TryPop()) {
      got = true;
      ++n_ticks;
      book.Update(*t);
      if (trading_day.empty() && t->trading_day[0] != '\0') trading_day = t->trading_day;
      if (n_ticks <= 15 || n_ticks % 1000 == 0) {
        qsev::Logger::Info("tick", FormatTick(*t));
      }
    }
    const size_t covered = CountCovered(book, batch);
    const auto now = std::chrono::steady_clock::now();
    if (covered > last_covered) {
      last_covered = covered;
      last_grow = now;
    }
    if (covered > 0 && now - last_grow >= std::chrono::milliseconds(kBatchIdleMs)) break;
    if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool WaitMdLogin(qsev::MdGateway& md, int timeout_ms = 5000) {
  const int steps = std::max(1, timeout_ms / 100);
  for (int i = 0; i < steps && !md.LoggedIn(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return md.LoggedIn();
}

void LogLiveStats(const qsev::TickSnapshotBook& book, const std::vector<std::string>& watched,
                  double stale_sec, double window_sec) {
  const auto stats = book.Stats(window_sec, stale_sec);
  size_t covered = 0;
  size_t depth_ok = 0;
  size_t quiet = 0;       // had ticks, but no update for > stale_sec (illiquid)
  size_t never = 0;       // subscribed id never appeared in book (true miss)
  uint64_t ticks = 0;
  for (const auto& id : watched) {
    const auto it = std::find_if(stats.begin(), stats.end(),
                                 [&](const qsev::InstrumentTickStats& s) { return s.instrument_id == id; });
    if (it == stats.end() || !it->has_data) {
      ++never;
      continue;
    }
    ++covered;
    ticks += it->tick_count;
    if (it->depth_ok) ++depth_ok;
    if (it->stale) ++quiet;
  }
  qsev::Logger::Info("live", "watched=" + std::to_string(watched.size()) +
                                 " covered=" + std::to_string(covered) +
                                 " depth_ok=" + std::to_string(depth_ok) +
                                 " quiet=" + std::to_string(quiet) + " (no tick >" +
                                 std::to_string(static_cast<int>(stale_sec)) + "s)" +
                                 " never=" + std::to_string(never) +
                                 " ticks=" + std::to_string(ticks) +
                                 " book=" + std::to_string(book.Size()));

  std::vector<qsev::InstrumentTickStats> hot;
  for (const auto& s : stats) {
    if (s.has_data) hot.push_back(s);
  }
  std::sort(hot.begin(), hot.end(),
            [](const qsev::InstrumentTickStats& a, const qsev::InstrumentTickStats& b) {
              return a.ticks_per_sec > b.ticks_per_sec;
            });
  for (size_t i = 0; i < hot.size() && i < 5; ++i) {
    qsev::Logger::Info("live", "hot " + hot[i].instrument_id + " tps=" +
                                   std::to_string(hot[i].ticks_per_sec) + " n=" +
                                   std::to_string(hot[i].tick_count) + " age=" +
                                   std::to_string(hot[i].seconds_since_update) + "s");
  }
  size_t shown_never = 0;
  size_t shown_quiet = 0;
  for (const auto& id : watched) {
    const auto it = std::find_if(stats.begin(), stats.end(),
                                 [&](const qsev::InstrumentTickStats& s) { return s.instrument_id == id; });
    if (it == stats.end() || !it->has_data) {
      if (shown_never < 8) {
        qsev::Logger::Warn("live", "NEVER_TICK (subscribe/id?) " + id);
        ++shown_never;
      }
      continue;
    }
    if (!it->stale) continue;
    if (shown_quiet >= 8) continue;
    qsev::Logger::Warn("live", "quiet " + id + " age=" + std::to_string(it->seconds_since_update) +
                                   "s ticks_total=" + std::to_string(it->tick_count) +
                                   " (subscribed OK; illiquid/no recent trade)");
    ++shown_quiet;
  }
}

int RunLiveTicks(qsev::AppConfig& cfg, int run_seconds, bool demo_switch, bool dominant_only,
                 bool print_all_ticks, bool allow_product_filter,
                 const std::string& dominant_file_override, bool save_ticks) {
  cfg.md_instruments.clear();

  qsev::TickBus bus;
  qsev::TickSnapshotBook book(static_cast<size_t>(std::max(1, cfg.tick_ring_size)));
  qsev::MdGateway md(cfg, bus);
  if (!md.Start()) return 4;
  if (!WaitMdLogin(md)) {
    qsev::Logger::Error("live", "Md login timeout");
    md.Stop();
    return 5;
  }

  qsev::DominantTable table;
  try {
    if (!dominant_file_override.empty()) {
      table = qsev::DominantTable::LoadFromFile(dominant_file_override);
    } else if (!cfg.dominant_file.empty()) {
      table = qsev::DominantTable::LoadFromFile(cfg.dominant_file);
    } else {
      table = qsev::DominantTable::LoadLatest(cfg.data_root);
    }
  } catch (const std::exception& e) {
    qsev::Logger::Error("live", e.what());
    md.Stop();
    return 6;
  }

  // Default: ALL products from dominant_table (CTP-derived). No hardcoded whitelist.
  std::vector<std::string> product_filter;
  if (allow_product_filter && !cfg.dominant_products.empty()) {
    qsev::Logger::Warn("live", "DEBUG --allow-product-filter; using DOMINANT_PRODUCTS");
    product_filter = cfg.dominant_products;
  } else if (!cfg.dominant_products.empty()) {
    qsev::Logger::Warn("live",
                       "DOMINANT_PRODUCTS ignored (see plan/no-hardcode-ctp.md). "
                       "Subscribe ALL from dominant JSON. Debug only: --allow-product-filter");
  }

  // skip_zero_oi=false: every dominant/second InstrumentID in the table is subscribed.
  auto ids = table.InstrumentIds(product_filter, /*skip_zero_oi=*/false, !dominant_only);
  {
    std::ostringstream oss;
    oss << "KEEP subscribe ALL mode=" << (dominant_only ? "dominant-only" : "dominant+second")
        << " day=" << table.TradingDay() << " products_in_table=" << table.Entries().size()
        << " n=" << ids.size() << " ids=";
    for (size_t i = 0; i < ids.size(); ++i) {
      if (i) oss << ",";
      if (i >= 40) {
        oss << "...(+" << (ids.size() - i) << ")";
        break;
      }
      oss << ids[i];
    }
    qsev::Logger::Info("live", oss.str());
  }
  if (ids.empty()) {
    qsev::Logger::Error("live", "no instruments from dominant table");
    md.Stop();
    return 7;
  }

  const bool verbose_ticks = print_all_ticks || ids.size() <= 16;
  qsev::Logger::Info("live", std::string("print_every_tick=") + (verbose_ticks ? "true" : "false") +
                                 " run_seconds=" + std::to_string(run_seconds) +
                                 " (subscription stays until process exit)");

  const auto instr_to_product = BuildInstrumentProductMap(table);
  qsev::SessionCalendar sessions = LoadSessionsForBars(cfg);
  qsev::MultiBarBuilder bars(&sessions);
  qsev::ParquetBarStore bar_store(cfg.data_root);
  qsev::TickDayStore tick_store(cfg.data_root);
  qsev::QuoteTapeBuilder quote_tape_builder(cfg.data_root, &sessions);
  size_t n_bars_closed = 0;
  auto last_tick_flush = std::chrono::steady_clock::now();
  auto last_quote_tape = std::chrono::steady_clock::now();
  const auto on_bar = [&](const qsev::Bar& b) {
    ++n_bars_closed;
    bar_store.Append(b);
    if (n_bars_closed <= 80 || n_bars_closed % 50 == 0) {
      qsev::Logger::Info("bar", FormatBar(b));
    }
  };
  qsev::Logger::Info("bars", "MultiBarBuilder+ParquetBarStore ready sessions=" +
                                 std::to_string(sessions.Size()) +
                                 " instr_map=" + std::to_string(instr_to_product.size()) +
                                 " data_root=" + cfg.data_root +
                                 " save_ticks=" + (save_ticks ? "true" : "false"));

  qsev::SubscriptionManager subs(md);
  if (!subs.Sync(ids)) {
    qsev::Logger::Warn("live", "initial Sync reported failures");
  }
  qsev::Logger::Info("live", "Md active_subs=" + std::to_string(md.ActiveSubscriptions().size()) +
                                 " — streaming ALL dominants...");

  const auto start = std::chrono::steady_clock::now();
  const bool forever = run_seconds <= 0;
  const auto deadline = start + std::chrono::seconds(forever ? 1 : run_seconds);
  const auto switch_at = start + std::chrono::seconds(std::max(1, run_seconds / 2));
  bool switched = false;
  size_t n_ticks = 0;
  size_t ticks_window = 0;
  auto last_stat = start;
  auto last_hz = start;
  std::string dropped;
  std::string live_trading_day;

  qsev::Logger::Info("live", forever ? "run until Ctrl+C (ticks+bars continuous)"
                                     : ("run_seconds=" + std::to_string(run_seconds)));

  while (forever || std::chrono::steady_clock::now() < deadline) {
    while (auto t = bus.TryPop()) {
      ++n_ticks;
      ++ticks_window;
      book.Update(*t);
      if (save_ticks) {
        tick_store.Append(*t);
      }
      if (live_trading_day.empty() && t->trading_day[0] != '\0') {
        live_trading_day = t->trading_day;
      }
      if (verbose_ticks || n_ticks <= 40 || n_ticks % 200 == 0) {
        qsev::Logger::Info("tick", FormatTick(*t));
      }
      const auto it = instr_to_product.find(t->instrument_id);
      if (it != instr_to_product.end()) {
        bars.OnTick(it->second, *t, on_bar);
      }
    }
    if (bar_store.PendingBars() > 0) {
      const auto nf = bar_store.Flush();
      qsev::Logger::Info("klines", "persist closed files=" + std::to_string(nf) +
                                       " bars_closed=" + std::to_string(n_bars_closed));
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_hz >= std::chrono::seconds(1)) {
      qsev::Logger::Info("live", "heartbeat ticks_1s=" + std::to_string(ticks_window) +
                                     " total=" + std::to_string(n_ticks) +
                                     " active_subs=" + std::to_string(subs.Current().size()) +
                                     " covered=" + std::to_string(CountCovered(book, ids)));
      ticks_window = 0;
      last_hz = now;
    }

    if (demo_switch && !switched && now >= switch_at && ids.size() >= 2) {
      switched = true;
      dropped = ids.back();
      auto next = ids;
      next.pop_back();
      qsev::Logger::Info("live", "demo-switch drop=" + dropped);
      if (!subs.Sync(next)) {
        qsev::Logger::Warn("live", "demo-switch Sync failed");
      }
      ids = next;
    }

    if (now - last_stat >= std::chrono::seconds(5)) {
      const double window = std::chrono::duration<double>(now - start).count();
      LogLiveStats(book, ids, cfg.tick_stale_sec, window);
      if (switched && !dropped.empty()) {
        const auto st = book.Stats(window, cfg.tick_stale_sec);
        auto it = std::find_if(st.begin(), st.end(), [&](const qsev::InstrumentTickStats& s) {
          return s.instrument_id == dropped;
        });
        if (it != st.end()) {
          qsev::Logger::Info("live", "dropped " + dropped + " age=" +
                                         std::to_string(it->seconds_since_update) + "s");
        }
      }
      last_stat = now;
    }
    if (save_ticks && now - last_tick_flush >= std::chrono::seconds(2)) {
      const auto n = tick_store.Flush();
      if (n > 0) {
        qsev::Logger::Info("ticks", "flushed +" + std::to_string(n) +
                                        " total=" + std::to_string(tick_store.TotalWritten()) +
                                        " dir=" + cfg.data_root + "/ticks/{TradingDay}/");
      }
      last_tick_flush = now;
    }
    if (now - last_quote_tape >= std::chrono::seconds(60)) {
      if (save_ticks) tick_store.Flush();
      quote_tape_builder.RebuildAll(table, /*write_klines=*/false, live_trading_day);
      last_quote_tape = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  bars.Flush(on_bar);
  const std::size_t n_files = bar_store.Flush();
  if (save_ticks) {
    tick_store.Flush();
  }
  const double window =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  LogLiveStats(book, ids, cfg.tick_stale_sec, window);
  qsev::Logger::Info("live", "done continuous stream ticks=" + std::to_string(n_ticks) +
                                 " bars_closed=" + std::to_string(n_bars_closed) +
                                 " parquet_files=" + std::to_string(n_files) +
                                 " ticks_saved=" + std::to_string(tick_store.TotalWritten()) +
                                 " pending=" + std::to_string(bar_store.PendingBars()) +
                                 " seconds=" + std::to_string(run_seconds) +
                                 " still_subscribed=" + std::to_string(subs.Current().size()));
  if (!ids.empty()) {
    const auto ring = book.RingCopy(ids.front());
    qsev::Logger::Info("live", "ring sample id=" + ids.front() +
                                   " size=" + std::to_string(ring.size()));
  }

  bus.Close();
  md.Stop();
  return 0;
}

int RunSyncInstruments(qsev::AppConfig& cfg) {
  // step05: 主力合约基本信息 + 每日保证金/涨跌停校验。依赖 step06 dominant JSON。
  qsev::DominantTable table;
  try {
    if (!cfg.dominant_file.empty()) {
      table = qsev::DominantTable::LoadFromFile(cfg.dominant_file);
    } else {
      table = qsev::DominantTable::LoadLatest(cfg.data_root);
    }
  } catch (const std::exception& e) {
    qsev::Logger::Error("instruments",
                        std::string("need dominant table first (--select-dominant): ") + e.what());
    return 5;
  }

  auto target_ids = table.InstrumentIds({}, /*skip_zero_oi=*/false, /*include_second=*/false);
  if (target_ids.empty()) {
    qsev::Logger::Error("instruments", "dominant table has no dominant instrument ids");
    return 5;
  }
  qsev::Logger::Info("instruments", "dominant targets n=" + std::to_string(target_ids.size()) +
                                        " day=" + table.TradingDay());

  qsev::InstrumentStore store;
  qsev::TdGateway td(cfg, store);
  td.EnableEnrichment(true);
  td.SetEnrichTargets(target_ids);
  if (!td.Start()) return 4;

  qsev::Logger::Info("instruments", "waiting InstrumentStore (full ReqQryInstrument for lookup)...");
  if (!WaitStoreReady(store, 180)) {
    qsev::Logger::Error("instruments", "InstrumentStore not ready size=" + std::to_string(store.Size()));
    td.Stop();
    return 6;
  }

  qsev::Logger::Info("instruments", "waiting enrich for dominant only (DepthMarketData + MarginRate)...");
  if (!WaitStoreEnriched(store, 300)) {
    qsev::Logger::Warn("instruments",
                       "enrich timeout — dumping partial (limits/margins may be incomplete)");
  }

  const auto day = store.TradingDay().empty() ? table.TradingDay() : store.TradingDay();
  std::vector<qsev::InstrumentInfo> rows;
  rows.reserve(target_ids.size());
  for (const auto& id : target_ids) {
    const auto* p = store.Find(id);
    if (!p) {
      qsev::Logger::Warn("instruments", "dominant id missing in store: " + id);
      continue;
    }
    rows.push_back(*p);
  }

  // CTP has no open/close clock. Fetch openctp /times (live); cache under data/live/sessions/.
  // Note: http://dict.openctp.cn/instruments has NO TimeBegin/TimeEnd — use /times.
  namespace fs = std::filesystem;
  const fs::path cache_dir = fs::path(cfg.data_root) / "live" / "sessions";
  const std::string cache_path = (cache_dir / "latest.json").string();
  const std::string day_cache =
      (cache_dir / ((day.empty() ? std::string("unknown") : day) + ".json")).string();
  qsev::SessionCalendar calendar;
  if (!cfg.sessions_url.empty()) {
    calendar = qsev::SessionCalendar::FetchOpenCtpTimes(cfg.sessions_url, cache_path);
    if (!calendar.Empty()) {
      calendar.WriteCache(day_cache);
    }
  }
  if (calendar.Empty() && fs::exists(cache_path)) {
    qsev::Logger::Warn("instruments", "sessions fetch failed; using cache " + cache_path);
    calendar = qsev::SessionCalendar::LoadFromFile(cache_path);
  }
  int sess_hit = 0;
  int sess_miss = 0;
  for (auto& e : rows) {
    const auto pk = qsev::InstrumentStore::ProductKey(e.exchange_id, e.product_id);
    if (const auto* segs = calendar.Find(pk)) {
      store.PatchSessions(e.instrument_id, *segs);
      if (const auto* updated = store.Find(e.instrument_id)) e = *updated;
      ++sess_hit;
    } else {
      qsev::Logger::Warn("instruments", "no trading sessions for " + pk);
      ++sess_miss;
    }
  }
  qsev::Logger::Info("instruments", "sessions attached hit=" + std::to_string(sess_hit) +
                                        " miss=" + std::to_string(sess_miss) +
                                        " products=" + std::to_string(calendar.Size()) +
                                        " source=" + calendar.Source());

  const fs::path out_dir = fs::path(cfg.data_root) / "live" / "instruments" / "dominant";
  const std::string out_path = (out_dir / (day + ".json")).string();
  const std::string latest_path = (out_dir / "latest.json").string();

  if (fs::exists(latest_path)) {
    qsev::InstrumentStore prev;
    if (prev.LoadJson(latest_path)) {
      int changed = 0;
      for (const auto& e : rows) {
        const auto* o = prev.Find(e.instrument_id);
        if (!o) {
          qsev::Logger::Info("instruments", "verify NEW " + e.instrument_id);
          ++changed;
          continue;
        }
        auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
        if (!near(o->long_margin_ratio_by_money, e.long_margin_ratio_by_money) ||
            !near(o->exch_long_margin_ratio_by_money, e.exch_long_margin_ratio_by_money) ||
            !near(o->upper_limit_price, e.upper_limit_price) ||
            !near(o->lower_limit_price, e.lower_limit_price) || o->expire_date != e.expire_date ||
            o->session_day_open != e.session_day_open || o->session_day_close != e.session_day_close ||
            o->session_night_open != e.session_night_open ||
            o->session_night_close != e.session_night_close) {
          qsev::Logger::Warn(
              "instruments",
              "verify CHANGED " + e.instrument_id + " inv_m " +
                  std::to_string(o->long_margin_ratio_by_money) + "->" +
                  std::to_string(e.long_margin_ratio_by_money) + " day " + o->session_day_open + "-" +
                  o->session_day_close + "->" + e.session_day_open + "-" + e.session_day_close +
                  " night " + o->session_night_open + "-" + o->session_night_close + "->" +
                  e.session_night_open + "-" + e.session_night_close);
          ++changed;
        }
      }
      qsev::Logger::Info("instruments", "daily verify vs latest.json changed=" +
                                            std::to_string(changed) + "/" +
                                            std::to_string(rows.size()));
    }
  }

  if (!qsev::InstrumentStore::WriteJson(out_path, day, rows)) {
    td.Stop();
    return 8;
  }
  try {
    fs::copy_file(out_path, latest_path, fs::copy_options::overwrite_existing);
    qsev::Logger::Info("instruments", "updated " + latest_path);
  } catch (const std::exception& e) {
    qsev::Logger::Warn("instruments", std::string("latest.json copy failed: ") + e.what());
  }

  for (size_t i = 0; i < rows.size() && i < 8; ++i) {
    const auto& p = rows[i];
    qsev::Logger::Info(
        "instruments",
        "sample " + p.instrument_id + " expire=" + p.expire_date + " day=" + p.session_day_open +
            "-" + p.session_day_close + " night=" + p.session_night_open + "-" +
            p.session_night_close + " segs=" + std::to_string(p.sessions.size()) +
            " upper=" + std::to_string(p.upper_limit_price) +
            " inv_long_m=" + std::to_string(p.long_margin_ratio_by_money));
  }

  td.Stop();
  qsev::Logger::Info("instruments", "step05 dominant basics saved count=" +
                                        std::to_string(rows.size()) + " path=" + out_path);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string dominant_file_override;
  int run_seconds = 20;
  bool run_seconds_set = false;
  bool demo_ticks = false;
  bool smoke_parquet = false;
  bool select_dominant = false;
  bool sync_instruments = false;
  bool live_ticks = false;
  bool quote_tape = false;
  bool demo_switch = false;
  bool dominant_only = false;
  bool print_all_ticks = false;
  bool allow_product_filter = false;
  bool save_ticks = true;
  qsev::ChipCliOptions chip_opt;
  qsev::OptionScreenCliOptions opt_screen;
  bool trade_selftest = false;
  bool oms_dry_run = false;
  bool oms_roundtrip = false;
  bool strategy_scan = false;
  bool strategy_live_verify = false;
  bool strategy_live = false;
  int max_positions = 8;
  bool enable_trading_cli = false;
  std::string oms_product = "SHFE.rb";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      PrintHelp();
      return 0;
    }
    if (a == "--config" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--run-seconds" && i + 1 < argc) {
      run_seconds = std::atoi(argv[++i]);
      run_seconds_set = true;
    } else if (a == "--dominant-file" && i + 1 < argc) {
      dominant_file_override = argv[++i];
    } else if (a == "--demo-ticks") {
      demo_ticks = true;
    } else if (a == "--smoke-parquet") {
      smoke_parquet = true;
    } else if (a == "--select-dominant") {
      select_dominant = true;
    } else if (a == "--sync-instruments") {
      sync_instruments = true;
    } else if (a == "--live-ticks") {
      live_ticks = true;
    } else if (a == "--quote-tape") {
      quote_tape = true;
    } else if (a == "--demo-switch") {
      demo_switch = true;
    } else if (a == "--dominant-only") {
      dominant_only = true;
    } else if (a == "--print-all-ticks") {
      print_all_ticks = true;
    } else if (a == "--allow-product-filter") {
      allow_product_filter = true;
    } else if (a == "--no-save-ticks") {
      save_ticks = false;
    } else if (a == "--chip") {
      chip_opt.enabled = true;
    } else if (a == "--chip-selftest") {
      chip_opt.enabled = true;
      chip_opt.selftest = true;
    } else if (a == "--chip-mode" && i + 1 < argc) {
      chip_opt.mode = argv[++i];
    } else if (a == "--chip-symbol" && i + 1 < argc) {
      chip_opt.symbol = argv[++i];
    } else if (a == "--chip-day" && i + 1 < argc) {
      chip_opt.day = argv[++i];
    } else if (a == "--chip-out" && i + 1 < argc) {
      chip_opt.out_path = argv[++i];
    } else if (a == "--chip-ticks-csv" && i + 1 < argc) {
      chip_opt.ticks_csv = argv[++i];
    } else if (a == "--chip-bars-csv" && i + 1 < argc) {
      chip_opt.bars_csv = argv[++i];
    } else if (a == "--chip-dist" && i + 1 < argc) {
      chip_opt.dist = argv[++i];
    } else if (a == "--chip-decay" && i + 1 < argc) {
      chip_opt.decay = std::atof(argv[++i]);
    } else if (a == "--chip-lookback" && i + 1 < argc) {
      chip_opt.lookback = std::atoi(argv[++i]);
    } else if (a == "--chip-session") {
      chip_opt.session_window = true;
    } else if (a == "--option-screen") {
      opt_screen.enabled = true;
    } else if (a == "--option-screen-selftest") {
      opt_screen.enabled = true;
      opt_screen.selftest = true;
    } else if (a == "--option-screen-live") {
      opt_screen.enabled = true;
      opt_screen.live = true;
    } else if (a == "--option-as-of" && i + 1 < argc) {
      opt_screen.as_of = argv[++i];
    } else if (a == "--option-dte-mode" && i + 1 < argc) {
      opt_screen.dte_mode = argv[++i];
    } else if (a == "--option-dte-min" && i + 1 < argc) {
      opt_screen.dte_min = std::atoi(argv[++i]);
    } else if (a == "--option-dte-max" && i + 1 < argc) {
      opt_screen.dte_max = std::atoi(argv[++i]);
    } else if (a == "--option-hv" && i + 1 < argc) {
      opt_screen.hv = std::atof(argv[++i]);
    } else if (a == "--option-rate" && i + 1 < argc) {
      opt_screen.rate = std::atof(argv[++i]);
    } else if (a == "--option-cheap-ratio" && i + 1 < argc) {
      opt_screen.cheap_ratio = std::atof(argv[++i]);
    } else if (a == "--option-min-theo" && i + 1 < argc) {
      opt_screen.min_theo_tv = std::atof(argv[++i]);
    } else if (a == "--option-min-gap" && i + 1 < argc) {
      opt_screen.min_gap = std::atof(argv[++i]);
    } else if (a == "--option-max-spread" && i + 1 < argc) {
      opt_screen.max_spread_ticks = std::atoi(argv[++i]);
    } else if (a == "--option-min-oi" && i + 1 < argc) {
      opt_screen.min_oi = std::atof(argv[++i]);
    } else if (a == "--option-min-volume" && i + 1 < argc) {
      opt_screen.min_volume = std::atof(argv[++i]);
    } else if (a == "--option-max-subscribe" && i + 1 < argc) {
      opt_screen.max_subscribe = std::atoi(argv[++i]);
    } else if (a == "--option-instruments" && i + 1 < argc) {
      opt_screen.instruments_path = argv[++i];
    } else if (a == "--option-out" && i + 1 < argc) {
      opt_screen.out_dir = argv[++i];
    } else if (a == "--option-no-spot") {
      opt_screen.include_spot = false;
    } else if (a == "--option-dominant-only") {
      opt_screen.include_second = false;
    } else if (a == "--trade-selftest") {
      trade_selftest = true;
    } else if (a == "--oms-dry-run") {
      oms_dry_run = true;
    } else if (a == "--oms-roundtrip") {
      oms_roundtrip = true;
    } else if (a == "--strategy-scan") {
      strategy_scan = true;
    } else if (a == "--strategy-live-verify") {
      strategy_live_verify = true;
    } else if (a == "--strategy-live") {
      strategy_live = true;
    } else if (a == "--max-positions" && i + 1 < argc) {
      max_positions = std::atoi(argv[++i]);
    } else if (a == "--enable-trading") {
      enable_trading_cli = true;
    } else if (a == "--oms-product" && i + 1 < argc) {
      oms_product = argv[++i];
    }
  }

  if (chip_opt.selftest) {
    return qsev::RunChipSelfTest();
  }
  if (opt_screen.selftest) {
    return qsev::RunOptionScreenSelfTest();
  }

  qsev::AppConfig cfg;
  try {
    if (!config_path.empty()) {
      cfg = qsev::AppConfig::LoadFromEnvFile(config_path);
    } else {
      cfg = qsev::AppConfig::LoadFromEnvironment();
      cfg.FinalizePaths();
    }
  } catch (const std::exception& e) {
    qsev::Logger::Error("engine", e.what());
    return 2;
  }

  if (!cfg.production_mode) {
    qsev::Logger::Error("engine", "production_mode=false is forbidden");
    return 3;
  }

  if (enable_trading_cli) cfg.enable_trading = true;

  if (trade_selftest) {
    int rc = 0;
    rc |= qsev::RunTradeSelfTest(cfg.data_root);
    rc |= qsev::RunLiquiditySelfTest();
    rc |= qsev::RunRiskSelfTest(cfg.data_root);
    rc |= qsev::strategy::RunPyramidSelfTest(cfg.data_root);
    rc |= qsev::strategy::RunStrategySelfTest(cfg.data_root);
    return rc == 0 ? 0 : 11;
  }

  if (oms_dry_run || oms_roundtrip) {
    qsev::Logger::Info("engine", "Quant_Sev starting (OMS / step11-13)");
    qsev::Logger::Info("engine", "td_front=" + cfg.td_front);
    qsev::Logger::Info("engine", "user_id=" + cfg.user_id);
    qsev::Logger::Info("engine", "password=" + qsev::MaskSecret(cfg.password));
    qsev::Logger::Info("engine", std::string("enable_trading=") + (cfg.enable_trading ? "true" : "false"));
    return qsev::RunOmsSession(cfg, oms_roundtrip, oms_product);
  }

  if (strategy_scan) {
    return qsev::strategy::RunStrategyScan(cfg, oms_product);
  }

  if (strategy_live_verify) {
    qsev::Logger::Info("engine", "Quant_Sev strategy live-verify");
    qsev::Logger::Info("engine", std::string("enable_trading=") + (cfg.enable_trading ? "true" : "false"));
    return qsev::RunStrategyLiveVerify(cfg, oms_product);
  }

  if (strategy_live) {
    return qsev::strategy::RunStrategyLive(cfg, max_positions);
  }

  if (chip_opt.enabled) {
    if (chip_opt.symbol.empty() && chip_opt.ticks_csv.empty() && chip_opt.bars_csv.empty()) {
      qsev::Logger::Error("chip", "--chip-symbol (or --chip-ticks-csv / --chip-bars-csv) required");
      return 2;
    }
    return qsev::RunChip(chip_opt, cfg.data_root);
  }

  if (opt_screen.enabled) {
    if (!dominant_file_override.empty()) opt_screen.dominant_path = dominant_file_override;
    if (run_seconds_set) opt_screen.live_seconds = run_seconds;
    return qsev::RunOptionScreen(opt_screen, cfg.data_root, &cfg);
  }

  if (quote_tape) {
    return qsev::RunQuoteTape(cfg.data_root, dominant_file_override);
  }

  if (smoke_parquet) {
    // Sample from consider.md section 0.2: SHFE.rb 5m Shanghai 09:50-09:55
    qsev::Bar b;
    b.product_key = "_SMOKE.test";
    b.interval_min = 5;
    b.datetime_ns = 1766022600000000000LL;
    b.open = 1.0;
    b.high = 2.0;
    b.low = 0.5;
    b.close = 1.5;
    b.volume = 10.0;
    b.time_unix = 1766022900;
    qsev::ParquetBarStore store(cfg.data_root);
    const std::string path = qsev::ParquetBarStore::PathFor(cfg.data_root, b);
    qsev::Logger::Info("parquet", "smoke path=" + path);
    store.Append(b);
    if (store.Flush() < 1) {
      qsev::Logger::Error("parquet", "smoke flush failed");
      return 9;
    }
    qsev::Logger::Info("parquet", "smoke ok");
    return 0;
  }

  if (sync_instruments) {
    qsev::Logger::Info("engine", "Quant_Sev starting (sync-instruments / step05)");
    qsev::Logger::Info("engine", "td_front=" + cfg.td_front);
    qsev::Logger::Info("engine", "user_id=" + cfg.user_id);
    qsev::Logger::Info("engine", "password=" + qsev::MaskSecret(cfg.password));
    return RunSyncInstruments(cfg);
  }

  if (live_ticks) {
    if (!run_seconds_set) run_seconds = 0;  // all-session: CTP has no history, keep streaming
    qsev::Logger::Info("engine", "Quant_Sev starting (live-ticks / step07)");
    qsev::Logger::Info("engine", "md_front=" + cfg.md_front);
    qsev::Logger::Info("engine", "user_id=" + cfg.user_id);
    qsev::Logger::Info("engine", "password=" + qsev::MaskSecret(cfg.password));
    return RunLiveTicks(cfg, run_seconds, demo_switch, dominant_only, print_all_ticks,
                        allow_product_filter, dominant_file_override, save_ticks);
  }

  if (select_dominant) cfg.md_instruments.clear();

  qsev::Logger::Info("engine", "Quant_Sev starting");
  qsev::Logger::Info("engine", "md_front=" + cfg.md_front);
  qsev::Logger::Info("engine", "td_front=" + cfg.td_front);
  qsev::Logger::Info("engine", "broker_id=" + cfg.broker_id);
  qsev::Logger::Info("engine", "user_id=" + cfg.user_id);
  qsev::Logger::Info("engine", "password=" + qsev::MaskSecret(cfg.password));
  qsev::Logger::Info("engine", std::string("enable_trading=") + (cfg.enable_trading ? "true" : "false"));
  qsev::Logger::Info("engine", std::string("select_dominant=") + (select_dominant ? "true" : "false"));

  qsev::TickBus bus;
  qsev::TickSnapshotBook book(static_cast<size_t>(std::max(1, cfg.tick_ring_size)));
  qsev::InstrumentStore store;
  qsev::MdGateway md(cfg, bus);
  qsev::TdGateway td(cfg, store);

  if (!md.Start()) return 4;
  if (!td.Start()) {
    md.Stop();
    return 5;
  }

  if (demo_ticks && !select_dominant) {
    for (int i = 0; i < 5; ++i) {
      qsev::DepthTick t;
      qsev::CopyCStr(t.instrument_id, sizeof(t.instrument_id), "rb2510");
      t.last_price = 3100;
      t.volume = 1;
      t.open_interest = 1;
      bus.Push(t);
    }
  }

  if (select_dominant) {
    qsev::Logger::Info("dominant", "waiting InstrumentStore (Td ReqQryInstrument empty=all)...");
    if (!WaitStoreReady(store, 180)) {
      qsev::Logger::Error("dominant", "InstrumentStore not ready, size=" + std::to_string(store.Size()));
      td.Stop();
      md.Stop();
      return 6;
    }

    const auto cls = qsev::DominantSelector::CountClasses(store);
    qsev::Logger::Info("dominant",
                       "store size=" + std::to_string(cls.total) + " futures=" + std::to_string(cls.futures) +
                           " futures_trading=" + std::to_string(cls.futures_trading) +
                           " options=" + std::to_string(cls.options) +
                           " spot_options=" + std::to_string(cls.spot_options) +
                           " other=" + std::to_string(cls.other));

    std::vector<std::string> products;
    if (allow_product_filter && !cfg.dominant_products.empty()) {
      qsev::Logger::Warn("dominant", "DEBUG --allow-product-filter; using DOMINANT_PRODUCTS");
      products = cfg.dominant_products;
    } else if (!cfg.dominant_products.empty()) {
      qsev::Logger::Warn("dominant",
                         "DOMINANT_PRODUCTS ignored (plan/no-hardcode-ctp.md); using ALL futures");
    } else {
      qsev::Logger::Info("dominant", "product_filter=(all futures from InstrumentStore)");
    }

    const auto candidates = qsev::DominantSelector::CollectCandidates(store, products);
    qsev::Logger::Info("dominant", "futures candidates=" + std::to_string(candidates.size()));
    if (candidates.empty()) {
      qsev::Logger::Error("dominant", "no futures candidates");
      td.Stop();
      md.Stop();
      return 7;
    }

    if (!WaitMdLogin(md)) {
      qsev::Logger::Error("dominant", "Md not logged in");
      td.Stop();
      md.Stop();
      return 7;
    }

    std::string trading_day;
    size_t n_ticks = 0;
    const int wait_ms = std::max(kBatchSampleMs, run_seconds * 200);

    qsev::Logger::Info("dominant", "md subscribe futures n=" + std::to_string(candidates.size()) +
                                       " per_call<=" + std::to_string(qsev::kMdSubscribeBatch) +
                                       " workers=" + std::to_string(qsev::kMdSubscribeWorkers));
    if (!md.SubscribeBatched(candidates)) {
      qsev::Logger::Warn("dominant", "subscribe reported failures");
    }
    SampleBatch(bus, book, trading_day, n_ticks, candidates, wait_ms);
    const size_t covered = CountCovered(book, candidates);
    qsev::Logger::Info("dominant", "covered=" + std::to_string(covered) + "/" +
                                       std::to_string(candidates.size()) +
                                       " book=" + std::to_string(book.Size()) +
                                       " ticks=" + std::to_string(n_ticks));
    md.UnSubscribeBatched(candidates);
    SampleBatch(bus, book, trading_day, n_ticks, candidates, 500);

    qsev::Logger::Info("dominant", "sampled ticks=" + std::to_string(n_ticks) +
                                       " snapshots=" + std::to_string(book.Size()));

    const auto entries = qsev::DominantSelector::Select(store, book, products);
    size_t with_oi = 0;
    for (const auto& e : entries) {
      if (e.dominant_oi > 0) {
        ++with_oi;
        qsev::Logger::Info("dominant", e.product_key + " -> " + e.dominant + "(oi=" +
                                           std::to_string(static_cast<long long>(e.dominant_oi)) +
                                           ") / " + e.second + "(oi=" +
                                           std::to_string(static_cast<long long>(e.second_oi)) + ")");
      }
    }
    qsev::Logger::Info("dominant", "products=" + std::to_string(entries.size()) +
                                       " with_oi=" + std::to_string(with_oi));

    if (trading_day.empty()) trading_day = "unknown";
    const std::string out_path = cfg.data_root + "/live/dominant/" + trading_day + ".json";
    if (!qsev::DominantSelector::WriteJson(out_path, trading_day, entries)) {
      td.Stop();
      md.Stop();
      return 8;
    }

    std::vector<std::string> follow;
    for (const auto& e : entries) {
      if (e.dominant_oi <= 0) continue;
      if (!e.dominant.empty()) follow.push_back(e.dominant);
      if (!e.second.empty()) follow.push_back(e.second);
    }
    std::sort(follow.begin(), follow.end());
    follow.erase(std::unique(follow.begin(), follow.end()), follow.end());
    qsev::SubscriptionManager subs(md);
    subs.Sync(follow);
    qsev::Logger::Info("dominant", "follow subscribe n=" + std::to_string(follow.size()));
  }

  if (!select_dominant) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds);
    size_t n_ticks = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (auto t = bus.TryPop()) {
        ++n_ticks;
        book.Update(*t);
        if (n_ticks <= 50 || n_ticks % 100 == 0) {
          qsev::Logger::Info("tick", FormatTick(*t));
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    qsev::Logger::Info("engine", "drained ticks=" + std::to_string(n_ticks) +
                                     " store_size=" + std::to_string(store.Size()));
  }

  bus.Close();
  td.Stop();
  md.Stop();
  qsev::Logger::Info("engine", "done");
  return 0;
}
