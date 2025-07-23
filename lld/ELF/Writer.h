//===- Writer.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_WRITER_H
#define LLD_ELF_WRITER_H

#include "Config.h"
#include <vector>

namespace lld::elf {
class OutputSection;
class PhdrEntry;

void copySectionsIntoPartitions(Ctx &ctx);
template <class ELFT> void writeResult(Ctx &ctx);

void addReservedSymbols(Ctx &ctx);
bool includeInSymtab(Ctx &, const Symbol &);
unsigned getSectionRank(Ctx &, OutputSection &osec);

// PS4/Orbis-specific program headers
extern std::vector<PhdrEntry*> scePhdrEntries;

} // namespace lld::elf

#endif
