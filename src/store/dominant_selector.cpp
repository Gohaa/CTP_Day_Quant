#include "store/dominant_selector.h"



#include "logging/logger.h"

#include "util/ctp_product_class.h"



#include <algorithm>

#include <filesystem>

#include <fstream>

#include <unordered_map>



namespace qsev {

namespace {



bool ProductAllowed(const std::string& key, const std::vector<std::string>& filter) {

  if (filter.empty()) return true;  // empty = all futures products from InstrumentStore

  for (const auto& f : filter) {

    if (f == key) return true;

  }

  return false;

}



bool IsTradableFutures(const InstrumentInfo& info) {

  if (!IsFuturesClass(info.product_class)) return false;

  if (!info.is_trading) return false;

  // InstLifePhase 0 sometimes appears; Started is the normal trading phase.

  if (info.inst_life_phase != 0 && info.inst_life_phase != kLifeStarted) return false;

  return true;

}



struct RankRow {

  std::string instrument_id;

  double oi{0};

  int volume{0};

};



}  // namespace



InstrumentClassStats DominantSelector::CountClasses(const InstrumentStore& store) {

  InstrumentClassStats s;

  for (const auto& info : store.List()) {

    ++s.total;

    if (IsFuturesClass(info.product_class)) {

      ++s.futures;

      if (info.is_trading) ++s.futures_trading;

    } else if (info.product_class == kPcOptions) {

      ++s.options;

    } else if (info.product_class == kPcSpotOption) {

      ++s.spot_options;

    } else {

      ++s.other;

    }

  }

  return s;

}



std::vector<std::string> DominantSelector::CollectCandidates(

    const InstrumentStore& store, const std::vector<std::string>& product_filter) {

  std::vector<std::string> out;

  for (const auto& info : store.List()) {

    if (!IsTradableFutures(info)) continue;

    const std::string key = InstrumentStore::ProductKey(info.exchange_id, info.product_id);

    if (!ProductAllowed(key, product_filter)) continue;

    out.push_back(info.instrument_id);

  }

  std::sort(out.begin(), out.end());

  out.erase(std::unique(out.begin(), out.end()), out.end());

  return out;

}



std::vector<DominantEntry> DominantSelector::Select(const InstrumentStore& store,

                                                    const TickSnapshotBook& book,

                                                    const std::vector<std::string>& product_filter) {

  std::unordered_map<std::string, std::vector<RankRow>> groups;

  for (const auto& info : store.List()) {

    if (!IsTradableFutures(info)) continue;

    const std::string key = InstrumentStore::ProductKey(info.exchange_id, info.product_id);

    if (!ProductAllowed(key, product_filter)) continue;

    const MarketSnapshot snap = book.Get(info.instrument_id);

    RankRow row;

    row.instrument_id = info.instrument_id;

    row.oi = snap.open_interest;

    row.volume = snap.volume;

    groups[key].push_back(row);

  }



  std::vector<DominantEntry> result;

  result.reserve(groups.size());

  for (auto& kv : groups) {

    auto& rows = kv.second;

    std::sort(rows.begin(), rows.end(), [](const RankRow& a, const RankRow& b) {

      if (a.oi != b.oi) return a.oi > b.oi;

      if (a.volume != b.volume) return a.volume > b.volume;

      return a.instrument_id < b.instrument_id;

    });

    DominantEntry e;

    e.product_key = kv.first;

    if (!rows.empty()) {

      e.dominant = rows[0].instrument_id;

      e.dominant_oi = rows[0].oi;

      e.dominant_volume = rows[0].volume;

    }

    if (rows.size() >= 2) {

      e.second = rows[1].instrument_id;

      e.second_oi = rows[1].oi;

      e.second_volume = rows[1].volume;

    }

    result.push_back(e);

  }

  std::sort(result.begin(), result.end(),

            [](const DominantEntry& a, const DominantEntry& b) { return a.product_key < b.product_key; });

  return result;

}



bool DominantSelector::WriteJson(const std::string& path, const std::string& trading_day,

                                 const std::vector<DominantEntry>& entries) {

  namespace fs = std::filesystem;

  try {

    fs::path p(path);

    if (p.has_parent_path()) fs::create_directories(p.parent_path());

  } catch (const std::exception& e) {

    Logger::Error("dominant", std::string("mkdir failed: ") + e.what());

    return false;

  }



  std::ofstream out(path, std::ios::binary);

  if (!out) {

    Logger::Error("dominant", "cannot write " + path);

    return false;

  }

  out << "{\n";

  out << "  \"trading_day\": \"" << trading_day << "\",\n";

  out << "  \"products\": {\n";

  for (size_t i = 0; i < entries.size(); ++i) {

    const auto& e = entries[i];

    out << "    \"" << e.product_key << "\": {"

        << "\"dominant\": \"" << e.dominant << "\", "

        << "\"second\": \"" << e.second << "\", "

        << "\"dominant_oi\": " << e.dominant_oi << ", "

        << "\"second_oi\": " << e.second_oi << ", "

        << "\"dominant_volume\": " << e.dominant_volume << ", "

        << "\"second_volume\": " << e.second_volume << "}";

    if (i + 1 < entries.size()) out << ",";

    out << "\n";

  }

  out << "  }\n";

  out << "}\n";

  Logger::Info("dominant", "wrote " + path + " products=" + std::to_string(entries.size()));

  return true;

}



}  // namespace qsev


