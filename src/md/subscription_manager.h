#pragma once

#include "md/md_gateway.h"

#include <string>
#include <vector>

namespace qsev {

// Keep Md active set equal to desired: UnSubscribe removed, Subscribe added (batched).
class SubscriptionManager {
 public:
  explicit SubscriptionManager(MdGateway& md);

  const std::vector<std::string>& Current() const { return current_; }

  // Returns false if any CTP call failed (partial apply may still have occurred).
  bool Sync(const std::vector<std::string>& desired, int batch_size = kMdSubscribeBatch,
            int delay_ms = 0);

 private:
  MdGateway& md_;
  std::vector<std::string> current_;
};

}  // namespace qsev
