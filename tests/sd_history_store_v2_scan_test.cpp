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
  uint32_t mkdir_count = 0;
  uint32_t read_open_count = 0;
  uint32_t append_open_count = 0;
  uint32_t write_count = 0;
  uint32_t read_count = 0;
  uint32_t read_seek_count = 0;
  uint32_t close_count = 0;
  uint32_t fail_read_at_position = UINT32_MAX;
  char last_open_mode[4] = {};

  bool exists(const char* path) override { return Find(path) >= 0; }

  bool mkdir(const char* path) override {
    ++mkdir_count;
    if (path == nullptr || path[0] == '\0') return false;
    int index = Find(path);
    if (index < 0) index = Allocate(path);
    if (index < 0) return false;
    files_[index].directory = true;
    return true;
  }

  File open(const char* path, const char* mode) override {
    if (mode != nullptr) {
      last_open_mode[0] = mode[0];
      last_open_mode[1] = mode[1];
      last_open_mode[2] = mode[2];
      last_open_mode[3] = '\0';
    }
    const bool append_mode = mode != nullptr && std::strcmp(mode, FILE_APPEND) == 0;
    const bool read_mode = mode != nullptr && std::strcmp(mode, FILE_READ) == 0;
    if (append_mode) ++append_open_count;
    if (read_mode) ++read_open_count;
    if (path == nullptr || mode == nullptr || (!append_mode && !read_mode)) return File();
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
    ++write_count;
    if (!ValidIndex(file_index) || data == nullptr || position == nullptr) return 0;
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
    ++read_count;
    if (!ValidIndex(file_index) || data == nullptr || position == nullptr) return -1;
    if (*position == fail_read_at_position) return -1;
    FakeFileData& file = files_[file_index];
    if (*position >= file.size) return 0;
    const uint32_t available = file.size - *position;
    const size_t to_read = (len < available) ? len : static_cast<size_t>(available);
    std::memcpy(data, file.bytes + *position, to_read);
    *position += static_cast<uint32_t>(to_read);
    return static_cast<int>(to_read);
  }

  void FileClosed(int file_index, bool append_mode, bool read_mode) override {
    (void)file_index;
    (void)append_mode;
    (void)read_mode;
    ++close_count;
  }

  void FileSeek(int file_index, bool read_mode, uint32_t position) override {
    (void)file_index;
    (void)position;
    if (read_mode) ++read_seek_count;
  }

  void ResetAudit() {
    mkdir_count = 0;
    read_open_count = 0;
    append_open_count = 0;
    write_count = 0;
    read_count = 0;
    read_seek_count = 0;
    close_count = 0;
    fail_read_at_position = UINT32_MAX;
    last_open_mode[0] = '\0';
  }

  uint32_t UsedFileCount() const {
    uint32_t count = 0;
    for (size_t i = 0; i < kMaxFiles; ++i) {
      if (files_[i].used) ++count;
    }
    return count;
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
    if (len != 0U) {
      if (data == nullptr) return false;
      std::memcpy(files_[index].bytes, data, len);
    }
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
    if (path == nullptr) return -1;
    const size_t len = std::strlen(path);
    if (len >= kMaxPath) return -1;
    for (size_t i = 0; i < kMaxFiles; ++i) {
      if (!files_[i].used) {
        files_[i] = FakeFileData{};
        files_[i].used = true;
        std::memcpy(files_[i].path, path, len + 1U);
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

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

bool StoreBegin(SdHistoryStore* store, FakeFs* fs) {
  return store != nullptr && fs != nullptr && store->Begin(*fs, kBaseDir);
}

bool SeedCleanFile(FakeFs* fs, SdHistoryStore* store, uint32_t hour) {
  HistoryHourSnapshot snapshot;
  return fs != nullptr && store != nullptr && BuildSnapshot(hour, 5U, 2134, &snapshot) &&
         store->AppendFinalizedHourSnapshot(snapshot);
}

SdFinalizedHourV2ScanResult ScanHour(SdHistoryStore* store,
                                     uint32_t hour,
                                     bool* ok) {
  SdFinalizedHourV2ScannerWorkspace workspace;
  SdFinalizedHourV2ScanResult result;
  *ok = store->ScanFinalizedHourFile(hour, workspace, &result);
  return result;
}

void ExpectReadOnlyScanAudit(const FakeFs& fs) {
  CHECK_EQ(fs.read_open_count, 1U);
  CHECK_EQ(fs.append_open_count, 0U);
  CHECK_EQ(fs.write_count, 0U);
  CHECK_EQ(fs.mkdir_count, 0U);
  CHECK_TRUE(std::strcmp(fs.last_open_mode, FILE_READ) == 0);
}

void TestCleanFileScan() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  CHECK_TRUE(SeedCleanFile(&fs, &store, kHour));
  char path[kSdHistoryPathMax];
  CHECK_TRUE(BuildPath(kHour, path, sizeof(path)));
  const FakeFileData* file = fs.FileByPath(path);
  CHECK_TRUE(file != nullptr);
  const uint32_t file_size = file->size;

  fs.ResetAudit();
  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kClean);
  CHECK_EQ(result.valid_record_count, 1U);
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(file_size));
  CHECK_EQ(result.first_unsafe_offset, static_cast<uint64_t>(file_size));
  ExpectReadOnlyScanAudit(fs);
  CHECK_EQ(fs.close_count, 1U);
}

void TestPreambleOnlyScan() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  HistoryHourSnapshot snapshot;
  CHECK_TRUE(BuildZeroSensorSnapshot(kHour, &snapshot));
  CHECK_TRUE(store.AppendFinalizedHourSnapshot(snapshot));

  fs.ResetAudit();
  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kEmptyPreambleOnly);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kNone);
  CHECK_EQ(result.valid_record_count, 0U);
  ExpectReadOnlyScanAudit(fs);
}

void TestMissingFileDoesNotCreate() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  fs.ResetAudit();
  const uint32_t before = fs.UsedFileCount();
  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(!ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kReadError);
  CHECK_EQ(fs.UsedFileCount(), before);
  CHECK_EQ(fs.read_open_count, 1U);
  CHECK_EQ(fs.append_open_count, 0U);
  CHECK_EQ(fs.write_count, 0U);
  CHECK_EQ(fs.mkdir_count, 0U);
  CHECK_TRUE(std::strcmp(fs.last_open_mode, FILE_READ) == 0);
}

void TestEmptyExistingFile() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  char path[kSdHistoryPathMax];
  CHECK_TRUE(BuildPath(kHour, path, sizeof(path)));
  CHECK_TRUE(fs.SeedFile(path, nullptr, 0U));

  fs.ResetAudit();
  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kMissingMarker);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kMarkerMissing);
  CHECK_EQ(result.valid_prefix_end_offset, 0ULL);
  ExpectReadOnlyScanAudit(fs);
}

void TestCorruptTailAfterValidRecord() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  CHECK_TRUE(SeedCleanFile(&fs, &store, kHour));
  char path[kSdHistoryPathMax];
  CHECK_TRUE(BuildPath(kHour, path, sizeof(path)));
  FakeFileData* file = fs.MutableFileByPath(path);
  CHECK_TRUE(file != nullptr);
  const uint32_t valid_prefix = file->size;
  CHECK_TRUE(valid_prefix < kMaxFileBytes);
  file->bytes[file->size++] = 0x4DU;

  fs.ResetAudit();
  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kCorruptTail);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kPartialHeader);
  CHECK_EQ(result.valid_record_count, 1U);
  CHECK_EQ(result.valid_prefix_end_offset, static_cast<uint64_t>(valid_prefix));
  CHECK_EQ(result.first_unsafe_offset, static_cast<uint64_t>(valid_prefix));
  ExpectReadOnlyScanAudit(fs);
}

void TestReadErrorPath() {
  FakeFs fs;
  SdHistoryStore store;
  CHECK_TRUE(StoreBegin(&store, &fs));
  CHECK_TRUE(SeedCleanFile(&fs, &store, kHour));
  fs.ResetAudit();
  fs.fail_read_at_position = 0U;

  bool ok = false;
  const SdFinalizedHourV2ScanResult result = ScanHour(&store, kHour, &ok);
  CHECK_TRUE(ok);
  CHECK_EQ(result.status, SdFinalizedHourV2ScanStatus::kReadError);
  CHECK_EQ(result.first_failure_reason, SdFinalizedHourV2ScanFailureReason::kReadError);
  ExpectReadOnlyScanAudit(fs);
}

void Run(const char* name, void (*fn)()) {
  g_test.current_test = name;
  fn();
}

}  // namespace

int main() {
  Run("TestCleanFileScan", TestCleanFileScan);
  Run("TestPreambleOnlyScan", TestPreambleOnlyScan);
  Run("TestMissingFileDoesNotCreate", TestMissingFileDoesNotCreate);
  Run("TestEmptyExistingFile", TestEmptyExistingFile);
  Run("TestCorruptTailAfterValidRecord", TestCorruptTailAfterValidRecord);
  Run("TestReadErrorPath", TestReadErrorPath);
  if (g_test.failures != 0U) {
    std::cerr << g_test.failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "sd_history_store_v2_scan_test passed" << std::endl;
  return 0;
}
