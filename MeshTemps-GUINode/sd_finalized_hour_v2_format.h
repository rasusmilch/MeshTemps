#ifndef SD_FINALIZED_HOUR_V2_FORMAT_H_
#define SD_FINALIZED_HOUR_V2_FORMAT_H_

#include <cstddef>
#include <cstdint>

#include "history_crc.h"
#include "history_hour_stager.h"

constexpr uint32_t kSdFinalizedHourV2RecordMagic = 0x3248544Du;  // MTH2
constexpr uint16_t kSdFinalizedHourV2RecordVersion = 2;
constexpr uint32_t kSdFinalizedHourV2BlockMagic = 0x3242534Du;  // MSB2
constexpr uint16_t kSdFinalizedHourV2BlockVersion = 2;
constexpr char kSdFinalizedHourV2BinaryStartMarker[] =
    "%%MESH_TEMPS_BINARY_START%%\n";

constexpr uint16_t kSdFinalizedHourV2HeaderBytes = 48;
constexpr uint16_t kSdFinalizedHourV2IndexEntryBytes = 12;
constexpr uint16_t kSdFinalizedHourV2BlockHeaderBytes = 36;
constexpr uint16_t kSdFinalizedHourV2DescriptorBytes = 108;
constexpr uint16_t kSdFinalizedHourV2PayloadBytes = 136;
constexpr uint16_t kSdFinalizedHourV2FixedBlockBytes =
    kSdFinalizedHourV2BlockHeaderBytes + kSdFinalizedHourV2DescriptorBytes +
    kSdFinalizedHourV2PayloadBytes;
constexpr uint16_t kSdFinalizedHourV2BitmapBytes = 8;
constexpr uint16_t kSdFinalizedHourV2SampleCount = 60;
constexpr uint16_t kSdFinalizedHourV2SampleBytes = 2;
constexpr uint16_t kSdFinalizedHourV2SampleEncodingInt16CentiCLe = 1;
constexpr uint8_t kSdFinalizedHourV2NodeLabelMaxBytes = 32;
constexpr uint8_t kSdFinalizedHourV2SensorLabelMaxBytes = 48;
constexpr uint32_t kSdFinalizedHourV2DescriptorFlagNodeLabelTruncated = 1U << 0;
constexpr uint32_t kSdFinalizedHourV2DescriptorFlagSensorLabelTruncated = 1U << 1;
constexpr size_t kSdFinalizedHourV2PreambleMaxBytes = 8192;

constexpr uint32_t kCrc32IsoHdlcPolynomial = 0x04C11DB7u;
constexpr uint32_t kCrc32IsoHdlcReflectedPolynomial = 0xEDB88320u;
constexpr uint32_t kCrc32IsoHdlcInitialValue = 0xFFFFFFFFu;
constexpr uint32_t kCrc32IsoHdlcFinalXor = 0xFFFFFFFFu;
constexpr uint32_t kCrc32IsoHdlcCheckValue = 0xCBF43926u;

struct SdFinalizedHourV2FieldSpec {
  const char* name;
  const char* type;
  uint16_t offset;
  uint16_t bytes;
  const char* description;
};

struct SdFinalizedHourV2Header {
  uint32_t record_magic = kSdFinalizedHourV2RecordMagic;
  uint16_t record_version = kSdFinalizedHourV2RecordVersion;
  uint16_t header_bytes = kSdFinalizedHourV2HeaderBytes;
  uint32_t record_bytes = 0;
  uint32_t hour_start_epoch_minute = 0;
  uint16_t sensor_count = 0;
  uint16_t index_entry_bytes = kSdFinalizedHourV2IndexEntryBytes;
  uint32_t index_offset = kSdFinalizedHourV2HeaderBytes;
  uint32_t index_bytes = 0;
  uint32_t sensor_blocks_offset = kSdFinalizedHourV2HeaderBytes;
  uint32_t sensor_blocks_bytes = 0;
  uint32_t payload_crc32 = 0;
  uint32_t header_crc32 = 0;
  uint32_t flags = 0;
};

struct SdFinalizedHourV2IndexEntry {
  uint64_t rom64 = 0;
  uint32_t sensor_block_offset_from_record_start = 0;
};

struct SdFinalizedHourV2BlockHeader {
  uint32_t block_magic = kSdFinalizedHourV2BlockMagic;
  uint16_t block_version = kSdFinalizedHourV2BlockVersion;
  uint16_t block_header_bytes = kSdFinalizedHourV2BlockHeaderBytes;
  uint32_t block_bytes = kSdFinalizedHourV2FixedBlockBytes;
  uint16_t descriptor_bytes = kSdFinalizedHourV2DescriptorBytes;
  uint16_t payload_bytes = kSdFinalizedHourV2PayloadBytes;
  uint16_t bitmap_bytes = kSdFinalizedHourV2BitmapBytes;
  uint16_t sample_count = kSdFinalizedHourV2SampleCount;
  uint16_t sample_bytes = kSdFinalizedHourV2SampleBytes;
  uint16_t sample_encoding = kSdFinalizedHourV2SampleEncodingInt16CentiCLe;
  uint32_t block_crc32 = 0;
  uint32_t flags = 0;
};

struct SdFinalizedHourV2Descriptor {
  uint64_t rom64 = 0;
  uint32_t last_known_node_id = 0;
  uint8_t first_seen_minute = 0;
  uint8_t last_seen_minute = 0;
  uint16_t valid_sample_count = 0;
  uint16_t missing_or_invalid_count = 0;
  uint16_t corrected_sample_count = 0;
  uint8_t node_label_len = 0;
  uint8_t sensor_label_len = 0;
  uint32_t descriptor_flags = 0;
  uint8_t node_label[kSdFinalizedHourV2NodeLabelMaxBytes] = {};
  uint8_t sensor_label[kSdFinalizedHourV2SensorLabelMaxBytes] = {};
};

struct SdFinalizedHourV2Payload {
  uint8_t presence_bitmap[kSdFinalizedHourV2BitmapBytes] = {};
  uint8_t corrected_bitmap[kSdFinalizedHourV2BitmapBytes] = {};
  int16_t samples[kSdFinalizedHourV2SampleCount] = {};
};

const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2HeaderFields(size_t* count);
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2IndexEntryFields(size_t* count);
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2BlockHeaderFields(size_t* count);
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2DescriptorFields(size_t* count);
const SdFinalizedHourV2FieldSpec* SdFinalizedHourV2PayloadFields(size_t* count);


bool SdFinalizedHourV2PutU8(uint8_t* out, size_t len, size_t* offset, uint8_t value);
bool SdFinalizedHourV2PutU16Le(uint8_t* out, size_t len, size_t* offset, uint16_t value);
bool SdFinalizedHourV2PutU32Le(uint8_t* out, size_t len, size_t* offset, uint32_t value);
bool SdFinalizedHourV2PutU64Le(uint8_t* out, size_t len, size_t* offset, uint64_t value);
bool SdFinalizedHourV2PutI16Le(uint8_t* out, size_t len, size_t* offset, int16_t value);
bool SdFinalizedHourV2ReadU8(const uint8_t* data, size_t len, size_t* offset, uint8_t* value);
bool SdFinalizedHourV2ReadU16Le(const uint8_t* data, size_t len, size_t* offset, uint16_t* value);
bool SdFinalizedHourV2ReadU32Le(const uint8_t* data, size_t len, size_t* offset, uint32_t* value);
bool SdFinalizedHourV2ReadU64Le(const uint8_t* data, size_t len, size_t* offset, uint64_t* value);
bool SdFinalizedHourV2ReadI16Le(const uint8_t* data, size_t len, size_t* offset, int16_t* value);

bool FormatSdFinalizedHourV2Rom64Addr16(uint64_t rom64, char out_addr16[17]);

bool EncodeSdFinalizedHourV2Header(const SdFinalizedHourV2Header& in, uint8_t* out, size_t len);
bool DecodeSdFinalizedHourV2Header(const uint8_t* data, size_t len, SdFinalizedHourV2Header* out);
bool EncodeSdFinalizedHourV2IndexEntry(const SdFinalizedHourV2IndexEntry& in, uint8_t* out, size_t len);
bool DecodeSdFinalizedHourV2IndexEntry(const uint8_t* data, size_t len, SdFinalizedHourV2IndexEntry* out);
bool EncodeSdFinalizedHourV2BlockHeader(const SdFinalizedHourV2BlockHeader& in, uint8_t* out, size_t len);
bool DecodeSdFinalizedHourV2BlockHeader(const uint8_t* data, size_t len, SdFinalizedHourV2BlockHeader* out);
bool EncodeSdFinalizedHourV2Descriptor(const SdFinalizedHourV2Descriptor& in, uint8_t* out, size_t len);
bool DecodeSdFinalizedHourV2Descriptor(const uint8_t* data, size_t len, SdFinalizedHourV2Descriptor* out);
bool EncodeSdFinalizedHourV2Payload(const SdFinalizedHourV2Payload& in, uint8_t* out, size_t len);
bool DecodeSdFinalizedHourV2Payload(const uint8_t* data, size_t len, SdFinalizedHourV2Payload* out);

void FillMissingSdFinalizedHourV2Samples(SdFinalizedHourV2Payload* payload);
bool SdFinalizedHourV2PayloadBitmapsAreValid(const SdFinalizedHourV2Payload& payload);

uint32_t ComputeSdFinalizedHourV2HeaderCrc32(const uint8_t* header_bytes, size_t len);
uint32_t ComputeSdFinalizedHourV2BlockCrc32(const uint8_t* block_bytes, size_t len);
uint32_t ComputeSdFinalizedHourV2PayloadCrc32(const uint8_t* payload_bytes, size_t len);

size_t BuildSdFinalizedHourV2Preamble(char* out, size_t len);

#endif  // SD_FINALIZED_HOUR_V2_FORMAT_H_
