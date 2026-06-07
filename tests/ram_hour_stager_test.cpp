#include "history_hour_stager.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

constexpr uint32_t kHour = 30000000U;

void TestSlotAssignment() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));

  uint8_t slot = 0xFF;
  assert(stager.FindOrCreateSlot("2800000000000001", 0x10000001U, 0, &slot));
  assert(slot == 0);
  assert(stager.FindOrCreateSlot("2800000000000002", 0x10000001U, 1, &slot));
  assert(slot == 1);
  assert(stager.FindOrCreateSlot("2800000000000001", 0x10000001U, 2, &slot));
  assert(slot == 0);
  assert(stager.status().active_slot_count == 2);
}

void TestMidHourDiscovery() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000010", 0x10000001U, 20, 21.25f, false));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  assert(snapshot.active_slot_count == 1);
  assert(snapshot.slots[0].first_seen_minute == 20);
  assert(!snapshot.frames[19].IsPresent(0));
  assert(snapshot.frames[20].IsPresent(0));
  assert(snapshot.frames[20].TemperatureCentiC(0) == 2125);
}

void TestDisappearAndReturn() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000020", 0x10000001U, 1, 18.0f, false));
  assert(stager.RecordMissing("2800000000000020", 0x10000001U, 2));
  assert(stager.RecordSample("2800000000000020", 0x10000001U, 6, 19.0f, false));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  assert(snapshot.active_slot_count == 1);
  assert(snapshot.frames[1].IsPresent(0));
  assert(!snapshot.frames[2].IsPresent(0));
  assert(snapshot.frames[6].IsPresent(0));
  assert(snapshot.slots[0].last_seen_minute == 6);
}

void TestNodeContextChange() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000030", 0x10000001U, 3, 20.0f, false));
  assert(stager.RecordSample("2800000000000030", 0x20000002U, 4, 20.5f, false));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  assert(snapshot.active_slot_count == 1);
  assert(snapshot.slots[0].last_known_node_id == 0x20000002U);
  assert(snapshot.frames[3].IsPresent(0));
  assert(snapshot.frames[4].IsPresent(0));
}

void TestCorrectedBitmap() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000040", 0x10000001U, 5, 10.0f, false));
  assert(stager.RecordSample("2800000000000040", 0x10000001U, 6, 11.0f, true));
  assert(stager.RecordMissing("2800000000000040", 0x10000001U, 7));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  assert(snapshot.frames[5].IsPresent(0));
  assert(!snapshot.frames[5].IsCorrected(0));
  assert(snapshot.frames[6].IsPresent(0));
  assert(snapshot.frames[6].IsCorrected(0));
  assert(!snapshot.frames[7].IsPresent(0));
  assert(!snapshot.frames[7].IsCorrected(0));
}

void TestInvalidTemperature() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(!stager.RecordSample("2800000000000050", 0x10000001U, 8, NAN, false));
  assert(!stager.RecordSample("2800000000000050", 0x10000001U, 9, INFINITY, true));
  assert(!stager.RecordSample("2800000000000050", 0x10000001U, 10, 400.0f, true));

  HistoryHourSnapshot snapshot;
  assert(stager.ExportSnapshot(&snapshot));
  assert(snapshot.active_slot_count == 1);
  assert(!snapshot.frames[8].IsPresent(0));
  assert(!snapshot.frames[9].IsPresent(0));
  assert(!snapshot.frames[10].IsPresent(0));
  assert(snapshot.status.invalid_temperature_count == 3);
}

void TestSlotCap() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));

  char addr[17] = "2800000000000000";
  for (uint8_t i = 0; i < kHistorySlotCapacity; ++i) {
    std::snprintf(addr, sizeof(addr), "28%014X", static_cast<unsigned>(i));
    uint8_t slot = 0xFF;
    assert(stager.FindOrCreateSlot(addr, 0x10000001U, i % kHistoryMinutesPerHour, &slot));
    assert(slot == i);
  }

  uint8_t overflow_slot = 0xFF;
  assert(!stager.FindOrCreateSlot("2800000000009999", 0x10000001U, 30, &overflow_slot));
  assert(overflow_slot == 0xFF);
  assert(stager.status().active_slot_count == kHistorySlotCapacity);
  assert(stager.status().overflowed);
  assert(stager.status().overflow_count == 1);
}

void TestExportDeterministicAndNeutral() {
  RamHourStager stager;
  assert(stager.ResetHour(kHour));
  assert(stager.RecordSample("2800000000000060", 0x10000001U, 0, 12.34f, true));
  assert(stager.RecordSample("2800000000000061", 0x10000002U, 59, -1.25f, false));

  HistoryHourSnapshot a;
  HistoryHourSnapshot b;
  assert(stager.ExportSnapshot(&a));
  assert(stager.ExportSnapshot(&b));

  assert(a.format_version == kHistoryHourSnapshotFormatVersion);
  assert(a.hour_start_epoch_minute == kHour);
  assert(a.active_slot_count == 2);
  assert(a.status.active_slot_count == 2);
  assert(a.status.exports_completed == 1);
  assert(b.status.exports_completed == 2);
  assert(a.frames.size() == kHistoryMinutesPerHour);
  assert(a.slots[0].rom64 == b.slots[0].rom64);
  assert(std::strcmp(a.slots[0].addr16, b.slots[0].addr16) == 0);
  assert(a.frames[0].presence == b.frames[0].presence);
  assert(a.frames[59].temp_c_x100[1] == -125);
}

void TestCentiConversion() {
  HistoryCentiCResult r = HistoryTempCToCentiC(1.235f);
  assert(r.valid);
  assert(r.value == 124);
  r = HistoryTempCToCentiC(-1.235f);
  assert(r.valid);
  assert(r.value == -124);
  assert(!HistoryTempCToCentiC(NAN).valid);
  assert(!HistoryTempCToCentiC(400.0f).valid);
}

}  // namespace

int main() {
  TestSlotAssignment();
  TestMidHourDiscovery();
  TestDisappearAndReturn();
  TestNodeContextChange();
  TestCorrectedBitmap();
  TestInvalidTemperature();
  TestSlotCap();
  TestExportDeterministicAndNeutral();
  TestCentiConversion();
  std::cout << "ram_hour_stager_test: PASS" << std::endl;
  return 0;
}
