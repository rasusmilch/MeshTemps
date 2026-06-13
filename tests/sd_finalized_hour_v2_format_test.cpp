#include "sd_finalized_hour_v2_format.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct TestHarness {
  const char* current_test = nullptr;
  uint32_t failures = 0;
};

TestHarness g_test;

bool CheckTrue(bool condition, const char* expr, const char* file, int line) {
  if (condition) return true;
  std::cerr << file << ":" << line << " in " << g_test.current_test
            << ": CHECK_TRUE failed: " << expr << std::endl;
  ++g_test.failures;
  return false;
}

template <typename A, typename E>
bool CheckEq(const A& actual, const E& expected, const char* a, const char* e,
             const char* file, int line) {
  if (actual == expected) return true;
  std::cerr << file << ":" << line << " in " << g_test.current_test
            << ": CHECK_EQ failed: " << a << " != " << e << std::endl;
  ++g_test.failures;
  return false;
}

#define CHECK_TRUE(expr) CheckTrue(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define CHECK_EQ(actual, expected) CheckEq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

#define CHECK_CONTAINS(text, needle) CHECK_TRUE(Contains((text), (needle)))

std::string Preamble() {
  char buf[kSdFinalizedHourV2PreambleMaxBytes] = {};
  const size_t needed = BuildSdFinalizedHourV2Preamble(buf, sizeof(buf));
  CHECK_TRUE(needed > 0U);
  CHECK_TRUE(needed < sizeof(buf));
  return std::string(buf);
}

void ExpectTable(const SdFinalizedHourV2FieldSpec* fields,
                 size_t count,
                 uint16_t expected_size) {
  uint16_t offset = 0;
  for (size_t i = 0; i < count; ++i) {
    CHECK_TRUE(fields[i].name != nullptr);
    CHECK_TRUE(fields[i].type != nullptr);
    CHECK_EQ(fields[i].offset, offset);
    offset = static_cast<uint16_t>(offset + fields[i].bytes);
  }
  CHECK_EQ(offset, expected_size);
}

bool HasField(const SdFinalizedHourV2FieldSpec* fields, size_t count, const char* name) {
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(fields[i].name, name) == 0) return true;
  }
  return false;
}

bool TablesHaveForbiddenField(const char* field) {
  size_t count = 0;
  const SdFinalizedHourV2FieldSpec* tables[] = {
      SdFinalizedHourV2HeaderFields(&count),
  };
  (void)tables;
  const SdFinalizedHourV2FieldSpec* f = SdFinalizedHourV2HeaderFields(&count);
  if (HasField(f, count, field)) return true;
  f = SdFinalizedHourV2IndexEntryFields(&count);
  if (HasField(f, count, field)) return true;
  f = SdFinalizedHourV2BlockHeaderFields(&count);
  if (HasField(f, count, field)) return true;
  f = SdFinalizedHourV2DescriptorFields(&count);
  if (HasField(f, count, field)) return true;
  f = SdFinalizedHourV2PayloadFields(&count);
  return HasField(f, count, field);
}

void TestCrc32IsoHdlcCheckValue() {
  const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
  CHECK_EQ(Crc32IsoHdlc(check, sizeof(check)), 0xCBF43926u);
}

void TestV2MagicBytes() {
  uint8_t bytes[4] = {};
  size_t o = 0;
  CHECK_TRUE(SdFinalizedHourV2PutU32Le(bytes, sizeof(bytes), &o, kSdFinalizedHourV2RecordMagic));
  CHECK_TRUE(std::memcmp(bytes, "MTH2", 4) == 0);
  o = 0;
  CHECK_TRUE(SdFinalizedHourV2PutU32Le(bytes, sizeof(bytes), &o, kSdFinalizedHourV2BlockMagic));
  CHECK_TRUE(std::memcmp(bytes, "MSB2", 4) == 0);
  CHECK_EQ(kSdFinalizedHourV2RecordVersion, static_cast<uint16_t>(2));
  CHECK_EQ(kSdFinalizedHourV2BlockVersion, static_cast<uint16_t>(2));
}

void TestBinaryStartMarker() {
  CHECK_TRUE(std::strcmp(kSdFinalizedHourV2BinaryStartMarker,
                         "%%MESH_TEMPS_BINARY_START%%\n") == 0);
}

void TestV2FieldSizes() {
  CHECK_EQ(kSdFinalizedHourV2HeaderBytes, static_cast<uint16_t>(48));
  CHECK_EQ(kSdFinalizedHourV2IndexEntryBytes, static_cast<uint16_t>(12));
  CHECK_EQ(kSdFinalizedHourV2BlockHeaderBytes, static_cast<uint16_t>(36));
  CHECK_EQ(kSdFinalizedHourV2DescriptorBytes, static_cast<uint16_t>(108));
  CHECK_EQ(kSdFinalizedHourV2PayloadBytes, static_cast<uint16_t>(136));
  CHECK_EQ(kSdFinalizedHourV2FixedBlockBytes, static_cast<uint16_t>(280));
}

void TestV2FieldOffsets() {
  size_t count = 0;
  const SdFinalizedHourV2FieldSpec* fields = SdFinalizedHourV2HeaderFields(&count);
  ExpectTable(fields, count, kSdFinalizedHourV2HeaderBytes);
  fields = SdFinalizedHourV2IndexEntryFields(&count);
  ExpectTable(fields, count, kSdFinalizedHourV2IndexEntryBytes);
  fields = SdFinalizedHourV2BlockHeaderFields(&count);
  ExpectTable(fields, count, kSdFinalizedHourV2BlockHeaderBytes);
  fields = SdFinalizedHourV2DescriptorFields(&count);
  ExpectTable(fields, count, kSdFinalizedHourV2DescriptorBytes);
  fields = SdFinalizedHourV2PayloadFields(&count);
  ExpectTable(fields, count, kSdFinalizedHourV2PayloadBytes);
}

void TestV2SchemaFieldTablesContainEveryApprovedField() {
  size_t count = 0;
  const auto* h = SdFinalizedHourV2HeaderFields(&count);
  const char* header[] = {"record_magic","record_version","header_bytes","record_bytes","hour_start_epoch_minute","sensor_count","index_entry_bytes","index_offset","index_bytes","sensor_blocks_offset","sensor_blocks_bytes","payload_crc32","header_crc32","flags"};
  for (const char* name : header) CHECK_TRUE(HasField(h, count, name));
  const auto* i = SdFinalizedHourV2IndexEntryFields(&count);
  CHECK_TRUE(HasField(i, count, "rom64"));
  CHECK_TRUE(HasField(i, count, "sensor_block_offset_from_record_start"));
  const auto* b = SdFinalizedHourV2BlockHeaderFields(&count);
  const char* block[] = {"block_magic","block_version","block_header_bytes","block_bytes","descriptor_bytes","payload_bytes","bitmap_bytes","sample_count","sample_bytes","sample_encoding","block_crc32","flags"};
  for (const char* name : block) CHECK_TRUE(HasField(b, count, name));
  const auto* d = SdFinalizedHourV2DescriptorFields(&count);
  const char* desc[] = {"rom64","last_known_node_id","first_seen_minute","last_seen_minute","valid_sample_count","missing_or_invalid_count","corrected_sample_count","node_label_len","sensor_label_len","descriptor_flags","node_label","sensor_label"};
  for (const char* name : desc) CHECK_TRUE(HasField(d, count, name));
  const auto* p = SdFinalizedHourV2PayloadFields(&count);
  CHECK_TRUE(HasField(p, count, "presence_bitmap"));
  CHECK_TRUE(HasField(p, count, "corrected_bitmap"));
  CHECK_TRUE(HasField(p, count, "samples"));
}

void TestV2SchemaDoesNotContainDurableSlotId() { CHECK_TRUE(!TablesHaveForbiddenField("slot_id")); }
void TestV2SchemaDoesNotContainStoredAddr16() { CHECK_TRUE(!TablesHaveForbiddenField("addr16")); }

void TestPreambleIncludesFormatAndMarker() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "MeshTemps finalized-hour v2 format");
  CHECK_CONTAINS(p, "format_version=2");
  CHECK_CONTAINS(p, "%%MESH_TEMPS_BINARY_START%%");
}
void TestPreambleIncludesEndian() { CHECK_CONTAINS(Preamble(), "little-endian"); }

void TestPreambleIncludesEveryFieldNameTypeAndByteLength() {
  const std::string p = Preamble();
  size_t count = 0;
  const SdFinalizedHourV2FieldSpec* all[] = {
      SdFinalizedHourV2HeaderFields(&count),
  };
  (void)all;
  const SdFinalizedHourV2FieldSpec* tables[] = {
      SdFinalizedHourV2HeaderFields(nullptr), SdFinalizedHourV2IndexEntryFields(nullptr),
      SdFinalizedHourV2BlockHeaderFields(nullptr), SdFinalizedHourV2DescriptorFields(nullptr),
      SdFinalizedHourV2PayloadFields(nullptr)};
  size_t counts[] = {0,0,0,0,0};
  SdFinalizedHourV2HeaderFields(&counts[0]);
  SdFinalizedHourV2IndexEntryFields(&counts[1]);
  SdFinalizedHourV2BlockHeaderFields(&counts[2]);
  SdFinalizedHourV2DescriptorFields(&counts[3]);
  SdFinalizedHourV2PayloadFields(&counts[4]);
  for (size_t t = 0; t < 5; ++t) {
    for (size_t i = 0; i < counts[t]; ++i) {
      CHECK_CONTAINS(p, tables[t][i].name);
      CHECK_CONTAINS(p, tables[t][i].type);
      CHECK_CONTAINS(p, "bytes=");
    }
  }
}

void TestPreambleIncludesCrc32IsoHdlcParameters() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "CRC-32/ISO-HDLC");
  CHECK_CONTAINS(p, "0x04C11DB7");
  CHECK_CONTAINS(p, "0xEDB88320");
  CHECK_CONTAINS(p, "0xFFFFFFFF");
  CHECK_CONTAINS(p, "0xCBF43926");
}
void TestPreambleIncludesCrcCoverageRules() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "header_crc32 covers");
  CHECK_CONTAINS(p, "payload_crc32 covers");
  CHECK_CONTAINS(p, "block_crc32 covers");
}
void TestPreambleIncludesLabelRules() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "node max 32");
  CHECK_CONTAINS(p, "sensor max 48");
  CHECK_CONTAINS(p, "truncated");
}
void TestPreambleIncludesSampleEncodingAndBitmapRules() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "presence bitmap");
  CHECK_CONTAINS(p, "corrected bitmap");
  CHECK_CONTAINS(p, "int16 centi-C");
  CHECK_CONTAINS(p, "kHistoryInvalidTempCentiC");
}
void TestPreambleIncludesValidityRecoveryPrinciples() {
  const std::string p = Preamble();
  CHECK_CONTAINS(p, "duplicate ROM64");
  CHECK_CONTAINS(p, "sorted by ROM64");
  CHECK_CONTAINS(p, "zero-sensor hour records skipped");
  CHECK_CONTAINS(p, "bad block CRC invalidates");
  CHECK_CONTAINS(p, "no_v1_compatibility=true");
  CHECK_CONTAINS(p, "no durable slot_id");
  CHECK_CONTAINS(p, "no stored addr16");
}
void TestPreambleGeneratedFromFieldTables() { TestPreambleIncludesEveryFieldNameTypeAndByteLength(); }

void TestLittleEndianPutReadU8U16U32U64I16() {
  uint8_t b[32] = {};
  size_t o = 0;
  CHECK_TRUE(SdFinalizedHourV2PutU8(b, sizeof(b), &o, 0x12));
  CHECK_TRUE(SdFinalizedHourV2PutU16Le(b, sizeof(b), &o, 0x3456));
  CHECK_TRUE(SdFinalizedHourV2PutU32Le(b, sizeof(b), &o, 0x789ABCDE));
  CHECK_TRUE(SdFinalizedHourV2PutU64Le(b, sizeof(b), &o, 0x0102030405060708ULL));
  CHECK_TRUE(SdFinalizedHourV2PutI16Le(b, sizeof(b), &o, static_cast<int16_t>(-1234)));
  o = 0;
  uint8_t u8 = 0; uint16_t u16 = 0; uint32_t u32 = 0; uint64_t u64 = 0; int16_t i16 = 0;
  CHECK_TRUE(SdFinalizedHourV2ReadU8(b, sizeof(b), &o, &u8));
  CHECK_TRUE(SdFinalizedHourV2ReadU16Le(b, sizeof(b), &o, &u16));
  CHECK_TRUE(SdFinalizedHourV2ReadU32Le(b, sizeof(b), &o, &u32));
  CHECK_TRUE(SdFinalizedHourV2ReadU64Le(b, sizeof(b), &o, &u64));
  CHECK_TRUE(SdFinalizedHourV2ReadI16Le(b, sizeof(b), &o, &i16));
  CHECK_EQ(u8, static_cast<uint8_t>(0x12)); CHECK_EQ(u16, static_cast<uint16_t>(0x3456));
  CHECK_EQ(u32, 0x789ABCDEu); CHECK_EQ(u64, 0x0102030405060708ULL); CHECK_EQ(i16, static_cast<int16_t>(-1234));
}

void TestEncodeDecodeHourRecordHeaderV2RoundTrip() {
  SdFinalizedHourV2Header in; in.record_bytes = 1234; in.hour_start_epoch_minute = 55; in.sensor_count = 3; in.index_bytes = 36; in.sensor_blocks_offset = 84; in.sensor_blocks_bytes = 840; in.payload_crc32 = 0xAABBCCDD; in.header_crc32 = 0x11223344; in.flags = 9;
  uint8_t b[kSdFinalizedHourV2HeaderBytes] = {}; SdFinalizedHourV2Header out;
  CHECK_TRUE(EncodeSdFinalizedHourV2Header(in, b, sizeof(b))); CHECK_TRUE(DecodeSdFinalizedHourV2Header(b, sizeof(b), &out));
  CHECK_EQ(out.record_magic, in.record_magic); CHECK_EQ(out.record_version, in.record_version); CHECK_EQ(out.sensor_count, in.sensor_count); CHECK_EQ(out.header_crc32, in.header_crc32);
}
void TestEncodeDecodeSensorIndexEntryV2RoundTrip() {
  SdFinalizedHourV2IndexEntry in{0x0123456789ABCDEFULL, 88}; uint8_t b[kSdFinalizedHourV2IndexEntryBytes] = {}; SdFinalizedHourV2IndexEntry out;
  CHECK_TRUE(EncodeSdFinalizedHourV2IndexEntry(in, b, sizeof(b))); CHECK_TRUE(DecodeSdFinalizedHourV2IndexEntry(b, sizeof(b), &out)); CHECK_EQ(out.rom64, in.rom64); CHECK_EQ(out.sensor_block_offset_from_record_start, in.sensor_block_offset_from_record_start);
}
void TestEncodeDecodeSensorBlockHeaderV2RoundTrip() {
  SdFinalizedHourV2BlockHeader in; in.block_crc32 = 0x99887766; in.flags = 4; uint8_t b[kSdFinalizedHourV2BlockHeaderBytes] = {}; SdFinalizedHourV2BlockHeader out;
  CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(in, b, sizeof(b))); CHECK_TRUE(DecodeSdFinalizedHourV2BlockHeader(b, sizeof(b), &out)); CHECK_EQ(out.block_magic, in.block_magic); CHECK_EQ(out.block_crc32, in.block_crc32);
}
void TestEncodeDecodeSensorDescriptorV2RoundTrip() {
  SdFinalizedHourV2Descriptor in; in.rom64 = 0x28; in.last_known_node_id = 7; in.node_label_len = 4; in.sensor_label_len = 6; in.descriptor_flags = kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated; std::memcpy(in.node_label, "Node", 4); std::memcpy(in.sensor_label, "Sensor", 6);
  uint8_t b[kSdFinalizedHourV2DescriptorBytes] = {}; SdFinalizedHourV2Descriptor out;
  CHECK_TRUE(EncodeSdFinalizedHourV2Descriptor(in, b, sizeof(b))); CHECK_TRUE(DecodeSdFinalizedHourV2Descriptor(b, sizeof(b), &out)); CHECK_EQ(out.rom64, in.rom64); CHECK_EQ(out.node_label_len, in.node_label_len); CHECK_TRUE(std::memcmp(out.sensor_label, "Sensor", 6) == 0);
}
void TestEncodeDecodeSensorPayloadV2RoundTrip() {
  SdFinalizedHourV2Payload in; in.presence_bitmap[0] = 3; in.corrected_bitmap[0] = 2; in.samples[0] = 123; in.samples[1] = -456; FillMissingSdFinalizedHourV2Samples(&in);
  uint8_t b[kSdFinalizedHourV2PayloadBytes] = {}; SdFinalizedHourV2Payload out;
  CHECK_TRUE(EncodeSdFinalizedHourV2Payload(in, b, sizeof(b))); CHECK_TRUE(DecodeSdFinalizedHourV2Payload(b, sizeof(b), &out)); CHECK_EQ(out.presence_bitmap[0], in.presence_bitmap[0]); CHECK_EQ(out.samples[2], kHistoryInvalidTempCentiC);
}
void TestDescriptorLabelTruncationFlagsExist() {
  CHECK_TRUE(kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated != 0U);
  CHECK_TRUE(kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated != 0U);
  CHECK_TRUE(kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated != kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated);
}
void TestMissingSamplesUseHistoryInvalidTempCentiC() {
  SdFinalizedHourV2Payload p; p.presence_bitmap[0] = 1; p.samples[0] = 2500; FillMissingSdFinalizedHourV2Samples(&p); CHECK_EQ(p.samples[0], static_cast<int16_t>(2500)); CHECK_EQ(p.samples[1], kHistoryInvalidTempCentiC);
}
void TestCorrectedWithoutPresenceIsInvalid() {
  SdFinalizedHourV2Payload p; p.corrected_bitmap[0] = 1; CHECK_TRUE(!SdFinalizedHourV2PayloadBitmapsAreValid(p)); p.presence_bitmap[0] = 1; CHECK_TRUE(SdFinalizedHourV2PayloadBitmapsAreValid(p));
}
void TestHeaderCrcZerosHeaderCrcField() {
  SdFinalizedHourV2Header h; h.record_bytes = 99; h.header_crc32 = 0x11111111; uint8_t a[kSdFinalizedHourV2HeaderBytes] = {}; uint8_t b[kSdFinalizedHourV2HeaderBytes] = {}; CHECK_TRUE(EncodeSdFinalizedHourV2Header(h, a, sizeof(a))); h.header_crc32 = 0x22222222; CHECK_TRUE(EncodeSdFinalizedHourV2Header(h, b, sizeof(b))); CHECK_EQ(ComputeSdFinalizedHourV2HeaderCrc32(a, sizeof(a)), ComputeSdFinalizedHourV2HeaderCrc32(b, sizeof(b)));
}
void TestBlockCrcZerosBlockCrcField() {
  SdFinalizedHourV2BlockHeader h; uint8_t a[kSdFinalizedHourV2FixedBlockBytes] = {}; uint8_t b[kSdFinalizedHourV2FixedBlockBytes] = {}; h.block_crc32 = 1; CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(h, a, kSdFinalizedHourV2BlockHeaderBytes)); h.block_crc32 = 2; CHECK_TRUE(EncodeSdFinalizedHourV2BlockHeader(h, b, kSdFinalizedHourV2BlockHeaderBytes)); CHECK_EQ(ComputeSdFinalizedHourV2BlockCrc32(a, sizeof(a)), ComputeSdFinalizedHourV2BlockCrc32(b, sizeof(b)));
}
void TestPayloadCrcCoversIndexAndBlocksOnly() {
  uint8_t bytes[5] = {1,2,3,4,5}; CHECK_EQ(ComputeSdFinalizedHourV2PayloadCrc32(bytes, sizeof(bytes)), Crc32IsoHdlc(bytes, sizeof(bytes))); bytes[4] = 6; CHECK_TRUE(ComputeSdFinalizedHourV2PayloadCrc32(bytes, sizeof(bytes)) != Crc32IsoHdlc(reinterpret_cast<const uint8_t*>("\x01\x02\x03\x04\x05"), 5));
}

void Run(const char* name, void (*fn)()) { g_test.current_test = name; fn(); }

}  // namespace

int main() {
  Run("TestCrc32IsoHdlcCheckValue", TestCrc32IsoHdlcCheckValue);
  Run("TestV2MagicBytes", TestV2MagicBytes);
  Run("TestBinaryStartMarker", TestBinaryStartMarker);
  Run("TestV2FieldSizes", TestV2FieldSizes);
  Run("TestV2FieldOffsets", TestV2FieldOffsets);
  Run("TestV2SchemaFieldTablesContainEveryApprovedField", TestV2SchemaFieldTablesContainEveryApprovedField);
  Run("TestV2SchemaDoesNotContainDurableSlotId", TestV2SchemaDoesNotContainDurableSlotId);
  Run("TestV2SchemaDoesNotContainStoredAddr16", TestV2SchemaDoesNotContainStoredAddr16);
  Run("TestPreambleIncludesFormatAndMarker", TestPreambleIncludesFormatAndMarker);
  Run("TestPreambleIncludesEndian", TestPreambleIncludesEndian);
  Run("TestPreambleIncludesEveryFieldNameTypeAndByteLength", TestPreambleIncludesEveryFieldNameTypeAndByteLength);
  Run("TestPreambleIncludesCrc32IsoHdlcParameters", TestPreambleIncludesCrc32IsoHdlcParameters);
  Run("TestPreambleIncludesCrcCoverageRules", TestPreambleIncludesCrcCoverageRules);
  Run("TestPreambleIncludesLabelRules", TestPreambleIncludesLabelRules);
  Run("TestPreambleIncludesSampleEncodingAndBitmapRules", TestPreambleIncludesSampleEncodingAndBitmapRules);
  Run("TestPreambleIncludesValidityRecoveryPrinciples", TestPreambleIncludesValidityRecoveryPrinciples);
  Run("TestPreambleGeneratedFromFieldTables", TestPreambleGeneratedFromFieldTables);
  Run("TestLittleEndianPutReadU8U16U32U64I16", TestLittleEndianPutReadU8U16U32U64I16);
  Run("TestEncodeDecodeHourRecordHeaderV2RoundTrip", TestEncodeDecodeHourRecordHeaderV2RoundTrip);
  Run("TestEncodeDecodeSensorIndexEntryV2RoundTrip", TestEncodeDecodeSensorIndexEntryV2RoundTrip);
  Run("TestEncodeDecodeSensorBlockHeaderV2RoundTrip", TestEncodeDecodeSensorBlockHeaderV2RoundTrip);
  Run("TestEncodeDecodeSensorDescriptorV2RoundTrip", TestEncodeDecodeSensorDescriptorV2RoundTrip);
  Run("TestEncodeDecodeSensorPayloadV2RoundTrip", TestEncodeDecodeSensorPayloadV2RoundTrip);
  Run("TestDescriptorLabelTruncationFlagsExist", TestDescriptorLabelTruncationFlagsExist);
  Run("TestMissingSamplesUseHistoryInvalidTempCentiC", TestMissingSamplesUseHistoryInvalidTempCentiC);
  Run("TestCorrectedWithoutPresenceIsInvalid", TestCorrectedWithoutPresenceIsInvalid);
  Run("TestHeaderCrcZerosHeaderCrcField", TestHeaderCrcZerosHeaderCrcField);
  Run("TestBlockCrcZerosBlockCrcField", TestBlockCrcZerosBlockCrcField);
  Run("TestPayloadCrcCoversIndexAndBlocksOnly", TestPayloadCrcCoversIndexAndBlocksOnly);
  if (g_test.failures != 0U) {
    std::cerr << g_test.failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "sd_finalized_hour_v2_format_test passed" << std::endl;
  return 0;
}
