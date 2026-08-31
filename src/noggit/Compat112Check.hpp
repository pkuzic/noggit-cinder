// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#ifndef NOGGIT_COMPAT112CHECK_HPP
#define NOGGIT_COMPAT112CHECK_HPP

#include <string>
#include <vector>

class World;

namespace Noggit
{
  enum class Compat112Severity
  {
    Error,    // the 1.12 client cannot render/use this at all (invisible / broken)
    Warning,  // renders, but wrong
    Info      // silently ignored by 1.12; harmless, informational only
  };

  struct Compat112Finding
  {
    std::string category;   // grouping label shown as a tree parent
    std::string detail;     // human-readable description of the specific issue
    int tile_x = -1;        // owning tile; -1 == map-global / not tile-specific
    int tile_z = -1;
    Compat112Severity severity = Compat112Severity::Error;
  };

  // Scans every currently-loaded tile of `world` for data the WoW 1.12 / vanilla
  // client cannot render or use. Read-only: never mutates the map. Returns the
  // findings (empty == the loaded tiles are 1.12-clean).
  std::vector<Compat112Finding> check_112_compat(World* world);
}

#endif // NOGGIT_COMPAT112CHECK_HPP
