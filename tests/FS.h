#ifndef MESHTEMPS_TEST_FS_H_
#define MESHTEMPS_TEST_FS_H_

#include <cstddef>
#include <cstdint>

namespace fs { class FS; }

class File {
 public:
  File();
  File(fs::FS* owner, int file_index, bool append_mode, bool read_mode);

  explicit operator bool() const;
  size_t write(const uint8_t* data, size_t len);
  int read(uint8_t* data, size_t len);
  bool seek(uint32_t position);
  uint32_t position() const;
  void flush();
  void close();

 private:
  fs::FS* owner_;
  int file_index_;
  uint32_t position_;
  bool open_;
  bool append_mode_;
  bool read_mode_;
};

namespace fs {

class FS {
 public:
  virtual ~FS() = default;
  virtual bool exists(const char* path) = 0;
  virtual bool mkdir(const char* path) = 0;
  virtual File open(const char* path, const char* mode) = 0;

  virtual uint32_t FileSize(int file_index) const = 0;
  virtual size_t FileWrite(int file_index,
                           const uint8_t* data,
                           size_t len,
                           bool append_mode,
                           uint32_t* position) = 0;
  virtual int FileRead(int file_index,
                       uint8_t* data,
                       size_t len,
                       uint32_t* position) = 0;
  virtual void FileClosed(int file_index, bool append_mode, bool read_mode) = 0;
  virtual void FileSeek(int file_index, bool read_mode, uint32_t position) = 0;
};

}  // namespace fs

inline File::File()
    : owner_(nullptr), file_index_(-1), position_(0), open_(false),
      append_mode_(false), read_mode_(false) {}

inline File::File(fs::FS* owner, int file_index, bool append_mode, bool read_mode)
    : owner_(owner), file_index_(file_index), position_(0), open_(owner != nullptr),
      append_mode_(append_mode), read_mode_(read_mode) {
  if (open_ && append_mode_) position_ = owner_->FileSize(file_index_);
}

inline File::operator bool() const { return open_ && owner_ != nullptr && file_index_ >= 0; }

inline size_t File::write(const uint8_t* data, size_t len) {
  if (!open_ || owner_ == nullptr || file_index_ < 0) return 0;
  return owner_->FileWrite(file_index_, data, len, append_mode_, &position_);
}

inline int File::read(uint8_t* data, size_t len) {
  if (!open_ || owner_ == nullptr || file_index_ < 0) return -1;
  return owner_->FileRead(file_index_, data, len, &position_);
}

inline bool File::seek(uint32_t position) {
  if (!open_ || owner_ == nullptr || file_index_ < 0 || position > owner_->FileSize(file_index_)) {
    return false;
  }
  position_ = position;
  owner_->FileSeek(file_index_, read_mode_, position);
  return true;
}

inline uint32_t File::position() const { return position_; }

inline void File::flush() {}

inline void File::close() {
  if (open_ && owner_ != nullptr && file_index_ >= 0) {
    owner_->FileClosed(file_index_, append_mode_, read_mode_);
  }
  open_ = false;
}

#endif  // MESHTEMPS_TEST_FS_H_
