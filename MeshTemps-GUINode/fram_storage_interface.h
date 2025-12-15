#ifndef FRAM_STORAGE_INTERFACE_H_
#define FRAM_STORAGE_INTERFACE_H_

#include <stddef.h>
#include <stdint.h>

class FramStorageInterface {
 public:
  virtual ~FramStorageInterface() = default;

  virtual bool ReadBytes(uint32_t address, void* buffer, size_t length) = 0;
  virtual bool WriteBytes(uint32_t address, const void* buffer, size_t length) = 0;
};

#endif  // FRAM_STORAGE_INTERFACE_H_
