#ifndef HISTORY_CRC_H_
#define HISTORY_CRC_H_

#include <stddef.h>
#include <stdint.h>

// Simple, small-footprint CRC implementations.
// These are intentionally bitwise (no tables) because the hourly payload sizes
// are modest (tens of KB), and code size matters on embedded targets.

inline uint16_t Crc16CcittFalseUpdate(uint16_t crc, const uint8_t* data, size_t len) {
  // CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0x0000
  while (len--) {
    crc ^= static_cast<uint16_t>(*data++) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

inline uint16_t Crc16CcittFalse(const uint8_t* data, size_t len) {
  return Crc16CcittFalseUpdate(0xFFFFu, data, len);
}

inline uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  // CRC-32 (IEEE 802.3): poly=0x04C11DB7 reflected => 0xEDB88320, init=0xFFFFFFFF, xorout=0xFFFFFFFF
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

inline uint32_t Crc32(const uint8_t* data, size_t len) {
  return Crc32Update(0xFFFFFFFFu, data, len);
}


// CRC-32/ISO-HDLC (aliases: CRC-32, CRC-32/ADCCP, PKZIP/zlib/gzip).
// width=32, poly=0x04C11DB7, reflected poly=0xEDB88320, init=0xFFFFFFFF,
// refin=true, refout=true, xorout=0xFFFFFFFF, check("123456789")=0xCBF43926.
inline uint32_t Crc32IsoHdlcBegin() { return 0xFFFFFFFFu; }

inline uint32_t Crc32IsoHdlcUpdate(uint32_t state, const uint8_t* data, size_t len) {
  if (data == nullptr && len != 0U) return state;
  while (len--) {
    state ^= *data++;
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(state & 1U)));
      state = (state >> 1U) ^ (0xEDB88320u & mask);
    }
  }
  return state;
}

inline uint32_t Crc32IsoHdlcFinalize(uint32_t state) { return state ^ 0xFFFFFFFFu; }

inline uint32_t Crc32IsoHdlc(const uint8_t* data, size_t len) {
  return Crc32IsoHdlcFinalize(Crc32IsoHdlcUpdate(Crc32IsoHdlcBegin(), data, len));
}

#endif  // HISTORY_CRC_H_