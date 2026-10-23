#include "logging/logger.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>

namespace qsev {
namespace {

std::mutex g_log_mu;
std::ofstream g_log_file;
bool g_log_file_tried = false;

void MaybeOpenLogFile() {
  if (g_log_file_tried) return;
  g_log_file_tried = true;
  std::error_code ec;
  const auto dir = std::filesystem::current_path() / "data" / "live" / "logs";
  std::filesystem::create_directories(dir, ec);
  g_log_file.open((dir / "live.log").string(), std::ios::app);
}

std::string NowLocal() {
  using clock = std::chrono::system_clock;
  const auto t = clock::to_time_t(clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

void Write(const char* level, const std::string& channel, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_log_mu);
  MaybeOpenLogFile();
  const std::string line = NowLocal() + " [" + level + "][" + channel + "] " + msg;
  std::cerr << line << std::endl;
  if (g_log_file) {
    g_log_file << line << '\n';
    g_log_file.flush();
  }
}

}  // namespace

void Logger::Info(const std::string& channel, const std::string& msg) { Write("INFO", channel, msg); }
void Logger::Warn(const std::string& channel, const std::string& msg) { Write("WARN", channel, msg); }
void Logger::Error(const std::string& channel, const std::string& msg) { Write("ERROR", channel, msg); }

}  // namespace qsev
