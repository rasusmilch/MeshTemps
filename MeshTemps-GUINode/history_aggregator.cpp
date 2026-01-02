#include "history_aggregator.h"
#include "history_crc.h"

#include <algorithm>
#include <string.h>
#include <time.h>
#include <math.h>

namespace {

constexpr int16_t kInvalidTempSentinel = static_cast<int16_t>(0x8000);
constexpr uint32_t kEpochSanityMin = 1700000000u;  // ~2023-11-14; adjust as desired.
constexpr uint32_t kMinValidEpochSeconds = 1704067200u;  // 2024-01-01 00:00:00 UTC
constexpr uint32_t kFramDailyStateBytes = 8u * 1024u;    // FRDS uses 2*4096 slots
constexpr uint32_t kFramHourJournalBytes = 16u * 1024u;  // FRHJ (60 frames) fits comfortably

uint32_t EpochSecondsToEpochMinute(uint32_t epoch_seconds) {
  return epoch_seconds / 60u;
}

uint32_t HourStartEpochMinute(uint32_t epoch_minute) {
  return epoch_minute - (epoch_minute % 60u);
}

}  // namespace

bool HistoryAggregator::Begin(FramStorageInterface* fram,
                             uint32_t fram_base_address,
                             uint32_t fram_region_bytes,
                             fs::FS& sd_fs,
                             const char* sd_base_dir) {
  fram_ = fram;
  fram_base_address_ = fram_base_address;
  fram_region_bytes_ = fram_region_bytes;

  storages_initialized_ = false;
  last_epoch_minute_seen_ = -1;
  daily_entries_.clear();
  daily_state_sequence_ = 0;
  current_day_start_epoch_minute_ = 0;

  if (fram_ == nullptr) return false;
  if (sd_base_dir == nullptr) return false;
  if (!sd_store_.Begin(sd_fs, sd_base_dir)) return false;

  // Build an initial descriptor snapshot (may be empty); the descriptor is rebuilt
  // at each hour boundary when time is valid.
  (void)RebuildDescriptorIfNeeded_(true);

  // If time is already valid, initialize FRAM layouts now. Otherwise, Tick() will
  // initialize on the first valid absolute time.
  if (IsTimeValid_()) {
    (void)InitializeIfTimeValid_(NowEpochSeconds_());
  }
  return true;
}



bool HistoryAggregator::InitializeIfTimeValid_(uint32_t epoch_seconds) {
  if (storages_initialized_) return true;
  if (!IsTimeValid_()) return false;

  // Split the provided FRAM region.
  if (fram_ == nullptr) return false;
  if (fram_region_bytes_ < (kFramDailyStateBytes + 4096u)) return false;

  fram_hour_base_address_ = fram_base_address_;
  const uint32_t remaining = fram_region_bytes_ - kFramDailyStateBytes;
  fram_hour_region_bytes_ = (remaining >= kFramHourJournalBytes) ? kFramHourJournalBytes : remaining;

  fram_daily_base_address_ = fram_base_address_ + fram_hour_region_bytes_;
  fram_daily_region_bytes_ = kFramDailyStateBytes;

  if (!fram_journal_.Begin(fram_, fram_hour_base_address_, fram_hour_region_bytes_,
                           static_cast<uint16_t>(slots_.size()), epoch_seconds)) {
    return false;
  }

  if (!daily_state_store_.Begin(fram_, fram_daily_base_address_, fram_daily_region_bytes_)) {
    return false;
  }

  if (!LoadOrRebuildDailyState_(epoch_seconds)) {
    // Not fatal; you can still start accumulating from now.
  }

  storages_initialized_ = true;
  return true;
}

bool HistoryAggregator::LoadOrRebuildDailyState_(uint32_t epoch_seconds) {
  const uint32_t today_start = DayStartEpochMinuteLocal_(epoch_seconds);

  FramDailyStateStore::State state;
  if (daily_state_store_.Load(&state) && state.day_start_epoch_minute == today_start) {
    daily_state_sequence_ = state.sequence;
    current_day_start_epoch_minute_ = state.day_start_epoch_minute;

    daily_entries_.clear();
    daily_entries_.reserve(state.entries.size());
    for (const auto& e : state.entries) {
      DailyAccumulatorEntry out;
      out.node_id = e.node_id;
      out.rom64 = e.rom64;
      out.sum_centi_c = e.sum_centi_c;
      out.sample_count = e.sample_count;
      out.min_centi_c = e.min_centi_c;
      out.max_centi_c = e.max_centi_c;
      daily_entries_.push_back(out);
    }
    return true;
  }

  // Fallback: rebuild from SD hourly rollups for today.
  const uint32_t day_end = today_start + 24u * 60u;
  if (!RebuildDailyFromSd_(today_start, day_end)) {
    return false;
  }
  return SaveDailyStateToFram_();
}

bool HistoryAggregator::RebuildDailyFromSd_(uint32_t day_start_epoch_minute,
                                           uint32_t day_end_epoch_minute) {
  current_day_start_epoch_minute_ = day_start_epoch_minute;
  daily_entries_.clear();

  struct HourSnap {
    struct Item {
      uint32_t node_id;
      uint64_t rom64;
      SdHistoryStore::HourlyRollupEntry rollup;
    };
    bool present = false;
    std::vector<Item> items;
  };

  HourSnap hours[24];

  const bool ok = sd_store_.ScanHourlyRollups(
      day_start_epoch_minute, day_end_epoch_minute,
      [&](uint32_t hour_start, uint16_t sensor_count,
          const uint32_t* node_ids, const uint64_t* rom64,
          const SdHistoryStore::HourlyRollupEntry* rollups) {
        const uint32_t delta = hour_start - day_start_epoch_minute;
        if (delta >= 24u * 60u) return;
        const uint8_t hour_index = static_cast<uint8_t>(delta / 60u);

        HourSnap& snap = hours[hour_index];
        snap.present = true;
        snap.items.clear();
        snap.items.reserve(sensor_count);

        for (uint16_t i = 0; i < sensor_count; ++i) {
          HourSnap::Item item;
          item.node_id = node_ids[i];
          item.rom64 = rom64[i];
          item.rollup = rollups[i];
          snap.items.push_back(item);
        }
      });

  if (!ok) return false;

  auto find_or_create = [&](uint32_t node_id, uint64_t rom) -> DailyAccumulatorEntry* {
    for (auto& e : daily_entries_) {
      if (e.node_id == node_id && e.rom64 == rom) return &e;
    }
    if (daily_entries_.size() >= max_sensors_) return nullptr;
    daily_entries_.push_back(DailyAccumulatorEntry{});
    DailyAccumulatorEntry& e = daily_entries_.back();
    e.node_id = node_id;
    e.rom64 = rom;
    return &e;
  };

  for (int h = 0; h < 24; ++h) {
    if (!hours[h].present) continue;
    for (const auto& item : hours[h].items) {
      const auto& r = item.rollup;
      if (r.sample_count == 0 || r.mean_centi_c == static_cast<int16_t>(0x8000)) continue;

      DailyAccumulatorEntry* dst = find_or_create(item.node_id, item.rom64);
      if (dst == nullptr) continue;

      dst->sum_centi_c += static_cast<int64_t>(r.mean_centi_c) *
                          static_cast<int64_t>(r.sample_count);

      const uint32_t new_count = static_cast<uint32_t>(dst->sample_count) + r.sample_count;
      dst->sample_count = (new_count > 1440u) ? 1440u : static_cast<uint16_t>(new_count);

      if (r.min_centi_c != static_cast<int16_t>(0x8000)) {
        dst->min_centi_c = (dst->min_centi_c == 0x7FFF) ? r.min_centi_c
                                                        : static_cast<int16_t>(min(dst->min_centi_c, r.min_centi_c));
      }
      if (r.max_centi_c != static_cast<int16_t>(0x8000)) {
        dst->max_centi_c = (dst->max_centi_c == static_cast<int16_t>(0x8001)) ? r.max_centi_c
                                                                              : static_cast<int16_t>(max(dst->max_centi_c, r.max_centi_c));
      }
    }
  }

  return true;
}

bool HistoryAggregator::SaveDailyStateToFram_() {
  FramDailyStateStore::State state;
  state.day_start_epoch_minute = current_day_start_epoch_minute_;
  state.sequence = daily_state_sequence_;

  state.entries.clear();
  state.entries.reserve(daily_entries_.size());
  for (const auto& e : daily_entries_) {
    FramDailyStateStore::Entry out;
    out.node_id = e.node_id;
    out.rom64 = e.rom64;
    out.sum_centi_c = e.sum_centi_c;
    out.sample_count = e.sample_count;
    out.min_centi_c = e.min_centi_c;
    out.max_centi_c = e.max_centi_c;
    state.entries.push_back(out);
  }

  if (!daily_state_store_.Save(state)) return false;
  daily_state_sequence_ += 1u;
  return true;
}

void HistoryAggregator::Tick(uint32_t now_ms) {
  if (!IsTimeValid_()) return;

  const uint32_t epoch_seconds = NowEpochSeconds_();
  if (!InitializeIfTimeValid_(epoch_seconds)) return;

  const uint32_t epoch_minute = EpochSecondsToEpochMinute(epoch_seconds);
  if (static_cast<int32_t>(epoch_minute) == last_epoch_minute_seen_) return;
  last_epoch_minute_seen_ = static_cast<int32_t>(epoch_minute);

  // Detect hour rollover BEFORE appending this minute, so we don't reset the FRAM
  // journal and lose the prior hour state.
  const uint32_t now_hour_start = HourStartEpochMinute(epoch_minute);
  const uint32_t journal_hour_start = fram_journal_.hour_start_epoch_minute();
  if (journal_hour_start != 0 && now_hour_start != journal_hour_start) {
    // Flush the completed (previous) hour to SD.
    std::vector<SdHistoryStore::HourlyRollupEntry> hour_rollups;
    if (FlushHourToSd_(journal_hour_start, &hour_rollups)) {
      UpdateDailyFromHourly_(hour_rollups);
      (void)SaveDailyStateToFram_();
    }

    // Rebuild descriptor at each hour boundary to capture add/remove sensors.
    (void)RebuildDescriptorIfNeeded_(true);

    // Start a fresh FRAM hour journal for the new hour.
    (void)fram_journal_.ResetToCurrentHour(epoch_seconds);
  }

  if (!PollAndAppendMinute_(now_ms, epoch_seconds)) return;

  // Handle day rollover AFTER we incorporated the final hour of the prior day.
  MaybeRotateDay_(epoch_seconds);
}

bool HistoryAggregator::PollAndAppendMinute_(uint32_t now_ms, uint32_t epoch_seconds) {
  const uint16_t sensor_count = static_cast<uint16_t>(slots_.size());
  if (sensor_count == 0) return true;

  // Presence bitmap (packed), padded to ceil(sensor_count/8).
  const size_t presence_bytes = (static_cast<size_t>(sensor_count) + 7u) / 8u;
  std::vector<uint8_t> presence(presence_bytes, 0);

  // Values array.
  std::vector<int16_t> values(sensor_count, kInvalidTempSentinel);

  // Build a lookup by (node_id, address) by scanning slots and then scanning MeshNodes.
  // For first pass: O(N*M) is fine at N<=100.
  for (uint16_t slot_index = 0; slot_index < sensor_count; ++slot_index) {
    const SensorSlot& slot = slots_[slot_index];
    const MeshNode* node = FindMeshNode(slot.node_id);
    if (node == nullptr) continue;

    const MeshNode::Sensor* sensor = node->FindSensor(slot.address_hex16);
    if (sensor == nullptr) continue;

    const bool fresh = sensor->has_value && (now_ms >= sensor->last_ms) &&
                       ((now_ms - sensor->last_ms) <= stale_threshold_ms_);
    if (!fresh) continue;

    // Mark presence bit.
    presence[slot_index / 8u] |= static_cast<uint8_t>(1u << (slot_index % 8u));

    // Convert float C to centi-C.
    const float temp_c = sensor->temp_c;
    if (!isnan(temp_c) && temp_c > -300.0f && temp_c < 300.0f) {
      const int32_t centi = static_cast<int32_t>(lroundf(temp_c * 100.0f));
      values[slot_index] = static_cast<int16_t>(centi);
    }
  }

  // Append to FRAM with write+readback verification inside FramHourJournal.
  return fram_journal_.AppendMinute(epoch_seconds,
                                    presence.data(), presence.size(),
                                    values.data(), values.size());
}


bool HistoryAggregator::FlushHourToSd_(
    uint32_t hour_start_epoch_minute,
    std::vector<SdHistoryStore::HourlyRollupEntry>* out_hourly_rollups) {
  const uint16_t sensor_count = static_cast<uint16_t>(slots_.size());
  if (sensor_count == 0) return false;

  HourComputationResult computation;
  if (!ComputeHourCrcAndRollup_(&computation)) return false;

  // Descriptor arrays (stable, derived from slots_).
  std::vector<uint32_t> node_ids(sensor_count);
  std::vector<uint64_t> rom64(sensor_count);
  for (uint16_t i = 0; i < sensor_count; ++i) {
    node_ids[i] = slots_[i].node_id;
    rom64[i] = slots_[i].rom64;
  }

  const bool minute_exists = sd_store_.HasMinuteHourBlock(hour_start_epoch_minute);
  if (!minute_exists) {
    if (!sd_store_.AppendMinuteHourBlock(
            hour_start_epoch_minute,
            sensor_count,
            fram_journal_.frame_bytes(),
            fram_journal_.presence_bytes_padded(),
            computation.bad_frame_mask,
            computation.payload_crc32,
            node_ids.data(),
            rom64.data(),
            [&](std::function<bool(const void*, size_t)> sink) {
              return StreamHourPayloadForSd_(computation, sink);
            })) {
      return false;
    }
  }

  const bool rollup_exists = sd_store_.HasHourlyRollupBlock(hour_start_epoch_minute);
  if (!rollup_exists) {
    if (!sd_store_.AppendHourlyRollupBlock(hour_start_epoch_minute,
                                           sensor_count,
                                           node_ids.data(),
                                           rom64.data(),
                                           computation.rollups.data())) {
      return false;
    }
  }

  if (out_hourly_rollups != nullptr) {
    *out_hourly_rollups = computation.rollups;
  }
  return true;
}

bool HistoryAggregator::FlushHourToSd_(uint32_t hour_start_epoch_minute) {
  return FlushHourToSd_(hour_start_epoch_minute, nullptr);
}

bool HistoryAggregator::RebuildDescriptorIfNeeded_(bool force) {
  // Gather all known sensors across all nodes.
  std::vector<SensorSlot> new_slots;
  const std::vector<uint32_t> node_ids = GetAllMeshNodeIds();

  for (uint32_t node_id : node_ids) {
    const MeshNode* node = FindMeshNode(node_id);
    if (node == nullptr) continue;

    for (const auto& sensor : node->sensors()) {
      if (sensor.address.length() != 16) continue;
      SensorSlot slot;
      slot.node_id = node_id;
      slot.address_hex16 = sensor.address;
      slot.rom64 = 0;
      if (!ParseHex16ToU64_(slot.address_hex16, &slot.rom64)) {
        continue;
      }
      new_slots.push_back(slot);
    }
  }

  // Stable order: by node_id then ROM.
  std::sort(new_slots.begin(), new_slots.end(),
            [](const SensorSlot& a, const SensorSlot& b) {
              if (a.node_id != b.node_id) return a.node_id < b.node_id;
              return a.rom64 < b.rom64;
            });

  if (new_slots.size() > max_sensors_) {
    new_slots.resize(max_sensors_);
  }

  if (!force && new_slots.size() == slots_.size()) {
    bool same = true;
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].node_id != new_slots[i].node_id ||
          slots_[i].rom64 != new_slots[i].rom64) {
        same = false;
        break;
      }
    }
    if (same) return true;
  }

  slots_.swap(new_slots);
  return true;
}

bool HistoryAggregator::ParseHex16ToU64_(const String& hex16, uint64_t* out) {
  if (out == nullptr) return false;
  if (hex16.length() != 16) return false;

  uint64_t value = 0;
  for (int i = 0; i < 16; ++i) {
    const char c = hex16[i];
    uint8_t nibble = 0;
    if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = static_cast<uint8_t>(10 + (c - 'a'));
    else if (c >= 'A' && c <= 'F') nibble = static_cast<uint8_t>(10 + (c - 'A'));
    else return false;
    value = (value << 4) | nibble;
  }

  *out = value;
  return true;
}

bool HistoryAggregator::IsTimeValid_() const {
  const uint32_t epoch_seconds = NowEpochSeconds_();
  return epoch_seconds >= kEpochSanityMin;
}

uint32_t HistoryAggregator::NowEpochSeconds_() const {
  const time_t now = time(nullptr);
  if (now < 0) return 0;
  return static_cast<uint32_t>(now);
}

bool HistoryAggregator::ComputeHourCrcAndRollup_(HourComputationResult* out) const {
  if (out == nullptr) return false;

  const uint16_t sensor_count = fram_journal_.sensor_count();
  if (sensor_count == 0) return false;

  out->payload_crc32 = 0xFFFFFFFFu;

  if (slots_.size() != sensor_count) return false;

  for (uint16_t i = 0; i < sensor_count; ++i) {
    const uint32_t node_id = slots_[i].node_id;
    const uint64_t rom64 = slots_[i].rom64;

    out->payload_crc32 = Crc32Update(out->payload_crc32,
                                    reinterpret_cast<const uint8_t*>(&node_id),
                                    sizeof(node_id));
    out->payload_crc32 = Crc32Update(out->payload_crc32,
                                    reinterpret_cast<const uint8_t*>(&rom64),
                                    sizeof(rom64));
  }

  out->bad_frame_mask = 0;
  out->rollups.clear();
  out->rollups.resize(sensor_count);

  // Accumulators.
  std::vector<int64_t> sum(sensor_count, 0);
  std::vector<uint16_t> count(sensor_count, 0);
  std::vector<int16_t> minv(sensor_count, static_cast<int16_t>(0x7FFF));
  std::vector<int16_t> maxv(sensor_count, static_cast<int16_t>(0x8001));

  const uint16_t presence_bytes_padded = fram_journal_.presence_bytes_padded();
  const size_t raw_presence_bytes = (static_cast<size_t>(sensor_count) + 7u) / 8u;
  const size_t values_bytes = static_cast<size_t>(sensor_count) * sizeof(int16_t);
  const size_t crc_offset = static_cast<size_t>(presence_bytes_padded) + values_bytes;

  std::vector<uint8_t> frame_bytes(fram_journal_.frame_bytes());
  std::vector<uint8_t> missing_frame(fram_journal_.frame_bytes());
  if (!fram_journal_.BuildMissingFrame(missing_frame.data(), missing_frame.size())) return false;

  const uint8_t minutes_written = fram_journal_.minutes_written();

  for (uint8_t minute_index = 0; minute_index < 60; ++minute_index) {
    if (minute_index >= minutes_written) {
      out->bad_frame_mask |= (1ULL << minute_index);
      out->payload_crc32 = Crc32Update(out->payload_crc32,
                                      missing_frame.data(),
                                      missing_frame.size());
      continue;
    }

    if (!fram_journal_.ReadRawFrame(minute_index, frame_bytes.data(), frame_bytes.size())) {
      // Treat unread as missing.
      out->bad_frame_mask |= (1ULL << minute_index);
      out->payload_crc32 = Crc32Update(out->payload_crc32, missing_frame.data(), missing_frame.size());
      continue;
    }

    const bool crc_ok = fram_journal_.FrameCrcOk(frame_bytes.data(), frame_bytes.size());
    const uint8_t* bytes_used = crc_ok ? frame_bytes.data() : missing_frame.data();

    if (!crc_ok) {
      out->bad_frame_mask |= (1ULL << minute_index);
    }

    // Payload CRC over exactly what will be written.
    out->payload_crc32 = Crc32Update(out->payload_crc32, bytes_used, fram_journal_.frame_bytes());

    // Rollup only if frame valid.
    if (!crc_ok) continue;

    const uint8_t* presence = frame_bytes.data();

    for (uint16_t sensor_index = 0; sensor_index < sensor_count; ++sensor_index) {
      const bool present = (presence[sensor_index / 8u] >> (sensor_index % 8u)) & 1u;
      if (!present) continue;

      // NOTE: frame_bytes is a uint8_t vector (alignment 1). Do not reinterpret_cast
      // into int16_t*; that is undefined behavior and can fault on some targets.
      int16_t value = 0;
      const uint8_t* value_ptr = frame_bytes.data() + static_cast<size_t>(presence_bytes_padded) +
                                 static_cast<size_t>(sensor_index) * sizeof(int16_t);
      memcpy(&value, value_ptr, sizeof(value));
      if (value == static_cast<int16_t>(0x8000)) continue;

      sum[sensor_index] += value;
      ++count[sensor_index];
      if (value < minv[sensor_index]) minv[sensor_index] = value;
      if (value > maxv[sensor_index]) maxv[sensor_index] = value;
    }
  }

  // Finalize rollups.
  for (uint16_t sensor_index = 0; sensor_index < sensor_count; ++sensor_index) {
    SdHistoryStore::HourlyRollupEntry entry;
    entry.sample_count = count[sensor_index];
    if (entry.sample_count == 0) {
      entry.mean_centi_c = static_cast<int16_t>(0x8000);
      entry.min_centi_c = static_cast<int16_t>(0x8000);
      entry.max_centi_c = static_cast<int16_t>(0x8000);
    } else {
      const int32_t mean = static_cast<int32_t>(llround(static_cast<double>(sum[sensor_index]) /
                                                        static_cast<double>(entry.sample_count)));
      entry.mean_centi_c = static_cast<int16_t>(mean);
      entry.min_centi_c = minv[sensor_index];
      entry.max_centi_c = maxv[sensor_index];
    }
    out->rollups[sensor_index] = entry;
  }

  return true;
}

bool HistoryAggregator::StreamHourPayloadForSd_(
    const HourComputationResult& info,
    std::function<bool(const void*, size_t)> sink) const {
  if (!sink) return false;

  std::vector<uint8_t> frame_bytes(fram_journal_.frame_bytes());
  std::vector<uint8_t> missing_frame(fram_journal_.frame_bytes());
  if (!fram_journal_.BuildMissingFrame(missing_frame.data(), missing_frame.size())) return false;

  const uint8_t minutes_written = fram_journal_.minutes_written();

  for (uint8_t minute_index = 0; minute_index < 60; ++minute_index) {
    if (minute_index >= minutes_written) {
      if (!sink(missing_frame.data(), missing_frame.size())) return false;
      continue;
    }

    const bool should_replace = (info.bad_frame_mask >> minute_index) & 1ULL;

    if (should_replace) {
      if (!sink(missing_frame.data(), missing_frame.size())) return false;
      continue;
    }

    if (!fram_journal_.ReadRawFrame(minute_index, frame_bytes.data(), frame_bytes.size())) {
      // If unread here, replace (defensive).
      if (!sink(missing_frame.data(), missing_frame.size())) return false;
      continue;
    }

    if (!sink(frame_bytes.data(), frame_bytes.size())) return false;
  }

  return true;
}

uint32_t HistoryAggregator::DayStartEpochMinuteLocal_(uint32_t epoch_seconds) {
  time_t t = static_cast<time_t>(epoch_seconds);
  struct tm tm_buf;
  if (localtime_r(&t, &tm_buf) == nullptr) {
    return (epoch_seconds / 60u) - ((epoch_seconds / 60u) % 1440u);  // fallback UTC-ish
  }
  tm_buf.tm_hour = 0;
  tm_buf.tm_min = 0;
  tm_buf.tm_sec = 0;
  const time_t midnight = mktime(&tm_buf);
  if (midnight <= 0) {
    return (epoch_seconds / 60u) - ((epoch_seconds / 60u) % 1440u);
  }
  return static_cast<uint32_t>(midnight) / 60u;
}

void HistoryAggregator::MaybeRotateDay_(uint32_t epoch_seconds) {
  const uint32_t day_start = DayStartEpochMinuteLocal_(epoch_seconds);

  if (current_day_start_epoch_minute_ == 0) {
    current_day_start_epoch_minute_ = day_start;
    return;
  }

  if (day_start == current_day_start_epoch_minute_) {
    return;
  }

  // Day changed: flush previous day once, append-only.
  (void)FlushDailyToSd_();

  // Reset for new day.
  daily_entries_.clear();
  current_day_start_epoch_minute_ = day_start;
}

void HistoryAggregator::UpdateDailyFromHourly_(
    const std::vector<SdHistoryStore::HourlyRollupEntry>& hourly_rollups) {
  // hourly_rollups is in current hour descriptor order = slots_ order.
  const uint16_t sensor_count = static_cast<uint16_t>(slots_.size());
  if (hourly_rollups.size() != sensor_count) return;

  for (uint16_t i = 0; i < sensor_count; ++i) {
    const SdHistoryStore::HourlyRollupEntry& hr = hourly_rollups[i];
    if (hr.sample_count == 0) continue;
    if (hr.mean_centi_c == static_cast<int16_t>(0x8000)) continue;

    const uint32_t node_id = slots_[i].node_id;
    const uint64_t rom64 = slots_[i].rom64;

    // Find or create entry (linear search is fine at <=100 sensors).
    DailyAccumulatorEntry* entry = nullptr;
    for (auto& candidate : daily_entries_) {
      if (candidate.node_id == node_id && candidate.rom64 == rom64) {
        entry = &candidate;
        break;
      }
    }
    if (entry == nullptr) {
      DailyAccumulatorEntry created;
      created.node_id = node_id;
      created.rom64 = rom64;
      daily_entries_.push_back(created);
      entry = &daily_entries_.back();
    }

    // Sum approximation: mean * count (mean already rounded).
    entry->sum_centi_c += static_cast<int64_t>(hr.mean_centi_c) *
                          static_cast<int64_t>(hr.sample_count);

    const uint32_t new_count = static_cast<uint32_t>(entry->sample_count) + hr.sample_count;
    entry->sample_count = (new_count > 1440u) ? 1440u : static_cast<uint16_t>(new_count);

    if (hr.min_centi_c != static_cast<int16_t>(0x8000) && hr.min_centi_c < entry->min_centi_c) {
      entry->min_centi_c = hr.min_centi_c;
    }
    if (hr.max_centi_c != static_cast<int16_t>(0x8000) && hr.max_centi_c > entry->max_centi_c) {
      entry->max_centi_c = hr.max_centi_c;
    }
  }
}

bool HistoryAggregator::FlushDailyToSd_() {
  if (current_day_start_epoch_minute_ == 0) return true;
  if (daily_entries_.empty()) return true;

  // Sort stable by (node_id, rom64) so records are deterministic.
  std::sort(daily_entries_.begin(), daily_entries_.end(),
            [](const DailyAccumulatorEntry& a, const DailyAccumulatorEntry& b) {
              if (a.node_id != b.node_id) return a.node_id < b.node_id;
              return a.rom64 < b.rom64;
            });

  const uint16_t sensor_count = static_cast<uint16_t>(daily_entries_.size());
  std::vector<uint32_t> node_ids(sensor_count);
  std::vector<uint64_t> rom64(sensor_count);
  std::vector<SdHistoryStore::HourlyRollupEntry> daily_rollups(sensor_count);

  for (uint16_t i = 0; i < sensor_count; ++i) {
    const DailyAccumulatorEntry& e = daily_entries_[i];
    node_ids[i] = e.node_id;
    rom64[i] = e.rom64;

    SdHistoryStore::HourlyRollupEntry out;
    out.sample_count = e.sample_count;
    if (out.sample_count == 0) {
      out.mean_centi_c = static_cast<int16_t>(0x8000);
      out.min_centi_c = static_cast<int16_t>(0x8000);
      out.max_centi_c = static_cast<int16_t>(0x8000);
    } else {
      const int32_t mean =
          static_cast<int32_t>(llround(static_cast<double>(e.sum_centi_c) /
                                       static_cast<double>(out.sample_count)));
      out.mean_centi_c = static_cast<int16_t>(mean);
      out.min_centi_c = (e.min_centi_c == 0x7FFF) ? static_cast<int16_t>(0x8000) : e.min_centi_c;
      out.max_centi_c = (e.max_centi_c == static_cast<int16_t>(0x8001)) ? static_cast<int16_t>(0x8000) : e.max_centi_c;
    }

    daily_rollups[i] = out;
  }

  // Append-only + CRC32 + read-back verify (in SdHistoryStore).
  return sd_store_.AppendDailyRollupBlock(current_day_start_epoch_minute_,
                                         sensor_count,
                                         node_ids.data(),
                                         rom64.data(),
                                         daily_rollups.data());
}