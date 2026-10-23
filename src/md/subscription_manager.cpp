#include "md/subscription_manager.h"

#include "logging/logger.h"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace qsev {

SubscriptionManager::SubscriptionManager(MdGateway& md) : md_(md) {}

bool SubscriptionManager::Sync(const std::vector<std::string>& desired_in, int batch_size,
                               int delay_ms) {
  std::vector<std::string> desired = desired_in;
  std::sort(desired.begin(), desired.end());
  desired.erase(std::unique(desired.begin(), desired.end()), desired.end());

  std::vector<std::string> to_drop;
  std::vector<std::string> to_add;
  std::set_difference(current_.begin(), current_.end(), desired.begin(), desired.end(),
                      std::back_inserter(to_drop));
  std::set_difference(desired.begin(), desired.end(), current_.begin(), current_.end(),
                      std::back_inserter(to_add));

  {
    std::ostringstream oss;
    oss << "sync drop=" << to_drop.size() << " add=" << to_add.size()
        << " -> active=" << desired.size();
    Logger::Info("md_sub", oss.str());
  }

  bool ok = true;
  if (!to_drop.empty()) {
    if (!md_.UnSubscribeBatched(to_drop, batch_size, delay_ms)) ok = false;
  }
  if (!to_add.empty()) {
    if (!md_.SubscribeBatched(to_add, batch_size, delay_ms)) ok = false;
  }
  current_ = std::move(desired);
  return ok;
}

}  // namespace qsev
