#pragma once

#include <string>

namespace qsev {

class Logger {
 public:
  static void Info(const std::string& channel, const std::string& msg);
  static void Warn(const std::string& channel, const std::string& msg);
  static void Error(const std::string& channel, const std::string& msg);
};

}  // namespace qsev
