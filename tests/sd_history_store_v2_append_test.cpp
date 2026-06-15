#include "sd_history_store.h"

#include <cstdint>
#include <cstring>
#include <iostream>

#include "sd_finalized_hour_v2_format.h"

namespace {

constexpr uint32_t kHour = 28928160U;
constexpr size_t kMaxFiles = 8;
constexpr size_t kMaxPath = 160;
constexpr size_t kMaxFileBytes = 65536;
constexpr const char* kBaseDir = "/hist";

struct TestHarness {
  const char* current_test = nullptr;
  uint32_t failures = 0;
};

TestHarness g_test;

size_t PreambleBytes();

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

struct FakeFileData {
  bool used = false;
  bool directory = false;
  char path[kMaxPath] = {};
  uint8_t bytes[kMaxFileBytes] = {};
  uint32_t size = 0;
};

class FakeFs final : public fs::FS {
 public:
  bool fail_next_write = false;
  bool mutate_header_after_append_close = false;
  bool mutate_payload_after_append_close = false;
  uint32_t append_close_count = 0;
  uint32_t read_seek_count = 0;
  uint32_t last_read_seek = 0;

  bool exists(const char* path) override { return Find(path) >= 0; }

  bool mkdir(const char* path) override {
    if (path == nullptr || path[0] == '\0') return false;
    int index = Find(path);
    if (index < 0) index = Allocate(path);
    if (index < 0) return false;
    files_[index].directory = true;
    return true;
  }

  File open(const char* path, const char* mode) override {
    if (path == nullptr || mode == nullptr) return File();
    const bool append_mode = std::strcmp(mode, FILE_APPEND) == 0;
    const bool read_mode = std::strcmp(mode, FILE_READ) == 0;
    if (!append_mode && !read_mode) return File();
    int index = Find(path);
    if (index < 0) {
      if (!append_mode) return File();
      index = Allocate(path);
      if (index < 0) return File();
    }
    return File(this, index, append_mode, read_mode);
  }

  uint32_t FileSize(int file_index) const override {
    return ValidIndex(file_index) ? files_[file_index].size : 0U;
  }

  size_t FileWrite(int file_index,
                   const uint8_t* data,
                   size_t len,
                   bool append_mode,
                   uint32_t* position) override {
    if (!ValidIndex(file_index) || data == nullptr || position == nullptr) return 0;
    if (fail_next_write) {
      fail_next_write = false;
      return 0;
    }
    FakeFileData& file = files_[file_index];
    uint32_t write_pos = append_mode ? file.size : *position;
    if (write_pos > kMaxFileBytes || len > kMaxFileBytes - write_pos) return 0;
    std::memcpy(file.bytes + write_pos, data, len);
    write_pos += static_cast<uint32_t>(len);
    if (write_pos > file.size) file.size = write_pos;
    *position = write_pos;
    return len;
  }

  int FileRead(int file_index,
               uint8_t* data,
               size_t len,
               uint32_t* position) override {
    if (!ValidIndex(file_index) || data == nullptr || position == nullptr) return -1;
    FakeFileData& file = files_[file_index];
    if (*position >= file.size) return 0;
    const uint32_t available = file.size - *position;
    const size_t to_read = (len < available) ? len : static_cast<size_t>(available);
    std::memcpy(data, file.bytes + *position, to_read);
    *position += static_cast<uint32_t>(to_read);
    return static_cast<int>(to_read);
  }

  void FileClosed(int file_index, bool append_mode, bool read_mode) override {
    (void)read_mode;
    if (!ValidIndex(file_index) || !append_mode) return;
    ++append_close_count;
    const size_t preamble_bytes = PreambleBytes();
    if (mutate_header_after_append_close && files_[file_index].size > preamble_bytes + 12U) {
      files_[file_index].bytes[preamble_bytes + 12U] ^= 0x01U;
      mutate_header_after_append_close = false;
    }
    if (mutate_payload_after_append_close &&
        files_[file_index].size > preamble_bytes + kSdFinalizedHourV2HeaderBytes) {
      files_[file_index].bytes[preamble_bytes + kSdFinalizedHourV2HeaderBytes] ^= 0x01U;
      mutate_payload_after_append_close = false;
    }
  }

  void FileSeek(int file_index, bool read_mode, uint32_t position) override {
    (void)file_index;
    if (!read_mode) return;
    ++read_seek_count;
    last_read_seek = position;
  }

  const FakeFileData* FileByPath(const char* path) const {
    const int index = Find(path);
    return (index >= 0) ? &files_[index] : nullptr;
  }

  FakeFileData* MutableFileByPath(const char* path) {
    const int index = Find(path);
    return (index >= 0) ? &files_[index] : nullptr;
  }

  bool SeedFile(const char* path, const uint8_t* data, size_t len) {
    int index = Find(path);
    if (index < 0) index = Allocate(path);
    if (index < 0 || len > kMaxFileBytes) return false;
    files_[index].directory = false;
    std::memcpy(files_[index].bytes, data, len);
    files_[index].size = static_cast<uint32_t>(len);
    return true;
  }

 private:
  FakeFileData files_[kMaxFiles] = {};

  bool ValidIndex(int index) const {
    return index >= 0 && static_cast<size_t>(index) < kMaxFiles && files_[index].used;
  }

  int Find(const char* path) const {
    if (path == nullptr) return -1;
    for (size_t i = 0; i < kMaxFiles; ++i) {
      if (files_[i].used && std::strcmp(files_[i].path, path) == 0) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int Allocate(const char* path) {
    for (size_t i = 0; i < kMaxFiles; ++i) {
      if (!files_[i].used) {
        files_[i] = FakeFileData{};
        files_[i].used = true;
        const size_t len = std::strlen(path);
        if (len >= kMaxPath) return -1;
        std::memcpy(files_[i].path, path, len + 1U);
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

}  // namespace

namespace {

size_t PreambleBytes() {
  return BuildSdFinalizedHourV2Preamble(nullptr, 0U);
}

bool BuildSnapshot(uint32_t hour, uint8_t minute, int16_t centi_c,
                   HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  RamHourStager stager;
  bool ok = stager.ResetHour(hour);
  ok = stager.RecordSampleCentiC("2800000000000060", 0x10000001U, minute,
                                 centi_c, false) && ok;
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}

bool BuildZeroSensorSnapshot(uint32_t hour, HistoryHourSnapshot* out) {
  if (out == nullptr) return false;
  RamHourStager stager;
  bool ok = stager.ResetHour(hour);
  ok = stager.ExportSnapshot(out) && ok;
  return ok;
}

bool BuildPath(uint32_t hour, char* out, size_t out_size) {
  return SdHistoryBuildFinalizedHourFilePath(hour, kBaseDir, out, out_size);
}

const FakeFileData* FinalizedFile(const FakeFs& fs, uint32_t hour) {
  char path[kSdHistoryPathMax];
  if (!BuildPath(hour, path, sizeof(path))) return nullptr;
  return fs.FileByPath(path);
}

uint32_t CountMarker(const FakeFileData& file) {
  const char* marker = kSdFinalizedHourV2BinaryStartMarker;
  const size_t marker_len = sizeof(kSdFinalizedHourV2BinaryStartMarker) - 1U;
  uint32_t count = 0;
  for (uint32_t i = 0; i + marker_len <= file.size; ++i) {
    if (std::memcmp(file.bytes + i, marker, marker_len) == 0) ++count;
  }
  return count;
}

bool DecodeHeaderAt(const FakeFileData& file,
                    size_t offset,
                    SdFinalizedHourV2Header* out) {
  return out != nullptr && offset + kSdFinalizedHourV2HeaderBytes <= file.size &&
         DecodeSdFinalizedHourV2Header(file.bytes + offset,
                                       kSdFinalizedHourV2HeaderBytes, out);
}

bool HeaderCrcValidAt(const FakeFileData& file, size_t offset) {
  SdFinalizedHourV2Header header;
  if (!DecodeHeaderAt(file, offset, &header)) return false;
  return ComputeSdFinalizedHourV2HeaderCrc32(file.bytes + offset,
                                             kSdFinalizedHourV2HeaderBytes) ==
         header.header_crc32;
}

bool PayloadCrcValidAt(const FakeFileData& file, size_t offset) {
  SdFinalizedHourV2Header header;
  if (!DecodeHeaderAt(file, offset, &header)) return false;
  if (offset + header.record_bytes > file.size ||
      header.record_bytes < header.header_bytes) {
    return false;
  }
  return ComputeSdFinalizedHourV2PayloadCrc32(
             file.bytes + offset + header.header_bytes,
             header.record_bytes - header.header_bytes) == header.payload_crc32;
}

bool StoreBegin(SdHistoryStore* store, FakeFs* fs) {
  return store != nullptr && fs != nullptr && store->Begin(*fs, kBaseDir);
}

void TestEmptyFileFirstAppendWritesPreambleMarkerAndRecord() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &snapshot));

  CHECK_TRUE(store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  const size_t preamble_bytes = PreambleBytes();
  CHECK_TRUE(file->size > preamble_bytes + kSdFinalizedHourV2HeaderBytes);
  CHECK_TRUE(std::memcmp(file->bytes, "MeshTemps finalized-hour v2 format\n", std::strlen("MeshTemps finalized-hour v2 format\n")) == 0);
  CHECK_EQ(CountMarker(*file), static_cast<uint32_t>(1));
  CHECK_EQ(fs.last_read_seek, static_cast<uint32_t>(preamble_bytes));

  SdFinalizedHourV2Header header;
  CHECK_TRUE(DecodeHeaderAt(*file, preamble_bytes, &header));
  CHECK_EQ(header.record_magic, kSdFinalizedHourV2RecordMagic);
  CHECK_EQ(header.record_version, kSdFinalizedHourV2RecordVersion);
  CHECK_TRUE(HeaderCrcValidAt(*file, preamble_bytes));
  CHECK_TRUE(PayloadCrcValidAt(*file, preamble_bytes));
}

void TestSecondAppendDoesNotDuplicatePreambleAndWritesTwoRecords() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot first;
  HistoryHourSnapshot second;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &first));
  CHECK_TRUE(BuildSnapshot(kHour, 6U, 2244, &second));

  CHECK_TRUE(store.AppendFinalizedHourSnapshot(first));
  CHECK_TRUE(store.AppendFinalizedHourSnapshot(second));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  const size_t preamble_bytes = PreambleBytes();
  CHECK_EQ(CountMarker(*file), static_cast<uint32_t>(1));

  SdFinalizedHourV2Header first_header;
  CHECK_TRUE(DecodeHeaderAt(*file, preamble_bytes, &first_header));
  const size_t second_offset = preamble_bytes + first_header.record_bytes;
  SdFinalizedHourV2Header second_header;
  CHECK_TRUE(DecodeHeaderAt(*file, second_offset, &second_header));
  CHECK_EQ(second_header.record_magic, kSdFinalizedHourV2RecordMagic);
  CHECK_TRUE(HeaderCrcValidAt(*file, preamble_bytes));
  CHECK_TRUE(PayloadCrcValidAt(*file, preamble_bytes));
  CHECK_TRUE(HeaderCrcValidAt(*file, second_offset));
  CHECK_TRUE(PayloadCrcValidAt(*file, second_offset));
  CHECK_EQ(file->size, static_cast<uint32_t>(second_offset + second_header.record_bytes));
}

void TestNonEmptyMissingMarkerFailsWithoutAppendOrRewrite() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  char path[kSdHistoryPathMax];
  CHECK_TRUE(BuildPath(kHour, path, sizeof(path)));
  const uint8_t bad[] = {'n', 'o', ' ', 'm', 'a', 'r', 'k', 'e', 'r'};
  CHECK_TRUE(fs.SeedFile(path, bad, sizeof(bad)));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &snapshot));

  CHECK_TRUE(!store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = fs.FileByPath(path);
  CHECK_TRUE(file != nullptr);
  CHECK_EQ(file->size, static_cast<uint32_t>(sizeof(bad)));
  CHECK_TRUE(std::memcmp(file->bytes, bad, sizeof(bad)) == 0);
}

void TestHeaderCrcMutationDuringReadbackRejectsAppend() {
  FakeFs fs;
  fs.mutate_header_after_append_close = true;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &snapshot));

  CHECK_TRUE(!store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  CHECK_TRUE(!HeaderCrcValidAt(*file, PreambleBytes()));
}

void TestPayloadCrcMutationDuringReadbackRejectsAppend() {
  FakeFs fs;
  fs.mutate_payload_after_append_close = true;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &snapshot));

  CHECK_TRUE(!store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  CHECK_TRUE(HeaderCrcValidAt(*file, PreambleBytes()));
  CHECK_TRUE(!PayloadCrcValidAt(*file, PreambleBytes()));
}

void TestPreambleWriteFailureReturnsFalseWithoutBinaryRecord() {
  FakeFs fs;
  fs.fail_next_write = true;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildSnapshot(kHour, 5U, 2134, &snapshot));

  CHECK_TRUE(!store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  CHECK_EQ(file->size, static_cast<uint32_t>(0));
}

void TestZeroSensorSnapshotCreatesPreambleOnlyInitializedDayFile() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildZeroSensorSnapshot(kHour, &snapshot));

  CHECK_TRUE(store.AppendFinalizedHourSnapshot(snapshot));
  const FakeFileData* file = FinalizedFile(fs, kHour);
  CHECK_TRUE(file != nullptr);
  const size_t preamble_bytes = PreambleBytes();
  CHECK_EQ(file->size, static_cast<uint32_t>(preamble_bytes));
  CHECK_EQ(CountMarker(*file), static_cast<uint32_t>(1));
}

void Run(const char* name, void (*fn)()) {
  g_test.current_test = name;
  fn();
}

}  // namespace

int main() {
  Run("TestEmptyFileFirstAppendWritesPreambleMarkerAndRecord",
      TestEmptyFileFirstAppendWritesPreambleMarkerAndRecord);
  Run("TestSecondAppendDoesNotDuplicatePreambleAndWritesTwoRecords",
      TestSecondAppendDoesNotDuplicatePreambleAndWritesTwoRecords);
  Run("TestNonEmptyMissingMarkerFailsWithoutAppendOrRewrite",
      TestNonEmptyMissingMarkerFailsWithoutAppendOrRewrite);
  Run("TestHeaderCrcMutationDuringReadbackRejectsAppend",
      TestHeaderCrcMutationDuringReadbackRejectsAppend);
  Run("TestPayloadCrcMutationDuringReadbackRejectsAppend",
      TestPayloadCrcMutationDuringReadbackRejectsAppend);
  Run("TestPreambleWriteFailureReturnsFalseWithoutBinaryRecord",
      TestPreambleWriteFailureReturnsFalseWithoutBinaryRecord);
  Run("TestZeroSensorSnapshotCreatesPreambleOnlyInitializedDayFile",
      TestZeroSensorSnapshotCreatesPreambleOnlyInitializedDayFile);
  if (g_test.failures != 0U) {
    std::cerr << g_test.failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "sd_history_store_v2_append_test passed" << std::endl;
  return 0;
}
