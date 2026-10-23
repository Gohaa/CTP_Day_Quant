#include "oms/oms.h"

#include "config/app_config.h"
#include "logging/logger.h"
#include "oms/exchange_rules.h"
#include "risk/risk_guard.h"
#include "store/dominant_table.h"
#include "store/instrument_store.h"
#include "strategy/ai_open/ai_open.h"
#include "strategy/engine.h"
#include "strategy/fixed/fixed.h"
#include "strategy/manual_open/manual_open.h"
#include "strategy/modules/modules.h"
#include "strategy/pyramid.h"
#include "td/td_gateway.h"
#include "util/mask.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace qsev {
namespace fs = std::filesystem;

namespace {

class OmsSink final : public TdTradeSink {
 public:
  explicit OmsSink(Oms* o) : o_(o) {}
  void OnOrder(const OrderRtn& r) override { o_->OnOrder(r); }
  void OnTrade(const TradeRtn& t) override { o_->OnTrade(t); }
  void OnAccount(const AccountSnapshot& a) override { o_->OnAccount(a); }
  void OnPositions(const std::vector<PositionRow>& rows) override { o_->OnPositions(rows); }
  void OnDepth(const DepthQuote& q) override { o_->OnDepth(q); }

 private:
  Oms* o_;
};

bool WaitMs(const std::function<bool()>& pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return pred();
}

std::string LatestInstrumentsJson(const std::string& data_root) {
  const fs::path dir = fs::path(data_root) / "live" / "instruments";
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  std::string best;
  for (const auto& ent : fs::directory_iterator(dir, ec)) {
    if (!ent.is_regular_file()) continue;
    if (ent.path().extension() != ".json") continue;
    const std::string n = ent.path().stem().string();
    if (n.size() == 8) {
      bool ok = true;
      for (char c : n) ok = ok && std::isdigit(static_cast<unsigned char>(c));
      if (ok && n > best) best = n;
    }
  }
  if (best.empty()) return {};
  return (dir / (best + ".json")).string();
}

bool ResolveProduct(const DominantTable& table, const InstrumentStore& store, const std::string& product_key,
                    std::string* instrument_id, std::string* exchange_id) {
  for (const auto& e : table.Entries()) {
    if (e.product_key != product_key) continue;
    if (e.dominant.empty()) return false;
    *instrument_id = e.dominant;
    if (const auto* info = store.Find(e.dominant)) {
      *exchange_id = info->exchange_id;
    } else if (product_key.size() > 0) {
      const auto dot = product_key.find('.');
      if (dot != std::string::npos) *exchange_id = product_key.substr(0, dot);
    }
    return !instrument_id->empty() && !exchange_id->empty();
  }
  return false;
}

int PositionLots(const std::vector<PositionRow>& rows, const std::string& instrument_id) {
  int n = 0;
  for (const auto& p : rows) {
    if (p.instrument_id == instrument_id) n += p.position;
  }
  return n;
}

}  // namespace

int RunOmsSession(const AppConfig& cfg, bool live_roundtrip, const std::string& product_key) {
  Logger::Info("oms", std::string("session start live_roundtrip=") + (live_roundtrip ? "true" : "false") +
                          " product=" + product_key);
  Logger::Info("oms", "user_id=" + cfg.user_id + " password=" + MaskSecret(cfg.password));
  if (live_roundtrip && !cfg.enable_trading) {
    Logger::Error("oms", "--oms-roundtrip needs --enable-trading (does not write live.env)");
    return 2;
  }

  InstrumentStore store;
  const auto inst_json = LatestInstrumentsJson(cfg.data_root);
  if (!inst_json.empty()) {
    store.LoadJson(inst_json);
    Logger::Info("oms", "loaded instruments " + inst_json + " size=" + std::to_string(store.Size()));
  }

  DominantTable table;
  try {
    table = DominantTable::LoadLatest(cfg.data_root);
  } catch (const std::exception& e) {
    Logger::Error("oms", e.what());
    return 6;
  }

  std::string instrument_id;
  std::string exchange_id;
  if (!ResolveProduct(table, store, product_key, &instrument_id, &exchange_id)) {
    Logger::Error("oms", "product not in dominant JSON: " + product_key);
    return 6;
  }
  Logger::Info("oms", "target " + product_key + " -> " + exchange_id + "." + instrument_id);

  TdGateway td(cfg, store);
  td.EnableEnrichment(false);
  td.EnableInstrumentQuery(false);
  Oms oms(cfg.data_root, &td, &store);
  OmsSink sink(&oms);
  td.SetTradeSink(&sink);

  OmsLimits lim;
  lim.dry_run = !live_roundtrip;
  lim.enable_trading = cfg.enable_trading && live_roundtrip;
  lim.max_volume = 1;
  lim.max_daily_orders = 4;
  lim.whitelist = {instrument_id};
  oms.SetLimits(lim);

  if (!td.Start()) return 5;
  if (!WaitMs([&] { return td.LoggedIn() && td.Settled(); }, 30000)) {
    Logger::Error("oms", "Td login/settle timeout");
    td.Stop();
    return 5;
  }
  oms.SeedOrderRef(td.LoginMaxOrderRef(), td.TradingDay());
  Logger::Info("oms", "td ready trading_day=" + td.TradingDay() +
                          " max_ref=" + std::to_string(td.LoginMaxOrderRef()));

  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  td.ReqQryTradingAccount();
  WaitMs([&] { return td.AccountQueryDone(); }, 10000);
  const auto acct = oms.Account();
  Logger::Info("oms", "account available=" + std::to_string(acct.available) +
                          " balance=" + std::to_string(acct.balance) +
                          " margin=" + std::to_string(acct.curr_margin) +
                          " frozen=" + std::to_string(acct.frozen_margin));

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ResetQueryFlags();
  td.ReqQryInvestorPosition();
  WaitMs([&] { return td.PositionQueryDone(); }, 15000);
  const auto pos0 = oms.Positions();
  Logger::Info("oms", "positions rows=" + std::to_string(pos0.size()));
  for (const auto& p : pos0) {
    if (p.instrument_id == instrument_id && p.position > 0) {
      Logger::Info("oms", "pos " + p.instrument_id + " dir=" + std::string(1, p.posi_direction) +
                              " date=" + std::string(1, p.position_date) +
                              " n=" + std::to_string(p.position));
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ReqQryDepth(instrument_id);
  WaitMs([&] { return td.DepthQueryDone() && oms.Depth().ok; }, 10000);
  auto q = oms.Depth();
  Logger::Info("oms", "depth last=" + std::to_string(q.last) + " bid=" + std::to_string(q.bid) +
                          " ask=" + std::to_string(q.ask));
  if (!q.ok) {
    Logger::Error("oms", "no depth quote");
    td.Stop();
    return 7;
  }
  const double px = q.ask > 0 ? q.ask : (q.last > 0 ? q.last : 0);
  if (!(px > 0)) {
    Logger::Error("oms", "no price");
    td.Stop();
    return 7;
  }

  OrderDraft open;
  open.instrument_id = instrument_id;
  open.exchange_id = exchange_id;
  open.product_key = product_key;
  open.direction = '0';
  open.offset = OffsetOpen();
  open.price = px;
  open.volume = 1;
  std::string err;
  if (!oms.Submit(open, &err)) {
    Logger::Error("oms", "open submit failed: " + err);
    td.Stop();
    return 8;
  }

  strategy::PyramidStrategy pyr;
  strategy::PyramidConfig pcfg;
  pcfg.v0 = 1;
  pcfg.max_layers = 2;
  pcfg.max_lots = 2;
  pcfg.delta = 10;
  pcfg.trigger = 10;
  pyr.Reset(strategy::Side::Long, px, pcfg);
  Logger::Info("pyramid", std::string("dry last=") + std::to_string(q.last) +
                              " add_if_now=" + std::to_string(pyr.OnPrice(q.last)));

  RiskGuard guard(cfg.data_root);
  RiskGuard::State gst;
  gst.instrument_id = instrument_id;
  gst.product_key = product_key;
  gst.side = '0';
  gst.volume = 1;
  gst.entry = px;
  gst.peak = px;
  gst.stop = px - 20;
  gst.take_profit = px + 40;
  guard.Arm(gst);
  const bool would = guard.OnTick(q.last, 0, 0);
  Logger::Info("risk", std::string("armed paper; would_flatten=") + (would ? "true" : "false"));
  if (!live_roundtrip) {
    Logger::Info("oms", "dry-run done (no Td send). gate-off test:");
    OmsLimits off = lim;
    off.dry_run = false;
    off.enable_trading = false;
    oms.SetLimits(off);
    std::string e2;
    const bool blocked = !oms.Submit(open, &e2);
    Logger::Info("oms", std::string("enable_trading=false blocked=") + (blocked ? "true " : "false ") + e2);
    guard.Disarm();
    td.Stop();
    return blocked ? 0 : 8;
  }

  if (!WaitMs([&] { return oms.TradedOf(open.order_ref) >= 1; }, 20000)) {
    Logger::Warn("oms", "open not filled, cancelling");
    td.ReqOrderAction(open.order_ref, exchange_id, instrument_id);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    guard.Disarm();
    td.Stop();
    return 9;
  }
  Logger::Info("oms", "open filled ref=" + open.order_ref);

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ReqQryDepth(instrument_id);
  WaitMs([&] { return oms.Depth().ok; }, 8000);
  q = oms.Depth();
  const double close_px = q.bid > 0 ? q.bid : (q.last > 0 ? q.last : px);

  OrderDraft cl;
  cl.instrument_id = instrument_id;
  cl.exchange_id = exchange_id;
  cl.direction = '1';
  cl.offset = OffsetClose(exchange_id, true);
  cl.price = close_px;
  cl.volume = 1;
  if (!oms.Submit(cl, &err)) {
    Logger::Error("oms", "close submit failed: " + err);
    td.Stop();
    return 8;
  }
  if (!WaitMs([&] { return oms.TradedOf(cl.order_ref) >= 1; }, 20000)) {
    Logger::Warn("oms", "close not filled at bid, retry lower");
    if (q.lower > 0) {
      cl.price = q.lower;
      cl.order_ref.clear();
      oms.Submit(cl, &err);
      WaitMs([&] { return oms.TradedOf(cl.order_ref) >= 1; }, 15000);
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ResetQueryFlags();
  td.ReqQryInvestorPosition();
  WaitMs([&] { return td.PositionQueryDone(); }, 10000);
  int left = 0;
  for (const auto& p : oms.Positions()) {
    if (p.instrument_id == instrument_id) left += p.position;
  }
  Logger::Info("oms", "roundtrip leftover position=" + std::to_string(left));
  guard.Disarm();
  td.Stop();
  return left == 0 ? 0 : 9;
}

int RunStrategyLiveVerify(const AppConfig& cfg, const std::string& product_key) {
  Logger::Info("strategy", "LIVE VERIFY product=" + product_key + " (1 lot, then guard flatten)");
  if (!cfg.enable_trading) {
    Logger::Error("strategy", "--strategy-live-verify needs --enable-trading (does not write live.env)");
    return 2;
  }

  strategy::Context ctx;
  strategy::OpenIntent rule;
  if (strategy::LoadContext(cfg.data_root, product_key, &ctx)) {
    const auto pack = strategy::ScoreModules(ctx);
    rule = strategy::RuleOpen(ctx);
    const auto ai = strategy::AiOpenIntent(ctx, cfg.data_root);
    const auto ind = strategy::IndicatorManual(ctx);
    Logger::Info("strategy", "scan rule ready=" + std::string(rule.ready ? "true" : "false") + " " +
                                 rule.reason);
    Logger::Info("strategy", "ai " + ai.reason + " (never send)");
    Logger::Info("strategy", "manual " + ind.reason + " (never send)");
    Logger::Info("strategy", "modules sum=" + std::to_string(pack.sum));
  } else {
    Logger::Warn("strategy", "quote context missing; verify still opens 1 lot from Td depth");
  }

  InstrumentStore store;
  const auto inst_json = LatestInstrumentsJson(cfg.data_root);
  if (!inst_json.empty()) store.LoadJson(inst_json);

  DominantTable table;
  try {
    table = DominantTable::LoadLatest(cfg.data_root);
  } catch (const std::exception& e) {
    Logger::Error("oms", e.what());
    return 6;
  }

  std::string instrument_id;
  std::string exchange_id;
  if (!ResolveProduct(table, store, product_key, &instrument_id, &exchange_id)) {
    Logger::Error("oms", "product not in dominant JSON: " + product_key);
    return 6;
  }
  ctx.instrument_id = instrument_id;
  ctx.exchange_id = exchange_id;
  ctx.product_key = product_key;
  double tick = 1;
  if (const auto* info = store.Find(instrument_id)) {
    if (info->price_tick > 0) tick = info->price_tick;
  }
  ctx.price_tick = tick;

  TdGateway td(cfg, store);
  td.EnableEnrichment(false);
  td.EnableInstrumentQuery(false);
  Oms oms(cfg.data_root, &td, &store);
  OmsSink sink(&oms);
  td.SetTradeSink(&sink);

  OmsLimits lim;
  lim.dry_run = false;
  lim.enable_trading = true;
  lim.max_volume = 2;
  lim.max_daily_orders = 8;
  lim.whitelist = {instrument_id};
  oms.SetLimits(lim);

  if (!td.Start()) return 5;
  if (!WaitMs([&] { return td.LoggedIn() && td.Settled(); }, 30000)) {
    Logger::Error("oms", "Td login/settle timeout");
    td.Stop();
    return 5;
  }
  oms.SeedOrderRef(td.LoginMaxOrderRef(), td.TradingDay());

  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  td.ReqQryTradingAccount();
  WaitMs([&] { return td.AccountQueryDone(); }, 10000);
  Logger::Info("oms", "account available=" + std::to_string(oms.Account().available));

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ResetQueryFlags();
  td.ReqQryInvestorPosition();
  WaitMs([&] { return td.PositionQueryDone(); }, 15000);
  if (PositionLots(oms.Positions(), instrument_id) > 0) {
    Logger::Error("oms", "existing position on " + instrument_id + " — flatten manually first");
    td.Stop();
    return 8;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ReqQryDepth(instrument_id);
  WaitMs([&] { return td.DepthQueryDone() && oms.Depth().ok; }, 10000);
  auto q = oms.Depth();
  if (!q.ok || !(q.last > 0 || q.ask > 0 || q.bid > 0)) {
    Logger::Error("oms", "no depth");
    td.Stop();
    return 7;
  }
  ctx.last = q.last > 0 ? q.last : (q.ask > 0 ? q.ask : q.bid);
  ctx.book.last = ctx.last;
  ctx.book.bid = q.bid;
  ctx.book.ask = q.ask;

  strategy::OpenIntent intent = rule;
  if (!intent.ready) {
    intent.ready = true;
    intent.side = strategy::Side::Long;
    intent.strategy_id = "live_verify_fallback";
    intent.reason = "rule not ready; verify fallback long 1 lot";
    Logger::Warn("strategy", intent.reason);
  }
  const auto ice = strategy::IcebergSlices(1, 1, 0);
  Logger::Info("strategy", "iceberg slices=" + std::to_string(ice.size()) + " (send 1 lot)");

  auto open = strategy::IntentToOpenDraft(ctx, intent, 1);
  if (!(open.price > 0)) open.price = q.ask > 0 ? q.ask : ctx.last;
  std::string err;
  if (!oms.Submit(open, &err)) {
    Logger::Error("oms", "open submit failed: " + err);
    td.Stop();
    return 8;
  }
  if (!WaitMs([&] { return oms.TradedOf(open.order_ref) >= 1; }, 20000)) {
    Logger::Warn("oms", "open not filled, cancelling");
    td.ReqOrderAction(open.order_ref, exchange_id, instrument_id);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    td.Stop();
    return 9;
  }
  const double fill = oms.FillPrice(open.order_ref);
  Logger::Info("oms", "open filled px=" + std::to_string(fill) + " ref=" + open.order_ref);

  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  td.ResetQueryFlags();
  td.ReqQryDepth(instrument_id);
  WaitMs([&] { return oms.Depth().ok; }, 8000);
  q = oms.Depth();
  const double last0 = q.last > 0 ? q.last : fill;

  const auto tpsl = strategy::FixedTpSl(fill, intent.side, 2, 8, tick);
  RiskGuard guard(cfg.data_root);
  RiskGuard::State gst;
  gst.instrument_id = instrument_id;
  gst.product_key = product_key;
  gst.side = intent.side == strategy::Side::Short ? '1' : '0';
  gst.volume = 1;
  gst.entry = fill;
  gst.peak = fill;
  gst.price_tick = tick;
  gst.initial_risk = 8 * tick;
  // TP at current last so the next quote proves flatten via take_profit.
  gst.take_profit = last0;
  gst.stop = tpsl.ok ? tpsl.stop_loss : (intent.side == strategy::Side::Short ? fill + 8 * tick : fill - 8 * tick);
  guard.Arm(gst);
  Logger::Info("risk", "armed tp=" + std::to_string(gst.take_profit) + " sl=" + std::to_string(gst.stop) +
                           " tick=" + std::to_string(tick));

  strategy::PyramidStrategy pyr;
  strategy::PyramidConfig pcfg;
  pcfg.v0 = 1;
  pcfg.max_layers = 2;
  pcfg.max_lots = 2;
  pcfg.trigger = 10 * tick;
  pcfg.delta = 10 * tick;
  pyr.Reset(intent.side, fill, pcfg);
  Logger::Info("pyramid", "live add_now=" + std::to_string(pyr.OnPrice(last0)) + " (no extra send unless >0)");

  std::string why;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(40);
  while (std::chrono::steady_clock::now() < until) {
    td.ResetQueryFlags();
    td.ReqQryDepth(instrument_id);
    WaitMs([&] { return td.DepthQueryDone(); }, 3000);
    q = oms.Depth();
    const double last = q.last > 0 ? q.last : last0;
    if (guard.OnTick(last, 0, 0, true)) {
      why = guard.Get().reason;
      Logger::Info("risk", "FIRE " + why + " last=" + std::to_string(last));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
  }
  if (why.empty()) {
    why = "timeout_flatten";
    Logger::Warn("risk", "guard did not fire in 40s, flattening anyway");
  }

  ctx.last = q.last > 0 ? q.last : last0;
  ctx.book.bid = q.bid;
  ctx.book.ask = q.ask;
  auto cl = strategy::IntentToCloseDraft(ctx, intent.side, 1, true);
  if (!(cl.price > 0)) {
    cl.price = intent.side == strategy::Side::Long ? (q.bid > 0 ? q.bid : ctx.last) : (q.ask > 0 ? q.ask : ctx.last);
  }
  if (!oms.Submit(cl, &err)) {
    Logger::Error("oms", "flatten submit failed: " + err);
    td.Stop();
    return 8;
  }
  if (!WaitMs([&] { return oms.TradedOf(cl.order_ref) >= 1; }, 20000)) {
    Logger::Warn("oms", "flatten not filled, retry limit");
    if (intent.side == strategy::Side::Long && q.lower > 0) cl.price = q.lower;
    if (intent.side == strategy::Side::Short && q.upper > 0) cl.price = q.upper;
    cl.order_ref.clear();
    oms.Submit(cl, &err);
    WaitMs([&] { return oms.TradedOf(cl.order_ref) >= 1; }, 15000);
  }
  Logger::Info("oms", "flatten filled px=" + std::to_string(oms.FillPrice(cl.order_ref)) + " why=" + why);

  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  td.ResetQueryFlags();
  td.ReqQryInvestorPosition();
  WaitMs([&] { return td.PositionQueryDone(); }, 10000);
  const int left = PositionLots(oms.Positions(), instrument_id);
  Logger::Info("oms", "live-verify leftover=" + std::to_string(left) + " why=" + why);
  guard.Disarm();
  pyr.Save(cfg.data_root, instrument_id);
  td.Stop();
  return left == 0 ? 0 : 9;
}

}  // namespace qsev
