#pragma once

#include "oms/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace qsev {

class RiskGuard {
 public:
  struct Config {
    double r_ticks{10};
    double arm_0_6{0.6};
    double arm_1{1.0};
    double arm_2{2.0};
    int eod_minutes{2};
    bool flatten_whole{true};  // vs per pyramid layer
  };

  struct State {
    std::string instrument_id;
    std::string product_key;
    char side{'0'};  // Buy=long '0'
    int volume{0};
    double entry{0};
    double peak{0};
    double stop{0};
    double take_profit{0};
    double initial_risk{0};
    double price_tick{1};
    bool armed{false};
    std::string reason;
  };

  explicit RiskGuard(std::string data_root);

  void SetConfig(Config c) { cfg_ = c; }
  void Arm(const State& st);
  void Disarm();
  State Get() const { return st_; }

  // Returns true if flatten should fire. Does not send.
  bool OnTick(double last, int64_t now_unix, int64_t session_close_unix, bool persist = true);

  bool Save() const;
  bool Load(const std::string& instrument_id);

 private:
  std::string data_root_;
  Config cfg_;
  State st_;
  std::string PathFor(const std::string& instrument_id) const;
};

int RunRiskSelfTest(const std::string& data_root);

}  // namespace qsev
