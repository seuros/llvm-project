//===- Orbis.h -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains PS4/Orbis-specific structures and functions for
// generating SELF (Signed ELF) files.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_ORBIS_H
#define LLD_ELF_ORBIS_H

#include <stdint.h>
#include <vector>

namespace lld::elf {

// SELF entry structure (32 bytes)
struct self_entry_t {
  uint64_t flags;
  uint64_t offset;
  uint64_t size;
  uint64_t reserved;
};

// SELF header structure
struct self_header_t {
  uint8_t magic[4];      // 0x00 - 0x4F, 0x15, 0x3D, 0x1D
  uint8_t version;       // 0x04
  uint8_t mode;          // 0x05
  uint8_t endianness;    // 0x06
  uint8_t attributes;    // 0x07
  uint32_t keyType;      // 0x08
  uint16_t headerSize;   // 0x0C
  uint16_t metaSize;     // 0x0E
  uint64_t fileSize;     // 0x10
  uint16_t entryCount;   // 0x18
  uint16_t flags;        // 0x1A
  uint32_t reserved;     // 0x1C
};

// SELF authentication info structure (136 bytes)
struct self_auth_info_t {
  uint64_t paid;         // Program Authentication ID
  uint64_t caps[4];      // Capabilities
  uint64_t attrs[4];     // Attributes
  uint8_t reserved[64];  // Reserved
};

// SELF extended info structure (64 bytes)
struct self_ex_info_t {
  uint64_t paid;         // Program Authentication ID
  uint64_t ptype;        // Program Type
  uint64_t appVersion;   // Application Version
  uint64_t fwVersion;    // Firmware Version
  uint8_t digest[32];    // SHA256 digest
};

// Internal entry info structure
struct self_entry_info_t {
  uint64_t offset;
  uint64_t size;
  bool isSegment;
  bool hasBlocks;
  uint16_t segmentIndex;
  bool hasDigest;
  bool isBlocked;
  uint32_t blockSize;
  uint64_t blockCount;
};

// SELF property bits
constexpr uint64_t SELF_PROPERTY_SIGNED = 0x01;
constexpr uint64_t SELF_PROPERTY_HASBLOCKS = 0x01;
constexpr uint64_t SELF_PROPERTY_BLOCKSIZE = 0x02;
constexpr uint64_t SELF_PROPERTY_HASDIGESTS = 0x04;
constexpr uint64_t SELF_PROPERTY_SEGMENT_INDEX = 0x0F00;

// Size constants
constexpr uint32_t BLOCK_SIZE = 0x4000;
constexpr uint32_t SELF_META_DATA_BLOCK_SIZE = 0x20;
constexpr uint32_t ORBIS_SIGNATURE_SIZE = 0x100;

// Header sizes
constexpr uint32_t SELF_HEADER_SIZE = sizeof(self_header_t);
constexpr uint32_t SELF_ELF_HEADER_SIZE = 0x40;
constexpr uint32_t SELF_EX_INFO_SIZE = sizeof(self_ex_info_t);
constexpr uint32_t SELF_AUTH_INFO_SIZE = sizeof(self_auth_info_t);

class PhdrEntry;

// Function declarations
std::vector<uint8_t> orbisCreateSignature(llvm::StringRef authInfo, uint64_t paid);
std::vector<self_entry_info_t> createSelfEntries(const std::vector<PhdrEntry*>& phdrs);
void writeSelf(uint8_t *buf);

// Global variable for PS4-specific program headers
extern std::vector<PhdrEntry*> scePhdrEntries;

} // namespace lld::elf

#endif