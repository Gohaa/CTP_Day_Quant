#include "oms/oms.h"

#include "logging/logger.h"
#include "oms/exchange_rules.h"
#include "store/instrument_store.h"
#include "td/td_gateway.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qsev {
namespace fs = std::filesystem;

namespace {

std::string PadRef(int n) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%012d", n);
  return buf;
}

int ParseRef(const std::string& s) {
  int n = 0;
  for (char c : s) {
    if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
  }
  return n;
}

}  // namespace

Oms::Oms(std::string data_root, TdGateway* td, InstrumentStore* store)
    : data_root_(std::move(data_root)), td_(td), store_(store) {
  LoadRef();
}

void Oms::SetLimits(OmsLimits lim) { lim_ = std::move(lim); }

void Oms::SeedOrderRef(int login_max_ref, const std::string& trading_day) {
  if (login_max_ref > max_ref_) max_ref_ = login_max_ref;
  if (!trading_day.empty()) trading_day_ = trading_day;
  PersistRef();
}

std::string Oms::NextOrderRef() {
  ++max_ref_;
  PersistRef();
  return PadRef(max_ref_);
}

std::string Oms::RefPath() const {
  return (fs::path(data_root_) / "live" / "oms" / "max_order_ref.json").string();
}

void Oms::LoadRef() {
  std::ifstream in(RefPath());
  if (!in) return;
  std::ostringstream oss;
  oss << in.rdbuf();
  const std::string t = oss.str();
  auto grab = [&](const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    auto k = t.find(pat);
    if (k == std::string::npos) return std::string();
    auto c = t.find(':', k);
    if (c == std::string::npos) return std::string();
    auto i = c + 1;
    while (i < t.size() && (t[i] == ' ' || t[i] == '"')) ++i;
    auto j = i;
    while (j < t.size() && t[j] != '"' && t[j] != ',' && t[j] != '}' && t[j] != '\n') ++j;
    return t.substr(i, j - i);
  };
  max_ref_ = ParseRef(grab("max"));
  trading_day_ = grab("trading_day");
}

void Oms::PersistRef() {
  std::error_code ec;
  fs::create_directories(fs::path(data_root_) / "live" / "oms", ec);
  std::ofstream out(RefPath(), std::ios::binary);
  if (!out) return;
  out << "{\n  \"max\":" << max_ref_ << ",\n  \"trading_day\":\"" << trading_day_ << "\"\n}\n";
}

bool Oms::Whitelisted(const std::string& instrument_id) const {
  if (lim_.whitelist.empty()) return false;
  for (const auto& w : lim_.whitelist) {
    if (w == instrument_id) return true;
  }
  if (!store_) return false;
  const auto* info = store_->Find(instrument_id);
  if (!info) return false;
  const std::string pk = InstrumentStore::ProductKey(info->exchange_id, info->product_id);
  if (pk.empty()) return false;
  for (const auto& w : lim_.whitelist) {
    if (w == pk) return true;
  }
  return false;
}

bool Oms::Compose(OrderDraft* d, std::string* err) const {
  if (!d) {
    if (err) *err = "null draft";
    return false;
  }
  if (d->instrument_id.empty() || d->exchange_id.empty()) {
    if (err) *err = "instrument/exchange required";
    return false;
  }
  if (d->volume <= 0) {
    if (err) *err = "volume<=0";
    return false;
  }
  if (!(d->price > 0)) {
    if (err) *err = "price<=0";
    return false;
  }
  if (store_) {
    if (const auto* info = store_->Find(d->instrument_id)) {
      if (d->exchange_id.empty()) d->exchange_id = info->exchange_id;
      if (info->min_limit_order_volume > 0 && d->volume < info->min_limit_order_volume) {
        if (err) *err = "below min_limit_order_volume";
        return false;
      }
    }
  }
  return true;
}

bool Oms::Submit(OrderDraft& d, std::string* err) {
  last_submit_sent_ = false;
  std::string e;
  if (!Compose(&d, &e)) {
    if (err) *err = e;
    return false;
  }
  if (d.volume > lim_.max_volume) {
    if (err) *err = "max_volume";
    return false;
  }
  const bool is_close = IsCloseOffset(d.offset);
  // Flatten / close must never be blocked by the open-order cap.
  if (!is_close && daily_orders_ >= lim_.max_daily_orders) {
    if (err) *err = "max_daily_orders";
    return false;
  }
  if (d.order_ref.empty()) d.order_ref = NextOrderRef();

  Logger::Info("oms", std::string(lim_.dry_run ? "DRY " : "") + "insert " + d.instrument_id + " " +
                          d.exchange_id + " dir=" + std::string(1, d.direction) +
                          " offset=" + OffsetName(d.offset) + " px=" + std::to_string(d.price) +
                          " vol=" + std::to_string(d.volume) + " ref=" + d.order_ref);

  if (lim_.dry_run) {
    if (!is_close) ++daily_orders_;
    return true;
  }
  if (!lim_.enable_trading) {
    if (err) *err = "enable_trading=false";
    Logger::Warn("oms", "submit blocked enable_trading=false");
    return false;
  }
  if (!Whitelisted(d.instrument_id)) {
    if (err) *err = "not in trade whitelist";
    return false;
  }
  if (!td_) {
    if (err) *err = "no TdGateway";
    return false;
  }
  const int rc = td_->ReqOrderInsert(d);
  if (rc != 0) {
    if (err) *err = "ReqOrderInsert rc=" + std::to_string(rc);
    return false;
  }
  last_submit_sent_ = true;
  if (!is_close) ++daily_orders_;
  return true;
}

void Oms::OnOrder(const OrderRtn& r) {
  std::lock_guard<std::mutex> lock(mu_);
  orders_.push_back(r);
  Logger::Info("oms", "RtnOrder ref=" + r.order_ref + " st=" + std::string(1, r.status) +
                          " traded=" + std::to_string(r.volume_traded) +
                          " err=" + std::to_string(r.err_id) + " " + r.err_msg);
}

void Oms::OnTrade(const TradeRtn& t) {
  std::lock_guard<std::mutex> lock(mu_);
  trades_.push_back(t);
  Logger::Info("oms", "RtnTrade ref=" + t.order_ref + " px=" + std::to_string(t.price) +
                          " vol=" + std::to_string(t.volume));
}

void Oms::OnAccount(const AccountSnapshot& a) {
  std::lock_guard<std::mutex> lock(mu_);
  account_ = a;
}

void Oms::OnPositions(const std::vector<PositionRow>& rows) {
  std::lock_guard<std::mutex> lock(mu_);
  positions_ = rows;
}

void Oms::OnDepth(const DepthQuote& q) {
  std::lock_guard<std::mutex> lock(mu_);
  depth_ = q;
}

AccountSnapshot Oms::Account() const {
  std::lock_guard<std::mutex> lock(mu_);
  return account_;
}

std::vector<PositionRow> Oms::Positions() const {
  std::lock_guard<std::mutex> lock(mu_);
  return positions_;
}

DepthQuote Oms::Depth() const {
  std::lock_guard<std::mutex> lock(mu_);
  return depth_;
}

int Oms::TradedOf(const std::string& order_ref) const {
  std::lock_guard<std::mutex> lock(mu_);
  int n = 0;
  for (const auto& t : trades_) {
    if (t.order_ref == order_ref) n += t.volume;
  }
  return n;
}

double Oms::FillPrice(const std::string& order_ref) const {
  std::lock_guard<std::mutex> lock(mu_);
  double notional = 0;
  int n = 0;
  for (const auto& t : trades_) {
    if (t.order_ref != order_ref) continue;
    notional += t.price * t.volume;
    n += t.volume;
  }
  return n > 0 ? notional / n : 0;
}

int RunTradeSelfTest(const std::string& data_root) {
  int fail = 0;
  auto check = [&](bool c, const char* m) {
    if (!c) {
      Logger::Error("oms", std::string("selftest fail: ") + m);
      ++fail;
    }
  };
  check(ExchangeUsesCloseToday("SHFE") && OffsetClose("SHFE", true) == '3', "shfe today");
  check(OffsetClose("SHFE", false) == '1', "shfe yd");
  check(OffsetClose("INE", true) == '3', "ine");
  check(OffsetClose("DCE", true) == '1', "dce no closetoday");
  check(OffsetClose("CZCE", true) == '1', "czce");
  check(OffsetClose("CFFEX", true) == '1', "cffex");
  check(OffsetOpen() == '0', "open");

  constexpr const char* kTestId = "_TEST.rb";

  Oms oms(data_root, nullptr, nullptr);
  OmsLimits lim;
  lim.dry_run = true;
  lim.enable_trading = false;
  lim.max_volume = 1;
  lim.max_daily_orders = 2;
  lim.whitelist = {kTestId};
  oms.SetLimits(lim);
  oms.SeedOrderRef(7, "20260825");
  const auto r1 = oms.NextOrderRef();
  const auto r2 = oms.NextOrderRef();
  check(r1 != r2 && r2.size() == 12, "order ref");

  OrderDraft d;
  d.instrument_id = kTestId;
  d.exchange_id = "SHFE";
  d.direction = '0';
  d.offset = OffsetOpen();
  d.price = 3077;
  d.volume = 1;
  std::string err;
  check(oms.Submit(d, &err), "dry submit");
  check(!oms.LastSubmitSent(), "dry no send");

  d.volume = 2;
  check(!oms.Submit(d, &err) && err == "max_volume", "max vol");
  d.volume = 1;
  lim.dry_run = false;
  lim.enable_trading = false;
  oms.SetLimits(lim);
  check(!oms.Submit(d, &err) && err == "enable_trading=false", "gate off");
  lim.enable_trading = true;
  lim.whitelist.clear();
  oms.SetLimits(lim);
  check(!oms.Submit(d, &err) && err == "not in trade whitelist", "whitelist");

  {
    Oms oms2(data_root, nullptr, nullptr);
    OmsLimits lim2;
    lim2.dry_run = true;
    lim2.max_volume = 1;
    lim2.max_daily_orders = 1;
    lim2.whitelist = {kTestId};
    oms2.SetLimits(lim2);
    OrderDraft open = d;
    open.offset = OffsetOpen();
    open.order_ref.clear();
    check(oms2.Submit(open, &err), "cap open 1");
    open.order_ref.clear();
    check(!oms2.Submit(open, &err) && err == "max_daily_orders", "cap blocks open");
    OrderDraft cl = d;
    cl.direction = '1';
    cl.offset = OffsetClose("SHFE", true);
    cl.order_ref.clear();
    check(oms2.Submit(cl, &err), "close bypasses cap");
    check(oms2.DailyOrderCount() == 1, "close not counted");
  }

  Logger::Info("oms", fail == 0 ? "selftest ok" : "selftest failed");
  return fail == 0 ? 0 : 11;
}

}  // namespace qsev
