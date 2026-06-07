#include "sd_history_path_builder.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {

void TestValidFinalizedPaths() {
  char base[kSdHistoryBaseDirMax + 1U];
  assert(SdHistoryCopyBaseDir("/history", base, sizeof(base)));
  assert(std::strcmp(base, "/history") == 0);

  char dir[kSdHistoryPathMax];
  assert(SdHistoryBuildFinalizedDirPath(base, dir, sizeof(dir)));
  assert(std::strcmp(dir, "/history/finalized") == 0);

  char path[kSdHistoryPathMax];
  assert(SdHistoryBuildFinalizedHourFilePath(28928160U, base, path, sizeof(path)));
  assert(std::strcmp(path, "/history/finalized/d20089.bin") == 0);
}

void TestMaxLengthBaseDir() {
  char input[kSdHistoryBaseDirMax + 1U];
  for (size_t i = 0; i < kSdHistoryBaseDirMax; ++i) {
    input[i] = 'a';
  }
  input[kSdHistoryBaseDirMax] = '\0';

  char base[kSdHistoryBaseDirMax + 1U];
  assert(SdHistoryCopyBaseDir(input, base, sizeof(base)));
  assert(std::strcmp(input, base) == 0);
}

void TestTooLongBaseDirRejected() {
  char input[kSdHistoryBaseDirMax + 2U];
  for (size_t i = 0; i < kSdHistoryBaseDirMax + 1U; ++i) {
    input[i] = 'b';
  }
  input[kSdHistoryBaseDirMax + 1U] = '\0';

  char base[kSdHistoryBaseDirMax + 1U];
  assert(!SdHistoryCopyBaseDir(input, base, sizeof(base)));
}

void TestSmallOutputRejected() {
  char small[8];
  assert(!SdHistoryBuildFinalizedDirPath("/history", small, sizeof(small)));
  assert(!SdHistoryBuildFinalizedHourFilePath(0U, "/history", small, sizeof(small)));
}

void TestEpochDayBucketBoundaries() {
  char path[kSdHistoryPathMax];
  assert(SdHistoryBuildFinalizedHourFilePath(0U, "/h", path, sizeof(path)));
  assert(std::strcmp(path, "/h/finalized/d0.bin") == 0);

  assert(SdHistoryBuildFinalizedHourFilePath(1439U, "/h", path, sizeof(path)));
  assert(std::strcmp(path, "/h/finalized/d0.bin") == 0);

  assert(SdHistoryBuildFinalizedHourFilePath(1440U, "/h", path, sizeof(path)));
  assert(std::strcmp(path, "/h/finalized/d1.bin") == 0);
}

void TestInvalidBaseInputsRejected() {
  char path[kSdHistoryPathMax];
  assert(!SdHistoryCopyBaseDir("", path, sizeof(path)));
  assert(!SdHistoryBuildFinalizedDirPath(nullptr, path, sizeof(path)));
}

}  // namespace

int main() {
  TestValidFinalizedPaths();
  TestMaxLengthBaseDir();
  TestTooLongBaseDirRejected();
  TestSmallOutputRejected();
  TestEpochDayBucketBoundaries();
  TestInvalidBaseInputsRejected();
  std::cout << "sd_history_path_builder_test: PASS" << std::endl;
  return 0;
}
