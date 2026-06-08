#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <iostream>

#include "history_hour_stager.h"
#include "sd_finalized_hour_block.h"
#include "sd_finalized_hour_write_coalescer.h"

namespace {

struct CaptureSink {
  uint8_t bytes[kSdFinalizedHourMaxRecordBytes]{};
  size_t used = 0;
  size_t write_calls = 0;
  size_t fail_after_bytes = kSdFinalizedHourMaxRecordBytes + 1U;
};

bool CaptureWrite(const uint8_t* data, size_t len, void* ctx) {
  CaptureSink* sink = static_cast<CaptureSink*>(ctx);
  if (sink == nullptr) return false;
  if (len == 0U) return true;
  if (data == nullptr) return false;
  if (sink->used + len > sizeof(sink->bytes)) return false;
  if (sink->used + len > sink->fail_after_bytes) return false;
  memcpy(sink->bytes + sink->used, data, len);
  sink->used += len;
  ++sink->write_calls;
  return true;
}

bool DirectCaptureWrite(const uint8_t* data, size_t len, void* ctx) {
  return CaptureWrite(data, len, ctx);
}

RamHourStager BuildStager() {
  RamHourStager stager;
  assert(stager.ResetHour(28928160U));
  assert(stager.RecordSample("28FF000000000001", 11U, 0U, 21.25f, false));
  assert(stager.RecordSample("28FF000000000002", 12U, 17U, -4.5f, true));
  assert(stager.RecordMissing("28FF000000000001", 11U, 18U));
  assert(stager.RecordSample("28FF000000000001", 13U, 59U, 22.0f, true));
  return stager;
}

struct CoalescedEncodeContext {
  SdFinalizedHourWriteCoalescer* coalescer = nullptr;
};

bool CoalescerWrite(const uint8_t* data, size_t len, void* ctx) {
  CoalescedEncodeContext* coalesced = static_cast<CoalescedEncodeContext*>(ctx);
  if (coalesced == nullptr || coalesced->coalescer == nullptr) return false;
  return coalesced->coalescer->Append(data, len);
}

void TestCoalescedOutputEqualsBaseline() {
  const RamHourStager stager = BuildStager();
  const RamHourStagerFinalizedHourSource source(stager);

  CaptureSink baseline;
  SdFinalizedHourBlockHeader baseline_header;
  assert(WriteSdFinalizedHourBlock(source, DirectCaptureWrite, &baseline,
                                   &baseline_header, nullptr));

  CaptureSink coalesced_sink;
  uint8_t buffer[64]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &coalesced_sink,
                                          buffer, sizeof(buffer));
  CoalescedEncodeContext ctx;
  ctx.coalescer = &coalescer;
  SdFinalizedHourBlockHeader coalesced_header;
  SdFinalizedHourWriteStatus status;
  assert(WriteSdFinalizedHourBlock(source, CoalescerWrite, &ctx,
                                   &coalesced_header, &status));
  assert(coalescer.logical_bytes() == coalesced_header.record_bytes);
  assert(coalescer.Flush());
  assert(!coalescer.failed());
  assert(coalescer.physical_bytes() == coalesced_header.record_bytes);

  assert(baseline.used == coalesced_sink.used);
  assert(baseline_header.record_bytes == coalesced_header.record_bytes);
  assert(status.bytes_written == coalesced_header.record_bytes);
  assert(memcmp(baseline.bytes, coalesced_sink.bytes, baseline.used) == 0);
  assert(VerifySdFinalizedHourBlock(coalesced_sink.bytes, coalesced_sink.used));
}

void TestFlushRequired() {
  CaptureSink sink;
  uint8_t buffer[16]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  const uint8_t data[] = {1U, 2U, 3U};
  assert(coalescer.Append(data, sizeof(data)));
  assert(coalescer.logical_bytes() == sizeof(data));
  assert(coalescer.physical_bytes() == 0U);
  assert(sink.used == 0U);
  assert(coalescer.Flush());
  assert(coalescer.physical_bytes() == sizeof(data));
  assert(sink.used == sizeof(data));
  assert(memcmp(sink.bytes, data, sizeof(data)) == 0);
}

void TestExactBoundary() {
  CaptureSink sink;
  uint8_t buffer[8]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  const uint8_t data[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
  assert(coalescer.Append(data, sizeof(data)));
  assert(coalescer.logical_bytes() == sizeof(data));
  assert(coalescer.physical_bytes() == sizeof(data));
  assert(sink.used == sizeof(data));
  assert(sink.write_calls == 1U);
  assert(coalescer.Flush());
  assert(sink.write_calls == 1U);
  assert(memcmp(sink.bytes, data, sizeof(data)) == 0);
}

void TestCrossBoundaryAppend() {
  CaptureSink sink;
  uint8_t buffer[8]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  const uint8_t first[] = {1U, 2U, 3U, 4U, 5U};
  const uint8_t second[] = {6U, 7U, 8U, 9U, 10U, 11U};
  assert(coalescer.Append(first, sizeof(first)));
  assert(coalescer.Append(second, sizeof(second)));
  assert(sink.used == sizeof(first));
  assert(coalescer.Flush());
  const uint8_t expected[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U};
  assert(sink.used == sizeof(expected));
  assert(memcmp(sink.bytes, expected, sizeof(expected)) == 0);
}

void TestLargeChunkBypass() {
  CaptureSink sink;
  uint8_t buffer[8]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  const uint8_t prefix[] = {1U, 2U, 3U};
  const uint8_t large[] = {4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U};
  assert(sizeof(large) > sizeof(buffer));
  assert(coalescer.Append(prefix, sizeof(prefix)));
  assert(coalescer.Append(large, sizeof(large)));
  assert(coalescer.physical_bytes() == sizeof(prefix) + sizeof(large));
  assert(sink.write_calls == 2U);
  assert(coalescer.Flush());
  const uint8_t expected[] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U};
  assert(sink.used == sizeof(expected));
  assert(memcmp(sink.bytes, expected, sizeof(expected)) == 0);
}

void TestUnderlyingWriteFailure() {
  CaptureSink sink;
  sink.fail_after_bytes = 0U;
  uint8_t buffer[8]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  const uint8_t data[] = {1U, 2U, 3U};
  assert(coalescer.Append(data, sizeof(data)));
  assert(!coalescer.Flush());
  assert(coalescer.failed());
  assert(!coalescer.Append(data, sizeof(data)));
  assert(!coalescer.Flush());
}

void TestInvalidArguments() {
  CaptureSink sink;
  uint8_t buffer[8]{};
  SdFinalizedHourWriteCoalescer coalescer(CaptureWrite, &sink, buffer, sizeof(buffer));
  assert(coalescer.Append(nullptr, 0U));
  assert(coalescer.logical_bytes() == 0U);
  assert(!coalescer.Append(nullptr, 1U));
  assert(coalescer.failed());

  SdFinalizedHourWriteCoalescer null_sink(nullptr, &sink, buffer, sizeof(buffer));
  const uint8_t data[] = {1U};
  assert(!null_sink.Append(data, sizeof(data)));

  SdFinalizedHourWriteCoalescer null_buffer(CaptureWrite, &sink, nullptr, sizeof(buffer));
  assert(!null_buffer.Append(data, sizeof(data)));
}

}  // namespace

int main() {
  TestCoalescedOutputEqualsBaseline();
  TestFlushRequired();
  TestExactBoundary();
  TestCrossBoundaryAppend();
  TestLargeChunkBypass();
  TestUnderlyingWriteFailure();
  TestInvalidArguments();
  std::cout << "sd_finalized_hour_write_coalescer_test: PASS" << std::endl;
  return 0;
}
