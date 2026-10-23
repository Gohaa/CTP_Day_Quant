#include "config/app_config.h"

#include "logging/logger.h"
#include "util/path_util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace qsev {
namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
  }
  return s.substr(i);
}

std::string StripSpaces(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](unsigned char c) { return c == ' ' || c == '\t'; }),
          s.end());
  return s;
}

std::vector<std::string> SplitCsv(const std::string& val) {
  std::vector<std::string> out;
  std::stringstream ss(val);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = Trim(item);
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

void ApplyKv(AppConfig& cfg, const std::string& key, const std::string& val) {
  if (key == "MD_FRONT") cfg.md_front = StripSpaces(val);
  else if (key == "TD_FRONT") cfg.td_front = StripSpaces(val);
  else if (key == "BROKER_ID") cfg.broker_id = val;
  else if (key == "USER_ID") cfg.user_id = val;
  else if (key == "PASSWORD") cfg.password = val;
  else if (key == "APP_ID") cfg.app_id = val;
  else if (key == "AUTH_CODE") cfg.auth_code = val;
  else if (key == "TQ_USERNAME") cfg.tq_username = val;
  else if (key == "TQ_PASSWORD") cfg.tq_password = val;
  else if (key == "DATA_ROOT") cfg.data_root = val;
  else if (key == "FLOW_PATH") cfg.flow_path = val;
  else if (key == "SESSIONS_URL") cfg.sessions_url = val;
  else if (key == "MD_INSTRUMENTS") cfg.md_instruments = SplitCsv(val);
  else if (key == "DOMINANT_PRODUCTS") cfg.dominant_products = SplitCsv(val);
  else if (key == "DOMINANT_FILE") cfg.dominant_file = val;
  else if (key == "TICK_RING_SIZE") cfg.tick_ring_size = std::atoi(val.c_str());
  else if (key == "TICK_STALE_SEC") cfg.tick_stale_sec = std::atof(val.c_str());
  else if (key == "ENABLE_TRADING") {
    cfg.enable_trading = (val == "1" || val == "true" || val == "TRUE" || val == "yes");
  } else if (key == "PRODUCTION_MODE") {
    cfg.production_mode = !(val == "0" || val == "false" || val == "FALSE");
  } else if (key == "WEB_PORT") {
    const int p = std::atoi(val.c_str());
    if (p > 0 && p < 65536) cfg.web_port = p;
  }
}

}  // namespace

AppConfig AppConfig::LoadFromEnvFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open config file: " + path);
  }
  AppConfig cfg = LoadFromEnvironment();
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    const auto pos = line.find('=');
    if (pos == std::string::npos) continue;
    ApplyKv(cfg, Trim(line.substr(0, pos)), Trim(line.substr(pos + 1)));
  }
  cfg.FinalizePaths(path);
  return cfg;
}

AppConfig AppConfig::LoadFromEnvironment() {
  AppConfig cfg;
  auto get = [](const char* k) -> std::string {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
  };
  if (auto v = get("MD_FRONT"); !v.empty()) cfg.md_front = StripSpaces(v);
  if (auto v = get("TD_FRONT"); !v.empty()) cfg.td_front = StripSpaces(v);
  if (auto v = get("BROKER_ID"); !v.empty()) cfg.broker_id = v;
  if (auto v = get("USER_ID"); !v.empty()) cfg.user_id = v;
  if (auto v = get("PASSWORD"); !v.empty()) cfg.password = v;
  if (auto v = get("APP_ID"); !v.empty()) cfg.app_id = v;
  if (auto v = get("AUTH_CODE"); !v.empty()) cfg.auth_code = v;
  if (auto v = get("TQ_USERNAME"); !v.empty()) cfg.tq_username = v;
  if (auto v = get("TQ_PASSWORD"); !v.empty()) cfg.tq_password = v;
  if (auto v = get("DATA_ROOT"); !v.empty()) cfg.data_root = v;
  if (auto v = get("FLOW_PATH"); !v.empty()) cfg.flow_path = v;
  if (auto v = get("SESSIONS_URL"); !v.empty()) cfg.sessions_url = v;
  if (auto v = get("MD_INSTRUMENTS"); !v.empty()) cfg.md_instruments = SplitCsv(v);
  if (auto v = get("DOMINANT_PRODUCTS"); !v.empty()) cfg.dominant_products = SplitCsv(v);
  if (auto v = get("DOMINANT_FILE"); !v.empty()) cfg.dominant_file = v;
  if (auto v = get("TICK_RING_SIZE"); !v.empty()) cfg.tick_ring_size = std::atoi(v.c_str());
  if (auto v = get("TICK_STALE_SEC"); !v.empty()) cfg.tick_stale_sec = std::atof(v.c_str());
  if (auto v = get("ENABLE_TRADING"); !v.empty()) {
    cfg.enable_trading = (v == "1" || v == "true" || v == "TRUE");
  }
  if (auto v = get("WEB_PORT"); !v.empty()) {
    const int p = std::atoi(v.c_str());
    if (p > 0 && p < 65536) cfg.web_port = p;
  }
  return cfg;
}

void AppConfig::FinalizePaths(const std::string& config_file) {
  namespace fs = std::filesystem;
  std::string root;
  if (!config_file.empty()) {
    root = ProjectRootFromConfig(config_file);
  } else {
    root = fs::current_path().string();
  }

  data_root = ResolvePath(data_root.empty() ? "./data" : data_root, root);
  try {
    fs::create_directories(fs::path(data_root) / "live" / "dominant");
    fs::create_directories(fs::path(data_root) / "live" / "instruments" / "dominant");
    fs::create_directories(fs::path(data_root) / "live" / "logs");
    fs::create_directories(fs::path(data_root) / "live" / "sessions");
    fs::create_directories(fs::path(data_root) / "live" / "option_screen");
    fs::create_directories(fs::path(data_root) / "live" / "oms");
    fs::create_directories(fs::path(data_root) / "live" / "guards");
    fs::create_directories(fs::path(data_root) / "klines");
    fs::create_directories(fs::path(data_root) / "ticks");
  } catch (const std::exception& e) {
    Logger::Warn("config", std::string("data mkdir: ") + e.what());
  }

  flow_path = ResolvePath(flow_path.empty() ? "./flow" : flow_path, root);
  flow_path = PrepareCtpFlowDir(flow_path);
  const std::string tag = CtpAccountFlowTag(broker_id, user_id);
  flow_path = PrepareCtpFlowDir((fs::path(flow_path) / tag).string());
  md_flow_path = PrepareCtpFlowDir((fs::path(flow_path) / "md").string());
  td_flow_path = PrepareCtpFlowDir((fs::path(flow_path) / "td").string());

  Logger::Info("config", "project_root=" + root);
  Logger::Info("config", "data_root=" + data_root);
  Logger::Info("config", "flow_path=" + flow_path);
  Logger::Info("config", "md_flow=" + md_flow_path);
  Logger::Info("config", "td_flow=" + td_flow_path);
  Logger::Info("config", "account_tag=" + tag + " web_port=" + std::to_string(web_port));
  Logger::Info("config", "ctp user=" + user_id + " password_len=" + std::to_string(password.size()));
}

}  // namespace qsev
