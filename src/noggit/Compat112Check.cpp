// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Compat112Check.hpp>

#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/MapTile.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/SceneObject.hpp>
#include <noggit/AsyncObject.h>
#include <noggit/ChunkWater.hpp>
#include <noggit/DBC.h>
#include <noggit/application/NoggitApplication.hpp>

#include <blizzard-archive-library/include/ClientData.hpp>

#include <map>
#include <set>

namespace
{
  std::string plural_placements(int n)
  {
    return std::to_string(n) + (n == 1 ? " placement" : " placements");
  }
}

namespace Noggit
{
  std::vector<Compat112Finding> check_112_compat(World* world)
  {
    std::vector<Compat112Finding> out;

    if (!world)
      return out;

    BlizzardArchive::ClientData* client_data =
      Application::NoggitApplication::instance()->clientData();

    // De-dupe file-level findings across the whole map: one row per unique path,
    // carrying the total placement count (a model used 400x reports once).
    std::map<std::string, int> missing_files;   // not present in the client MPQs at all
    std::map<std::string, int> failed_files;     // present but this build can't load it
    // De-dupe DBC-reference findings so the same bad id from many chunks reports once.
    std::set<int> reported_area_ids;
    std::set<int> reported_liquid_types;

    for (MapTile* tile : world->mapIndex.loaded_tiles())
    {
      int const tx = static_cast<int>(tile->index.x);
      int const tz = static_cast<int>(tile->index.z);

      // ---- chunk-level checks (terrain / area / water) ----
      for (unsigned cz = 0; cz < 16; ++cz)
      {
        for (unsigned cx = 0; cx < 16; ++cx)
        {
          MapChunk* chunk = tile->getChunk(cx, cz);
          if (!chunk)
            continue;

          // High-res holes: a WotLK 64-bit hole grid the 1.12 client cannot read;
          // it falls back to the legacy 16-bit bitmask, so holes render wrong.
          if (chunk->header_flags.flags.high_res_holes)
          {
            out.push_back({ "High-res terrain holes",
              "chunk " + std::to_string(cx) + "," + std::to_string(cz)
                + " uses WotLK 64-bit holes; 1.12 reads the 16-bit bitmask (holes render wrong)",
              tx, tz, Compat112Severity::Error });
          }

          // Area id not present in the 1.12 AreaTable.dbc -> wrong / unknown zone text.
          int const area = chunk->getAreaID();
          if (area != 0 && !gAreaDB.CheckIfIdExists(static_cast<unsigned>(area)))
          {
            if (reported_area_ids.insert(area).second)
            {
              out.push_back({ "Unknown area ID",
                "areaID " + std::to_string(area) + " is not in the 1.12 AreaTable.dbc",
                tx, tz, Compat112Severity::Warning });
            }
          }

          // Water layers referencing a liquid type absent from the 1.12 LiquidType set.
          if (ChunkWater* water = chunk->liquid_chunk())
          {
            int const layers = water->layer_count();
            for (int l = 0; l < layers; ++l)
            {
              if (!water->hasData(static_cast<size_t>(l)))
                continue;

              int const liquid_type = water->getType(static_cast<size_t>(l));
              if (liquid_type != 0 && !gLiquidTypeDB.CheckIfIdExists(static_cast<unsigned>(liquid_type)))
              {
                if (reported_liquid_types.insert(liquid_type).second)
                {
                  out.push_back({ "Unsupported liquid type",
                    "liquid type " + std::to_string(liquid_type)
                      + " is not in the 1.12 LiquidType set (water may not render)",
                    tx, tz, Compat112Severity::Warning });
                }
              }
            }
          }
        }
      }

      // ---- object instances: model (M2) / WMO files that won't render on 1.12 ----
      // getObjectInstances() is keyed by the shared asset, so we test each unique
      // model/WMO once and attribute the whole placement count to it.
      for (auto const& entry : tile->getObjectInstances())
      {
        AsyncObject* asset = entry.first;
        if (!asset || entry.second.empty())
          continue;

        // Only judged assets: skip anything still streaming in.
        if (!asset->finishedLoading())
          continue;

        // loading_failed() == the editor draws an error-cube and the 1.12 client
        // will show nothing. This catches truly-missing files AND files present but
        // in a format this (vanilla) build can't parse -- e.g. a WotLK-only M2.
        if (!asset->loading_failed())
          continue;

        std::string const path = asset->file_key().filepath();
        int const count = static_cast<int>(entry.second.size());

        bool const in_client = client_data && client_data->exists(asset->file_key());
        if (in_client)
          failed_files[path] += count;
        else
          missing_files[path] += count;
      }
    }

    // Emit the de-duped file findings (map-global -- not tied to one tile).
    for (auto const& kv : missing_files)
    {
      out.push_back({ "Missing model/WMO file",
        kv.first + "  (" + plural_placements(kv.second)
          + ") -- not in the client data; invisible in game",
        -1, -1, Compat112Severity::Error });
    }
    for (auto const& kv : failed_files)
    {
      out.push_back({ "Model/WMO failed to load",
        kv.first + "  (" + plural_placements(kv.second)
          + ") -- present but unreadable by this build (corrupt or non-1.12 format)",
        -1, -1, Compat112Severity::Error });
    }

    return out;
  }
}
