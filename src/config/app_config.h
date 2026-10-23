#pragma once

#include <string>
#include <vector>

namespace qsev {

struct AppConfig {
  std::string md_front;
  std::string td_front;
  std::string broker_id;
  std::string user_id;
  std::string password;
  std::string app_id;
  std::string auth_code;
  std::string tq_username;
  std::string tq_password;
  std::string data_root{"./data"};
  // Base dir for CTP .con flow files. Always resolved to absolute .../Quant_Sev/flow/
  // with trailing separator. Md/Td use subdirs md/ and td/ (never share prefix).
  std::string flow_path{"./flow"};
  std::string md_flow_path;  // filled by FinalizePaths → flow/{broker}_{user}/md/
  std::string td_flow_path;  // filled by FinalizePaths → flow/{broker}_{user}/td/
  // Trading sessions: openctp /times (NOT /instruments — no TimeBegin/TimeEnd there).
  std::string sessions_url{"http://dict.openctp.cn/times"};
  std::string dominant_file;  // optional path; else latest under data_root/live/dominant
  // DEBUG ONLY. Must stay empty in live.env. Live exe / --select-dominant / --live-ticks clear this.
  std::vector<std::string> md_instruments;
  // DEBUG ONLY. Must stay empty in live.env. Ignored unless --allow-product-filter.
  std::vector<std::string> dominant_products;
  int tick_ring_size{256};
  double tick_stale_sec{15.0};
  bool enable_trading{false};
  bool production_mode{true};
  int web_port{8765};

  static AppConfig LoadFromEnvFile(const std::string& path);
  static AppConfig LoadFromEnvironment();

  // Resolve DATA_ROOT / FLOW_PATH against Quant_Sev root; create data + flow/md + flow/td.
  // Call after LoadFromEnvFile (pass the same config path) or with empty for CWD-based resolve.
  void FinalizePaths(const std::string& config_file = {});
};

}  // namespace qsev
