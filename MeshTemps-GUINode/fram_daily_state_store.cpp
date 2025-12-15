#include "fram_daily_state_store.h"

#include <string.h>

#include "history_crc.h"

namespace {
constexpr size_t kVerifyChunkBytes = 64;
}  // namespace

bool FramDailyStateStore::Begin(FramStorageInterface* fram,
                               uint32_t base_address,
                               uint32_t region_bytes) {
  fram_ = fram;
  base_address_ = base_address;
  region_bytes_ = region_bytes;

  if (fram_ == nullptr) return false;
  if (region_bytes_ < 2u * slot_bytes_) return false;
  if (slot_bytes_ < (kHeaderBytes + sizeof(Entry))) return false;

  initialized_ = true;
  return true;
}

uint32_t FramDailyStateStore::ComputePayloadCrc32_(const void* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(data), length);
  return crc;
}

uint32_t FramDailyStateStore::ComputeHeaderCrc32_(Header header) {
  header.header_crc32 = 0;
  uint32_t crc = 0xFFFFFFFFu;
  crc = Crc32Update(crc, reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  return crc;
}

bool FramDailyStateStore::WriteBytesVerified_(uint32_t address,
                                             const void* data,
                                             size_t length) const {
  const uint8_t* src = reinterpret_cast<const uint8_t*>(data);
  uint8_t verify[kVerifyChunkBytes];

  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = (length - offset > sizeof(verify)) ? sizeof(verify) : (length - offset);
    if (!fram_->WriteBytes(address + static_cast<uint32_t>(offset), src + offset, chunk)) {
      return false;
    }
    if (!fram_->ReadBytes(address + static_cast<uint32_t>(offset), verify, chunk)) {
      return false;
    }
    if (memcmp(verify, src + offset, chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

bool FramDailyStateStore::ReadBytesChecked_(uint32_t address,
                                           void* data,
                                           size_t length) const {
  if (!fram_->ReadBytes(address, data, length)) return false;
  return true;
}

bool FramDailyStateStore::ReadHeader_(uint32_t slot_base, Header* out) const {
  if (out == nullptr) return false;
  Header header;
  if (!ReadBytesChecked_(slot_base, &header, sizeof(header))) return false;

  if (header.magic != kMagic) return false;
  if (header.version != kVersion) return false;
  if (header.header_bytes != sizeof(Header)) return false;
  if (header.entry_bytes != sizeof(Entry)) return false;

  const uint32_t stored_crc = header.header_crc32;
  const uint32_t expected_crc = ComputeHeaderCrc32_(header);
  if (stored_crc != expected_crc) return false;

  const uint32_t max_payload = slot_bytes_ - sizeof(Header);
  if (header.payload_bytes > max_payload) return false;
  if (header.payload_bytes != static_cast<uint32_t>(header.entry_count) * sizeof(Entry)) return false;

  *out = header;
  return true;
}

bool FramDailyStateStore::ReadPayloadAndVerify_(uint32_t slot_base,
                                               const Header& header,
                                               std::vector<Entry>* out_entries) const {
  if (out_entries == nullptr) return false;

  out_entries->clear();
  if (header.entry_count == 0) return true;

  out_entries->resize(header.entry_count);
  const uint32_t payload_address = slot_base + sizeof(Header);
  const size_t payload_len = static_cast<size_t>(header.payload_bytes);

  if (!ReadBytesChecked_(payload_address, out_entries->data(), payload_len)) return false;

  const uint32_t crc = ComputePayloadCrc32_(out_entries->data(), payload_len);
  return (crc == header.payload_crc32);
}

bool FramDailyStateStore::Load(State* out) const {
  if (!initialized_ || out == nullptr) return false;

  Header header_a, header_b;
  const bool ok_a = ReadHeader_(SlotBaseA_(), &header_a);
  const bool ok_b = ReadHeader_(SlotBaseB_(), &header_b);

  if (!ok_a && !ok_b) return false;

  const bool choose_b = ok_b && (!ok_a || header_b.sequence >= header_a.sequence);
  const Header chosen_header = choose_b ? header_b : header_a;
  const uint32_t chosen_base = choose_b ? SlotBaseB_() : SlotBaseA_();

  std::vector<Entry> entries;
  if (!ReadPayloadAndVerify_(chosen_base, chosen_header, &entries)) {
    // Try the other slot if available.
    if (choose_b && ok_a) {
      entries.clear();
      if (!ReadPayloadAndVerify_(SlotBaseA_(), header_a, &entries)) return false;
      out->sequence = header_a.sequence;
      out->day_start_epoch_minute = header_a.day_start_epoch_minute;
      out->entries = entries;
      return true;
    }
    if (!choose_b && ok_b) {
      entries.clear();
      if (!ReadPayloadAndVerify_(SlotBaseB_(), header_b, &entries)) return false;
      out->sequence = header_b.sequence;
      out->day_start_epoch_minute = header_b.day_start_epoch_minute;
      out->entries = entries;
      return true;
    }
    return false;
  }

  out->sequence = chosen_header.sequence;
  out->day_start_epoch_minute = chosen_header.day_start_epoch_minute;
  out->entries = entries;
  return true;
}

bool FramDailyStateStore::Save(const State& state) {
  if (!initialized_) return false;

  const uint16_t entry_count = static_cast<uint16_t>(state.entries.size());
  const uint32_t payload_bytes = static_cast<uint32_t>(entry_count) * sizeof(Entry);

  if (payload_bytes > (slot_bytes_ - sizeof(Header))) return false;

  const uint32_t next_sequence = state.sequence + 1u;
  const bool write_b = (next_sequence & 1u) != 0u;
  const uint32_t slot_base = write_b ? SlotBaseB_() : SlotBaseA_();

  Header header;
  memset(&header, 0, sizeof(header));
  header.magic = kMagic;
  header.version = kVersion;
  header.header_bytes = sizeof(Header);
  header.sequence = next_sequence;
  header.day_start_epoch_minute = state.day_start_epoch_minute;
  header.entry_count = entry_count;
  header.entry_bytes = sizeof(Entry);
  header.payload_bytes = payload_bytes;

  uint32_t payload_crc = 0xFFFFFFFFu;
  if (payload_bytes > 0) {
    payload_crc = ComputePayloadCrc32_(state.entries.data(), payload_bytes);
  }
  header.payload_crc32 = payload_crc;
  header.header_crc32 = ComputeHeaderCrc32_(header);

  // Atomic-ish: write payload first, header last.
  const uint32_t payload_address = slot_base + sizeof(Header);
  if (payload_bytes > 0) {
    if (!WriteBytesVerified_(payload_address, state.entries.data(), payload_bytes)) return false;
  }
  if (!WriteBytesVerified_(slot_base, &header, sizeof(header))) return false;

  return true;
}
