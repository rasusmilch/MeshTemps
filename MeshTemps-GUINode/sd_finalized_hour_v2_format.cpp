#include "sd_finalized_hour_v2_format.h"

#include <cstring>

#include "history_crc.h"

namespace {

constexpr uint16_t kHeaderCrcOffset = 40;
constexpr uint16_t kBlockCrcOffset = 24;

const SdFinalizedHourV2FieldSpec kHeaderFields[] = {
    {"record_magic", "u32", 0, 4, "MTH2 record magic"},
    {"record_version", "u16", 4, 2, "HourRecordV2 version"},
    {"header_bytes", "u16", 6, 2, "header byte length"},
    {"record_bytes", "u32", 8, 4, "total record byte length"},
    {"hour_start_epoch_minute", "u32", 12, 4, "UTC epoch minute at hour start"},
    {"sensor_count", "u16", 16, 2, "sensor block count"},
    {"index_entry_bytes", "u16", 18, 2, "index entry byte length"},
    {"index_offset", "u32", 20, 4, "index offset from record start"},
    {"index_bytes", "u32", 24, 4, "index table byte length"},
    {"sensor_blocks_offset", "u32", 28, 4, "sensor blocks offset from record start"},
    {"sensor_blocks_bytes", "u32", 32, 4, "sensor blocks byte length"},
    {"payload_crc32", "u32", 36, 4, "CRC over index table and sensor blocks"},
    {"header_crc32", "u32", 40, 4, "CRC over header with this field zeroed"},
    {"flags", "u32", 44, 4, "record flags"},
};

const SdFinalizedHourV2FieldSpec kIndexFields[] = {
    {"rom64", "u64", 0, 8, "canonical DS18B20 ROM identity"},
    {"sensor_block_offset_from_record_start", "u32", 8, 4, "sensor block offset"},
};

const SdFinalizedHourV2FieldSpec kBlockHeaderFields[] = {
    {"block_magic", "u32", 0, 4, "MSB2 block magic at known offset"},
    {"block_version", "u16", 4, 2, "SensorBlockV2 version"},
    {"block_header_bytes", "u16", 6, 2, "block header byte length"},
    {"block_bytes", "u32", 8, 4, "total sensor block byte length"},
    {"descriptor_bytes", "u16", 12, 2, "descriptor byte length"},
    {"payload_bytes", "u16", 14, 2, "payload byte length"},
    {"bitmap_bytes", "u16", 16, 2, "presence/corrected bitmap byte length"},
    {"sample_count", "u16", 18, 2, "minute sample count"},
    {"sample_bytes", "u16", 20, 2, "bytes per sample"},
    {"sample_encoding", "u16", 22, 2, "int16 centi-C little-endian encoding"},
    {"block_crc32", "u32", 24, 4, "CRC over block with this field zeroed"},
    {"flags", "u32", 28, 4, "block flags"},
};

const SdFinalizedHourV2FieldSpec kDescriptorFields[] = {
    {"rom64", "u64", 0, 8, "canonical DS18B20 ROM identity"},
    {"last_known_node_id", "u32", 8, 4, "reporting node provenance"},
    {"first_seen_minute", "u8", 12, 1, "first minute seen"},
    {"last_seen_minute", "u8", 13, 1, "last minute seen"},
    {"valid_sample_count", "u16", 14, 2, "presence bitmap sample count"},
    {"missing_or_invalid_count", "u16", 16, 2, "missing or invalid minute count"},
    {"corrected_sample_count", "u16", 18, 2, "corrected valid sample count"},
    {"node_label_len", "u8", 20, 1, "node label byte length"},
    {"sensor_label_len", "u8", 21, 1, "sensor label byte length"},
    {"descriptor_flags", "u32", 22, 4, "descriptor flags including truncation"},
    {"node_label", "u8[32]", 26, 32, "UTF-8-compatible node label bytes"},
    {"sensor_label", "u8[48]", 58, 48, "UTF-8-compatible sensor label bytes"},
};

const SdFinalizedHourV2FieldSpec kPayloadFields[] = {
    {"presence_bitmap", "u8[8]", 0, 8, "valid-minute bitmap"},
    {"corrected_bitmap", "u8[8]", 8, 8, "corrected valid-minute bitmap"},
    {"samples", "i16[60]", 16, 120, "int16 centi-C samples"},
};

template <size_t N>
const SdFinalizedHourV2FieldSpec* Fields(const SdFinalizedHourV2FieldSpec (&fields)[N],
                                         size_t* count) {
  if (count != nullptr) *count = N;
  return fields;
}

bool AppendChar(char* out, size_t len, size_t* offset, char ch) {
  if (offset == nullptr) return false;
  if (out != nullptr && *offset + 1U < len) out[*offset] = ch;
  ++(*offset);
  return true;
}

bool AppendLiteral(char* out, size_t len, size_t* offset, const char* text) {
  if (text == nullptr) return false;
  while (*text != '\0') {
    if (!AppendChar(out, len, offset, *text++)) return false;
  }
  return true;
}

bool AppendUnsigned(char* out, size_t len, size_t* offset, uint32_t value) {
  char digits[10];
  size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + (value % 10U));
    value /= 10U;
  } while (value != 0U && count < sizeof(digits));
  while (count > 0U) {
    if (!AppendChar(out, len, offset, digits[--count])) return false;
  }
  return true;
}

bool AppendHex8(char* out, size_t len, size_t* offset, uint32_t value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (!AppendLiteral(out, len, offset, "0x")) return false;
  for (int shift = 28; shift >= 0; shift -= 4) {
    if (!AppendChar(out, len, offset, kHex[(value >> shift) & 0xFU])) return false;
  }
  return true;
}

bool AppendFieldTable(char* out,
                      size_t len,
                      size_t* offset,
                      const char* title,
                      const SdFinalizedHourV2FieldSpec* fields,
                      size_t count) {
  if (!AppendLiteral(out, len, offset, title) || !AppendChar(out, len, offset, '\n')) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!AppendLiteral(out, len, offset, "- ") ||
        !AppendLiteral(out, len, offset, fields[i].name) ||
        !AppendLiteral(out, len, offset, " type=") ||
        !AppendLiteral(out, len, offset, fields[i].type) ||
        !AppendLiteral(out, len, offset, " offset=") ||
        !AppendUnsigned(out, len, offset, fields[i].offset) ||
        !AppendLiteral(out, len, offset, " bytes=") ||
        !AppendUnsigned(out, len, offset, fields[i].bytes) ||
        !AppendLiteral(out, len, offset, " - ") ||
        !AppendLiteral(out, len, offset, fields[i].description) ||
        !AppendChar(out, len, offset, '\n')) {
      return false;
    }
  }
  return true;
}

uint32_t CrcWithZeroedField(const uint8_t* bytes,
                            size_t len,
                            size_t field_offset) {
  if (bytes == nullptr || field_offset + sizeof(uint32_t) > len) return 0U;
  uint32_t state = Crc32IsoHdlcBegin();
  state = Crc32IsoHdlcUpdate(state, bytes, field_offset);
  const uint8_t zeros[4] = {};
  state = Crc32IsoHdlcUpdate(state, zeros, sizeof(zeros));
  state = Crc32IsoHdlcUpdate(state, bytes + field_offset + sizeof(uint32_t),
                             len - field_offset - sizeof(uint32_t));
  return Crc32IsoHdlcFinalize(state);
}

}  // namespace

const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2HeaderFields(size_t* count) {
  return Fields(kHeaderFields, count);
}
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2IndexEntryFields(size_t* count) {
  return Fields(kIndexFields, count);
}
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2BlockHeaderFields(size_t* count) {
  return Fields(kBlockHeaderFields, count);
}
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2DescriptorFields(size_t* count) {
  return Fields(kDescriptorFields, count);
}
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2PayloadFields(size_t* count) {
  return Fields(kPayloadFields, count);
}

bool SdFinalizedHourV2PutU8(uint8_t* out, size_t len, size_t* offset, uint8_t value) {
  if (out == nullptr || offset == nullptr || *offset + 1U > len) return false;
  out[(*offset)++] = value;
  return true;
}

bool SdFinalizedHourV2PutU16Le(uint8_t* out, size_t len, size_t* offset, uint16_t value) {
  return SdFinalizedHourV2PutU8(out, len, offset, static_cast<uint8_t>(value & 0xFFU)) &&
         SdFinalizedHourV2PutU8(out, len, offset, static_cast<uint8_t>((value >> 8U) & 0xFFU));
}

bool SdFinalizedHourV2PutU32Le(uint8_t* out, size_t len, size_t* offset, uint32_t value) {
  for (uint8_t i = 0; i < 4U; ++i) {
    if (!SdFinalizedHourV2PutU8(out, len, offset,
                                static_cast<uint8_t>((value >> (8U * i)) & 0xFFU))) {
      return false;
    }
  }
  return true;
}

bool SdFinalizedHourV2PutU64Le(uint8_t* out, size_t len, size_t* offset, uint64_t value) {
  for (uint8_t i = 0; i < 8U; ++i) {
    if (!SdFinalizedHourV2PutU8(out, len, offset,
                                static_cast<uint8_t>((value >> (8U * i)) & 0xFFU))) {
      return false;
    }
  }
  return true;
}

bool SdFinalizedHourV2PutI16Le(uint8_t* out, size_t len, size_t* offset, int16_t value) {
  return SdFinalizedHourV2PutU16Le(out, len, offset, static_cast<uint16_t>(value));
}

bool SdFinalizedHourV2ReadU8(const uint8_t* data, size_t len, size_t* offset, uint8_t* value) {
  if (data == nullptr || offset == nullptr || value == nullptr || *offset + 1U > len) {
    return false;
  }
  *value = data[(*offset)++];
  return true;
}

bool SdFinalizedHourV2ReadU16Le(const uint8_t* data, size_t len, size_t* offset, uint16_t* value) {
  uint8_t b0 = 0;
  uint8_t b1 = 0;
  if (!SdFinalizedHourV2ReadU8(data, len, offset, &b0) ||
      !SdFinalizedHourV2ReadU8(data, len, offset, &b1)) {
    return false;
  }
  *value = static_cast<uint16_t>(b0) | static_cast<uint16_t>(static_cast<uint16_t>(b1) << 8U);
  return true;
}

bool SdFinalizedHourV2ReadU32Le(const uint8_t* data, size_t len, size_t* offset, uint32_t* value) {
  if (value == nullptr) return false;
  uint32_t out = 0;
  for (uint8_t i = 0; i < 4U; ++i) {
    uint8_t byte = 0;
    if (!SdFinalizedHourV2ReadU8(data, len, offset, &byte)) return false;
    out |= static_cast<uint32_t>(byte) << (8U * i);
  }
  *value = out;
  return true;
}

bool SdFinalizedHourV2ReadU64Le(const uint8_t* data, size_t len, size_t* offset, uint64_t* value) {
  if (value == nullptr) return false;
  uint64_t out = 0;
  for (uint8_t i = 0; i < 8U; ++i) {
    uint8_t byte = 0;
    if (!SdFinalizedHourV2ReadU8(data, len, offset, &byte)) return false;
    out |= static_cast<uint64_t>(byte) << (8U * i);
  }
  *value = out;
  return true;
}

bool SdFinalizedHourV2ReadI16Le(const uint8_t* data, size_t len, size_t* offset, int16_t* value) {
  uint16_t raw = 0;
  if (!SdFinalizedHourV2ReadU16Le(data, len, offset, &raw) || value == nullptr) return false;
  *value = static_cast<int16_t>(raw);
  return true;
}

bool FormatSdFinalizedHourV2Rom64Addr16(uint64_t rom64, char out_addr16[17]) {
  if (out_addr16 == nullptr) return false;
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (int nibble = 15; nibble >= 0; --nibble) {
    out_addr16[15 - nibble] = kHex[(rom64 >> (4U * nibble)) & 0xFU];
  }
  out_addr16[16] = '\0';
  return true;
}

bool EncodeSdFinalizedHourV2Header(const SdFinalizedHourV2Header& in, uint8_t* out, size_t len) {
  size_t o = 0;
  return len >= kSdFinalizedHourV2HeaderBytes &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.record_magic) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.record_version) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.header_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.record_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.hour_start_epoch_minute) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.sensor_count) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.index_entry_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.index_offset) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.index_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.sensor_blocks_offset) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.sensor_blocks_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.payload_crc32) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.header_crc32) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.flags) &&
         o == kSdFinalizedHourV2HeaderBytes;
}

bool DecodeSdFinalizedHourV2Header(const uint8_t* data, size_t len, SdFinalizedHourV2Header* out) {
  if (out == nullptr || len < kSdFinalizedHourV2HeaderBytes) return false;
  size_t o = 0;
  return SdFinalizedHourV2ReadU32Le(data, len, &o, &out->record_magic) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->record_version) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->header_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->record_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->hour_start_epoch_minute) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->sensor_count) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->index_entry_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->index_offset) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->index_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->sensor_blocks_offset) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->sensor_blocks_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->payload_crc32) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->header_crc32) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->flags) &&
         o == kSdFinalizedHourV2HeaderBytes;
}

bool EncodeSdFinalizedHourV2IndexEntry(const SdFinalizedHourV2IndexEntry& in, uint8_t* out, size_t len) {
  size_t o = 0;
  return len >= kSdFinalizedHourV2IndexEntryBytes &&
         SdFinalizedHourV2PutU64Le(out, len, &o, in.rom64) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.sensor_block_offset_from_record_start) &&
         o == kSdFinalizedHourV2IndexEntryBytes;
}

bool DecodeSdFinalizedHourV2IndexEntry(const uint8_t* data, size_t len, SdFinalizedHourV2IndexEntry* out) {
  if (out == nullptr || len < kSdFinalizedHourV2IndexEntryBytes) return false;
  size_t o = 0;
  return SdFinalizedHourV2ReadU64Le(data, len, &o, &out->rom64) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->sensor_block_offset_from_record_start) &&
         o == kSdFinalizedHourV2IndexEntryBytes;
}

bool EncodeSdFinalizedHourV2BlockHeader(const SdFinalizedHourV2BlockHeader& in, uint8_t* out, size_t len) {
  size_t o = 0;
  return len >= kSdFinalizedHourV2BlockHeaderBytes &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.block_magic) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.block_version) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.block_header_bytes) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.block_bytes) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.descriptor_bytes) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.payload_bytes) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.bitmap_bytes) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.sample_count) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.sample_bytes) &&
         SdFinalizedHourV2PutU16Le(out, len, &o, in.sample_encoding) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.block_crc32) &&
         SdFinalizedHourV2PutU32Le(out, len, &o, in.flags) &&
         o == kSdFinalizedHourV2BlockHeaderBytes;
}

bool DecodeSdFinalizedHourV2BlockHeader(const uint8_t* data, size_t len, SdFinalizedHourV2BlockHeader* out) {
  if (out == nullptr || len < kSdFinalizedHourV2BlockHeaderBytes) return false;
  size_t o = 0;
  return SdFinalizedHourV2ReadU32Le(data, len, &o, &out->block_magic) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->block_version) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->block_header_bytes) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->block_bytes) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->descriptor_bytes) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->payload_bytes) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->bitmap_bytes) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->sample_count) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->sample_bytes) &&
         SdFinalizedHourV2ReadU16Le(data, len, &o, &out->sample_encoding) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->block_crc32) &&
         SdFinalizedHourV2ReadU32Le(data, len, &o, &out->flags) &&
         o == kSdFinalizedHourV2BlockHeaderBytes;
}

bool EncodeSdFinalizedHourV2Descriptor(const SdFinalizedHourV2Descriptor& in, uint8_t* out, size_t len) {
  if (in.node_label_len > kSdFinalizedHourV2NodeLabelMaxBytes ||
      in.sensor_label_len > kSdFinalizedHourV2SensorLabelMaxBytes) return false;
  size_t o = 0;
  if (!(len >= kSdFinalizedHourV2DescriptorBytes &&
        SdFinalizedHourV2PutU64Le(out, len, &o, in.rom64) &&
        SdFinalizedHourV2PutU32Le(out, len, &o, in.last_known_node_id) &&
        SdFinalizedHourV2PutU8(out, len, &o, in.first_seen_minute) &&
        SdFinalizedHourV2PutU8(out, len, &o, in.last_seen_minute) &&
        SdFinalizedHourV2PutU16Le(out, len, &o, in.valid_sample_count) &&
        SdFinalizedHourV2PutU16Le(out, len, &o, in.missing_or_invalid_count) &&
        SdFinalizedHourV2PutU16Le(out, len, &o, in.corrected_sample_count) &&
        SdFinalizedHourV2PutU8(out, len, &o, in.node_label_len) &&
        SdFinalizedHourV2PutU8(out, len, &o, in.sensor_label_len) &&
        SdFinalizedHourV2PutU32Le(out, len, &o, in.descriptor_flags))) return false;
  for (uint8_t value : in.node_label) if (!SdFinalizedHourV2PutU8(out, len, &o, value)) return false;
  for (uint8_t value : in.sensor_label) if (!SdFinalizedHourV2PutU8(out, len, &o, value)) return false;
  return o == kSdFinalizedHourV2DescriptorBytes;
}

bool DecodeSdFinalizedHourV2Descriptor(const uint8_t* data, size_t len, SdFinalizedHourV2Descriptor* out) {
  if (out == nullptr || len < kSdFinalizedHourV2DescriptorBytes) return false;
  size_t o = 0;
  if (!(SdFinalizedHourV2ReadU64Le(data, len, &o, &out->rom64) &&
        SdFinalizedHourV2ReadU32Le(data, len, &o, &out->last_known_node_id) &&
        SdFinalizedHourV2ReadU8(data, len, &o, &out->first_seen_minute) &&
        SdFinalizedHourV2ReadU8(data, len, &o, &out->last_seen_minute) &&
        SdFinalizedHourV2ReadU16Le(data, len, &o, &out->valid_sample_count) &&
        SdFinalizedHourV2ReadU16Le(data, len, &o, &out->missing_or_invalid_count) &&
        SdFinalizedHourV2ReadU16Le(data, len, &o, &out->corrected_sample_count) &&
        SdFinalizedHourV2ReadU8(data, len, &o, &out->node_label_len) &&
        SdFinalizedHourV2ReadU8(data, len, &o, &out->sensor_label_len) &&
        SdFinalizedHourV2ReadU32Le(data, len, &o, &out->descriptor_flags))) return false;
  if (out->node_label_len > kSdFinalizedHourV2NodeLabelMaxBytes ||
      out->sensor_label_len > kSdFinalizedHourV2SensorLabelMaxBytes) return false;
  for (uint8_t& value : out->node_label) if (!SdFinalizedHourV2ReadU8(data, len, &o, &value)) return false;
  for (uint8_t& value : out->sensor_label) if (!SdFinalizedHourV2ReadU8(data, len, &o, &value)) return false;
  return o == kSdFinalizedHourV2DescriptorBytes;
}

bool EncodeSdFinalizedHourV2Payload(const SdFinalizedHourV2Payload& in, uint8_t* out, size_t len) {
  size_t o = 0;
  if (len < kSdFinalizedHourV2PayloadBytes) return false;
  for (uint8_t value : in.presence_bitmap) if (!SdFinalizedHourV2PutU8(out, len, &o, value)) return false;
  for (uint8_t value : in.corrected_bitmap) if (!SdFinalizedHourV2PutU8(out, len, &o, value)) return false;
  for (int16_t value : in.samples) if (!SdFinalizedHourV2PutI16Le(out, len, &o, value)) return false;
  return o == kSdFinalizedHourV2PayloadBytes;
}

bool DecodeSdFinalizedHourV2Payload(const uint8_t* data, size_t len, SdFinalizedHourV2Payload* out) {
  if (out == nullptr || len < kSdFinalizedHourV2PayloadBytes) return false;
  size_t o = 0;
  for (uint8_t& value : out->presence_bitmap) if (!SdFinalizedHourV2ReadU8(data, len, &o, &value)) return false;
  for (uint8_t& value : out->corrected_bitmap) if (!SdFinalizedHourV2ReadU8(data, len, &o, &value)) return false;
  for (int16_t& value : out->samples) if (!SdFinalizedHourV2ReadI16Le(data, len, &o, &value)) return false;
  return o == kSdFinalizedHourV2PayloadBytes;
}

void FillMissingSdFinalizedHourV2Samples(SdFinalizedHourV2Payload* payload) {
  if (payload == nullptr) return;
  for (uint16_t i = 0; i < kSdFinalizedHourV2SampleCount; ++i) {
    const uint8_t mask = static_cast<uint8_t>(1U << (i % 8U));
    if ((payload->presence_bitmap[i / 8U] & mask) == 0U) {
      payload->samples[i] = kSdFinalizedHourV2InvalidTempCentiC;
    }
  }
}

bool SdFinalizedHourV2PayloadBitmapsAreValid(const SdFinalizedHourV2Payload& payload) {
  for (uint8_t i = 0; i < kSdFinalizedHourV2BitmapBytes; ++i) {
    if ((payload.corrected_bitmap[i] & static_cast<uint8_t>(~payload.presence_bitmap[i])) != 0U) {
      return false;
    }
  }
  return true;
}

uint32_t ComputeSdFinalizedHourV2HeaderCrc32(const uint8_t* header_bytes, size_t len) {
  return CrcWithZeroedField(header_bytes, len, kHeaderCrcOffset);
}

uint32_t ComputeSdFinalizedHourV2BlockCrc32(const uint8_t* block_bytes, size_t len) {
  return CrcWithZeroedField(block_bytes, len, kBlockCrcOffset);
}

uint32_t ComputeSdFinalizedHourV2PayloadCrc32(const uint8_t* payload_bytes, size_t len) {
  return Crc32IsoHdlc(payload_bytes, len);
}

size_t BuildSdFinalizedHourV2Preamble(char* out, size_t len) {
  size_t o = 0;
  AppendLiteral(out, len, &o, "MeshTemps finalized-hour v2 format\n");
  AppendLiteral(out, len, &o, "format_name=MeshTemps Finalized HourRecordV2\n");
  AppendLiteral(out, len, &o, "format_version=2\n");
  AppendLiteral(out, len, &o, "binary_start_marker=");
  AppendLiteral(out, len, &o, kSdFinalizedHourV2BinaryStartMarker);
  AppendLiteral(out, len, &o, "endian=little-endian for every binary integer field; raw compiler structs are not serialized\n");
  AppendLiteral(out, len, &o, "record_magic=MTH2 block_magic=MSB2 block magic checked only at known offsets; no sentinel hunting\n");
  AppendLiteral(out, len, &o, "no_v1_compatibility=true; v1 MTHR is not a production archive format\n");
  AppendLiteral(out, len, &o, "no durable slot_id; no stored addr16 by default; derive display addr16 from ROM64\n");
  AppendLiteral(out, len, &o, "CRC-32/ISO-HDLC parameters: aliases CRC-32 CRC-32/ADCCP PKZIP zlib gzip; width=32 polynomial=");
  AppendHex8(out, len, &o, kCrc32IsoHdlcPolynomial);
  AppendLiteral(out, len, &o, " reflected_polynomial=");
  AppendHex8(out, len, &o, kCrc32IsoHdlcReflectedPolynomial);
  AppendLiteral(out, len, &o, " init=");
  AppendHex8(out, len, &o, kCrc32IsoHdlcInitialValue);
  AppendLiteral(out, len, &o, " refin=true refout=true xorout=");
  AppendHex8(out, len, &o, kCrc32IsoHdlcFinalXor);
  AppendLiteral(out, len, &o, " check_123456789=");
  AppendHex8(out, len, &o, kCrc32IsoHdlcCheckValue);
  AppendLiteral(out, len, &o, " stored=little-endian-u32\n");
  AppendLiteral(out, len, &o, "CRC coverage: header_crc32 covers HourRecordHeaderV2 with header_crc32 zeroed; payload_crc32 covers SensorIndexTableV2 plus all SensorBlockV2 bytes excluding header; block_crc32 covers SensorBlockHeaderV2 with block_crc32 zeroed plus descriptor plus payload\n");
  AppendLiteral(out, len, &o, "label rules: UTF-8-compatible raw bytes with explicit lengths; node max 32; sensor max 48; not NUL-terminated for parsing; overlong labels truncated and descriptor flags set; labels are context not identity\n");
  AppendLiteral(out, len, &o, "size rules: HourRecordHeaderV2=48 bytes; SensorIndexEntryV2=12 bytes; SensorBlockHeaderV2=32 bytes; SensorDescriptorV2=106 bytes; SensorPayloadV2=136 bytes; fixed SensorBlockV2=274 bytes; block_crc32 offset=24; descriptor_flags offset=22; no reserved fields, no fake padding, no generic reserved bytes\n");
  AppendLiteral(out, len, &o, "sample rules: 8-byte presence bitmap; 8-byte corrected bitmap; 60 int16 centi-C little-endian samples; missing positions filled with kSdFinalizedHourV2InvalidTempCentiC; validity comes only from presence; corrected without presence is corrupt; v2 format owns ABI constants and does not depend on staging internals\n");
  AppendLiteral(out, len, &o, "validity rules: duplicate ROM64 index/block entries are corrupt; sensor blocks sorted by ROM64; zero-sensor hour records skipped; any bad required block structural check or bad block CRC invalidates the whole hour record\n");
  AppendFieldTable(out, len, &o, "HourRecordHeaderV2 fields:", kHeaderFields,
                   sizeof(kHeaderFields) / sizeof(kHeaderFields[0]));
  AppendFieldTable(out, len, &o, "SensorIndexEntryV2 fields:", kIndexFields,
                   sizeof(kIndexFields) / sizeof(kIndexFields[0]));
  AppendFieldTable(out, len, &o, "SensorBlockHeaderV2 fields:", kBlockHeaderFields,
                   sizeof(kBlockHeaderFields) / sizeof(kBlockHeaderFields[0]));
  AppendFieldTable(out, len, &o, "SensorDescriptorV2 fields:", kDescriptorFields,
                   sizeof(kDescriptorFields) / sizeof(kDescriptorFields[0]));
  AppendFieldTable(out, len, &o, "SensorPayloadV2 fields:", kPayloadFields,
                   sizeof(kPayloadFields) / sizeof(kPayloadFields[0]));
  if (out != nullptr && len != 0U) {
    out[(o < len) ? o : (len - 1U)] = '\0';
  }
  return o;
}
