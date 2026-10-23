#include "risk/risk_guard.h"

#include "logging/logger.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace qsev {
namespace fs = std::filesystem;

RiskGuard::RiskGuard(std::string data_root) : data_root_(std::move(data_root)) {}

std::string RiskGuard::PathFor(const std::string& instrument_id) const {
  return (fs::path(data_root_) / "live" / "guards" / (instrument_id + ".json")).string();
}

void RiskGuard::Arm(const State& st) {
  st_ = st;
  st_.armed = true;
  if (st_.peak <= 0) st_.peak = st_.entry;
  if (!(st_.price_tick > 0)) st_.price_tick = 1;
  if (!(st_.initial_risk > 0) && st_.stop > 0 && st_.entry > 0) {
    st_.initial_risk = std::fabs(st_.entry - st_.stop);
  }
  Save();
  Logger::Info("risk", "armed " + st_.instrument_id + " vol=" + std::to_string(st_.volume) +
                           " entry=" + std::to_string(st_.entry));
}

void RiskGuard::Disarm() {
  st_ = State{};
  Logger::Info("risk", "disarmed");
}

bool RiskGuard::OnTick(double last, int64_t now_unix, int64_t session_close_unix, bool persist) {
  if (!st_.armed || st_.volume <= 0 || !(last > 0)) return false;
  const bool is_long = st_.side == '0';
  if (is_long) {
    if (last > st_.peak) st_.peak = last;
  } else {
    if (st_.peak <= 0 || last < st_.peak) st_.peak = last;
  }

  const double risk = st_.initial_risk;
  const double tick = st_.price_tick > 0 ? st_.price_tick : 1.0;
  if (risk > 0) {
    if (is_long) {
      const double profit_r = (st_.peak - st_.entry) / risk;
      double trail_dist = -1;
      double floor = st_.stop;
      if (profit_r >= cfg_.arm_2) {
        trail_dist = 0.5 * risk;
        floor = st_.entry + 1.5 * risk;
      } else if (profit_r >= cfg_.arm_1) {
        trail_dist = 0.7 * risk;
        floor = st_.entry + 0.3 * risk;
      } else if (profit_r >= cfg_.arm_0_6) {
        trail_dist = 1.0 * risk;
        floor = st_.entry + tick;
      }
      if (trail_dist > 0 && st_.take_profit > st_.entry) {
        const double zone = st_.take_profit - st_.entry;
        const double dist_ratio = (st_.take_profit - last) / zone;
        if (dist_ratio >= 0 && dist_ratio <= 0.15) trail_dist = std::min(trail_dist, 0.15 * risk);
        else if (dist_ratio <= 0.35) trail_dist = std::min(trail_dist, 0.3 * risk);
      }
      if (trail_dist > 0) {
        double ns = st_.peak - trail_dist;
        if (ns < floor) ns = floor;
        if (ns > st_.stop) st_.stop = ns;
      }
    } else {
      const double profit_r = (st_.entry - st_.peak) / risk;
      double trail_dist = -1;
      double floor = st_.stop;
      if (profit_r >= cfg_.arm_2) {
        trail_dist = 0.5 * risk;
        floor = st_.entry - 1.5 * risk;
      } else if (profit_r >= cfg_.arm_1) {
        trail_dist = 0.7 * risk;
        floor = st_.entry - 0.3 * risk;
      } else if (profit_r >= cfg_.arm_0_6) {
        trail_dist = 1.0 * risk;
        floor = st_.entry - tick;
      }
      if (trail_dist > 0 && st_.entry > st_.take_profit && st_.take_profit > 0) {
        const double zone = st_.entry - st_.take_profit;
        const double dist_ratio = (last - st_.take_profit) / zone;
        if (dist_ratio >= 0 && dist_ratio <= 0.15) trail_dist = std::min(trail_dist, 0.15 * risk);
        else if (dist_ratio <= 0.35) trail_dist = std::min(trail_dist, 0.3 * risk);
      }
      if (trail_dist > 0) {
        double ns = st_.peak + trail_dist;
        if (ns > floor) ns = floor;
        if (ns < st_.stop) st_.stop = ns;
      }
    }
  }

  if (st_.take_profit > 0) {
    if (is_long && last >= st_.take_profit) {
      st_.reason = "take_profit";
      if (persist) Save();
      return true;
    }
    if (!is_long && last <= st_.take_profit) {
      st_.reason = "take_profit";
      if (persist) Save();
      return true;
    }
  }
  if (st_.stop > 0) {
    if (is_long && last <= st_.stop) {
      st_.reason = "stop";
      if (persist) Save();
      return true;
    }
    if (!is_long && last >= st_.stop) {
      st_.reason = "stop";
      if (persist) Save();
      return true;
    }
  }
  if (session_close_unix > 0 && now_unix > 0) {
    const int64_t remain = session_close_unix - now_unix;
    if (remain <= cfg_.eod_minutes * 60 && remain >= -60) {
      st_.reason = "eod";
      if (persist) Save();
      return true;
    }
  }
  if (persist) Save();
  return false;
}

bool RiskGuard::Save() const {
  if (st_.instrument_id.empty()) return false;
  std::error_code ec;
  fs::create_directories(fs::path(data_root_) / "live" / "guards", ec);
  std::ofstream out(PathFor(st_.instrument_id), std::ios::binary);
  if (!out) return false;
  out << "{\n"
      << "  \"instrument_id\":\"" << st_.instrument_id << "\",\n"
      << "  \"product_key\":\"" << st_.product_key << "\",\n"
      << "  \"side\":\"" << st_.side << "\",\n"
      << "  \"volume\":" << st_.volume << ",\n"
      << "  \"entry\":" << st_.entry << ",\n"
      << "  \"peak\":" << st_.peak << ",\n"
      << "  \"stop\":" << st_.stop << ",\n"
      << "  \"take_profit\":" << st_.take_profit << ",\n"
      << "  \"initial_risk\":" << st_.initial_risk << ",\n"
      << "  \"price_tick\":" << st_.price_tick << ",\n"
      << "  \"armed\":" << (st_.armed ? "true" : "false") << ",\n"
      << "  \"reason\":\"" << st_.reason << "\",\n"
      << "  \"flatten_whole\":" << (cfg_.flatten_whole ? "true" : "false") << "\n"
      << "}\n";
  return true;
}

bool RiskGuard::Load(const std::string& instrument_id) {
  std::ifstream in(PathFor(instrument_id));
  if (!in) return false;
  std::ostringstream oss;
  oss << in.rdbuf();
  const std::string t = oss.str();
  auto num = [&](const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    auto k = t.find(pat);
    if (k == std::string::npos) return 0.0;
    auto c = t.find(':', k);
    return std::atof(t.c_str() + c + 1);
  };
  auto str = [&](const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    auto k = t.find(pat);
    if (k == std::string::npos) return std::string();
    auto q1 = t.find('"', t.find(':', k) + 1);
    auto q2 = t.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) return std::string();
    return t.substr(q1 + 1, q2 - q1 - 1);
  };
  st_.instrument_id = str("instrument_id");
  st_.product_key = str("product_key");
  const auto side = str("side");
  st_.side = side.empty() ? '0' : side[0];
  st_.volume = static_cast<int>(num("volume"));
  st_.entry = num("entry");
  st_.peak = num("peak");
  st_.stop = num("stop");
  st_.take_profit = num("take_profit");
  st_.initial_risk = num("initial_risk");
  st_.price_tick = num("price_tick");
  if (!(st_.price_tick > 0)) st_.price_tick = 1;
  st_.armed = t.find("\"armed\":true") != std::string::npos;
  st_.reason = str("reason");
  return st_.armed;
}

int RunRiskSelfTest(const std::string& data_root) {
  int fail = 0;
  auto check = [&](bool c, const char* m) {
    if (!c) {
      Logger::Error("risk", std::string("selftest fail: ") + m);
      ++fail;
    }
  };
  RiskGuard g(data_root);
  RiskGuard::State st;
  st.instrument_id = "_TEST.rb";
  st.side = '0';
  st.volume = 1;
  st.entry = 100;
  st.peak = 100;
  st.stop = 90;
  st.take_profit = 200;
  g.Arm(st);
  check(!g.OnTick(100, 1000, 100000), "no fire at entry");
  check(!g.OnTick(106, 1000, 100000), "trail not fire");
  check(g.Get().stop >= 100, "trail to breakeven");
  g.Arm(st);
  check(!g.OnTick(110, 1000, 100000), "no fire at +10");
  st.take_profit = 120;
  g.Arm(st);
  check(g.OnTick(120, 1000, 100000), "tp");
  check(g.Get().reason == "take_profit", "tp reason");
  g.Arm(st);
  check(g.OnTick(90, 1000, 100000), "stop");
  g.Arm(st);
  check(g.OnTick(100, 99940, 100000), "eod 1min");
  RiskGuard g2(data_root);
  check(g2.Load("_TEST.rb"), "reload");
  check(g2.Get().entry == 100, "persist entry");
  Logger::Info("risk", fail == 0 ? "selftest ok" : "selftest failed");
  return fail == 0 ? 0 : 11;
}

}  // namespace qsev
