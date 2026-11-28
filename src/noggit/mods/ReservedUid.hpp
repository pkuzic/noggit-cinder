// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// Reserved MODF/MDDF uniqueId block for mod placements with a FIXED uid (mod.add_wmo opts.uid, docs/MODS.md).
// The max-uid search and "fix all uids" keep uids in it untouched and new uids never enter it, so a uid in it stays
// what the mod chose (the Everwood House Builder mod places its generated buildings there; its server tools expect it).
#pragma once

#include <cstdint>

namespace Noggit::Mods
{
  constexpr std::uint32_t RESERVED_UID_FIRST = 0x57F00000u;
  constexpr std::uint32_t RESERVED_UID_END = 0x58000000u;  // exclusive
  inline bool isReservedUid(std::uint32_t uid) { return uid >= RESERVED_UID_FIRST && uid < RESERVED_UID_END; }
}
