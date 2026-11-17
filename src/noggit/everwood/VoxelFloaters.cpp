// This file is part of Noggit3, licensed under GNU General Public License (version 3).
// See VoxelFloaters.hpp.

#include <noggit/everwood/VoxelFloaters.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace Noggit::Everwood::Voxel
{
  namespace
  {
    enum : uint8_t { V_NEW = 0, V_BIG = 1, V_CUR = 2, V_DONE = 3 };

    struct ChunkInfo
    {
      enum Kind : uint8_t { Air, Solid, Mixed } kind = Air;
      Density const* d = nullptr;
      std::vector<uint8_t> vis;                   // Mixed only
    };

    using InfoMap = std::unordered_map<Key, ChunkInfo, KeyHash>;
    using KeySet = std::unordered_set<Key, KeyHash>;

    int32_t const N6[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

    Key chunkKey(int32_t x, int32_t y, int32_t z)
    {
      return Key{ chunk_of(x), chunk_of(y), chunk_of(z) };
    }

    size_t localIndex(Key const& k, int32_t x, int32_t y, int32_t z)
    {
      return owned_index(x - k[0] * C, y - k[1] * C, z - k[2] * C);
    }
  }

  // ------------------------------------------------------------------------------------------- find
  std::vector<Floater> FindFloaters(World& w, uint32_t maxSizeIn, FloaterRegion const& region)
  {
    uint32_t const maxSize = std::clamp<uint32_t>(maxSizeIn, 1u, 16384u);
    Chunks const& chunks = w.ChunksRef();
    InfoMap info;
    info.reserve(chunks.size());
    for (auto const& [k, d] : chunks)
    {
      size_t solid = 0;
      for (int8_t v : d)
        solid += v < 0 ? 1 : 0;
      ChunkInfo ci;
      ci.d = &d;
      ci.kind = solid == 0 ? ChunkInfo::Air : (solid == d.size() ? ChunkInfo::Solid : ChunkInfo::Mixed);
      if (ci.kind == ChunkInfo::Mixed)
        ci.vis.assign(OWNED, V_NEW);
      info.emplace(k, std::move(ci));
    }

    std::vector<Floater> out;
    std::vector<Key> comp;
    for (auto& [ck, ci] : info)
    {
      if (ci.kind != ChunkInfo::Mixed)
        continue;
      int32_t lo[3], hi[3];
      bool skip = false;
      for (int a = 0; a < 3; ++a)
      {
        lo[a] = ck[a] * C;
        hi[a] = ck[a] * C + C - 1;
        if (!region.all)
        {
          lo[a] = std::max(lo[a], region.lo[a]);
          hi[a] = std::min(hi[a], region.hi[a]);
          skip = skip || lo[a] > hi[a];
        }
      }
      if (skip)
        continue;
      for (int32_t z = lo[2]; z <= hi[2]; ++z)
        for (int32_t y = lo[1]; y <= hi[1]; ++y)
          for (int32_t x = lo[0]; x <= hi[0]; ++x)
          {
            size_t const idx = localIndex(ck, x, y, z);
            if ((*ci.d)[idx] >= 0 || ci.vis[idx] != V_NEW)
              continue;
            comp.clear();
            comp.push_back(Key{ x, y, z });
            ci.vis[idx] = V_CUR;
            bool bad = false;
            for (size_t qi = 0; qi < comp.size() && !bad; ++qi)
            {
              Key const c = comp[qi];
              for (auto const& o : N6)
              {
                int32_t const nx = c[0] + o[0], ny = c[1] + o[1], nz = c[2] + o[2];
                Key const nk = chunkKey(nx, ny, nz);
                auto it = info.find(nk);
                if (it == info.end())
                {
                  if (w.Sample(nx, ny, nz) < 0)   // the seeded ground continues here: grounded
                  {
                    bad = true;
                    break;
                  }
                  continue;
                }
                ChunkInfo& n = it->second;
                if (n.kind == ChunkInfo::Air)
                  continue;
                if (n.kind == ChunkInfo::Solid)
                {
                  bad = true;
                  break;
                }
                size_t const ni = localIndex(nk, nx, ny, nz);
                if ((*n.d)[ni] >= 0)
                  continue;
                uint8_t& v = n.vis[ni];
                if (v == V_BIG)
                {
                  bad = true;
                  break;
                }
                if (v == V_CUR)
                  continue;
                v = V_CUR;
                comp.push_back(Key{ nx, ny, nz });
                if (comp.size() > maxSize)
                {
                  bad = true;
                  break;
                }
              }
            }
            for (Key const& c : comp)
            {
              Key const k = chunkKey(c[0], c[1], c[2]);
              info[k].vis[localIndex(k, c[0], c[1], c[2])] = bad ? V_BIG : V_DONE;
            }
            if (bad)
              continue;
            if (!region.all && !region.balls.empty())
            {
              bool under = false;
              for (Key const& c : comp)
              {
                for (auto const& b : region.balls)
                {
                  float const dx = float(c[0]) - b[0], dy = float(c[1]) - b[1], dz = float(c[2]) - b[2];
                  float const reach = b[3] + 1.f;
                  if (dx * dx + dy * dy + dz * dz <= reach * reach)
                  {
                    under = true;
                    break;
                  }
                }
                if (under)
                  break;
              }
              if (!under)
                continue;
            }
            Floater f;
            f.samples = comp;
            out.push_back(std::move(f));
          }
    }

    // Diagonal contact with other solid ground.
    for (Floater& f : out)
    {
      KeySet own(f.samples.begin(), f.samples.end());
      for (Key const& s : f.samples)
      {
        for (int dz = -1; dz <= 1 && !f.touching; ++dz)
          for (int dy = -1; dy <= 1 && !f.touching; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
              Key const n{ s[0] + dx, s[1] + dy, s[2] + dz };
              if ((dx || dy || dz) && !own.count(n) && w.Sample(n[0], n[1], n[2]) < 0)
              {
                f.touching = true;
                break;
              }
            }
        if (f.touching)
          break;
      }
    }
    // A stable order (the chunk map is unordered): by first sample, z then y then x.
    std::sort(out.begin(), out.end(), [](Floater const& a, Floater const& b)
    {
      Key const& p = a.samples.front();
      Key const& q = b.samples.front();
      return std::array<int32_t, 3>{ p[2], p[1], p[0] } < std::array<int32_t, 3>{ q[2], q[1], q[0] };
    });
    return out;
  }

  // ------------------------------------------------------------------------------------------- plan
  namespace
  {
    // The field as it would be after the digs planned so far: the world, plus `committed` (digs of the floaters
    // already accepted) plus `stage` (digs of the floater being planned). A chunk is held as a full copy.
    struct Scratch
    {
      World& w;
      Chunks committed, stage;

      explicit Scratch(World& world) : w(world) {}

      int8_t get(int32_t x, int32_t y, int32_t z)
      {
        Key const k = chunkKey(x, y, z);
        auto s = stage.find(k);
        if (s != stage.end())
          return s->second[localIndex(k, x, y, z)];
        auto c = committed.find(k);
        if (c != committed.end())
          return c->second[localIndex(k, x, y, z)];
        return w.Sample(x, y, z);
      }

      // The chunk's density as the scratch has it; an inactive chunk is its seed (what activating it makes it).
      Density gather(Key const& k)
      {
        auto s = stage.find(k);
        if (s != stage.end())
          return s->second;
        auto c = committed.find(k);
        if (c != committed.end())
          return c->second;
        Chunks const& real = w.ChunksRef();
        auto r = real.find(k);
        if (r != real.end())
          return r->second;
        std::shared_ptr<ColumnSeed const> seed = w.Seed(ColKey{ k[0], k[1] });
        Density d(OWNED, AIR);
        for (int32_t z = 0; z < C; ++z)
          for (int32_t y = 0; y < C; ++y)
            for (int32_t x = 0; x < C; ++x)
              d[owned_index(x, y, z)] = seed->Value(k[0] * C + x, k[1] * C + y, k[2] * C + z);
        return d;
      }
    };

    // A hard sphere dig that covers the samples (a near-minimal ball by Badoiu-Clarkson iterations, a hair of
    // margin so the farthest sample ends at density >= 0).
    bool coverEdit(std::vector<Key> const& s, Edit& e)
    {
      float c[3];
      for (int a = 0; a < 3; ++a)
      {
        int32_t lo = s[0][a], hi = s[0][a];
        for (Key const& p : s)
        {
          lo = std::min(lo, p[a]);
          hi = std::max(hi, p[a]);
        }
        c[a] = (float(lo) + float(hi)) * 0.5f;
      }
      auto farthest = [&](float& dist) -> Key const&
      {
        Key const* best = &s[0];
        float bd = -1.f;
        for (Key const& p : s)
        {
          float const dx = float(p[0]) - c[0], dy = float(p[1]) - c[1], dz = float(p[2]) - c[2];
          float const d = std::sqrt(dx * dx + dy * dy + dz * dz);
          if (d > bd)
          {
            bd = d;
            best = &p;
          }
        }
        dist = bd;
        return *best;
      };
      float dist = 0.f;
      for (int i = 1; i <= 24; ++i)
      {
        Key const& f = farthest(dist);
        for (int a = 0; a < 3; ++a)
          c[a] += (float(f[a]) - c[a]) / float(i + 1);
      }
      farthest(dist);
      float const radiusYd = std::max((dist + 0.02f) * VOXEL_SIZE, 0.05f);
      if (radiusYd > 16.f)
        return false;
      e = Edit{};
      e.pos[0] = c[0] * VOXEL_SIZE;
      e.pos[1] = c[1] * VOXEL_SIZE;
      e.pos[2] = c[2] * VOXEL_SIZE;
      e.radius = radiusYd;
      e.mode = MODE_DIG;
      e.strength = 255;
      e.param = 0;
      return e.IsValid();
    }

    // Dry-run `e` on the scratch. Accepted (staged) only when every changed sample is a floater sample, or an
    // air sample (density >= 0 before) with no solid non-floater sample in its 26-neighbourhood, i.e. the dig
    // changes no ground and no surface of the ground, and every sample of `must` ends up non-solid.
    bool tryDig(Scratch& sc, Edit const& e, KeySet const& own, std::vector<Key> const& must)
    {
      Key lo, hi;
      e.Bbox(lo, hi);
      Chunks before, local;
      for (Key const& k : ChunksOfBox(lo, hi))
      {
        Density d = sc.gather(k);
        before.emplace(k, d);
        local.emplace(k, std::move(d));
      }
      ApplyEditSet(e, local, [&](int32_t x, int32_t y, int32_t z) { return sc.get(x, y, z); });

      auto beforeAt = [&](int32_t x, int32_t y, int32_t z) -> int8_t
      {
        Key const k = chunkKey(x, y, z);
        auto it = before.find(k);
        return it != before.end() ? it->second[localIndex(k, x, y, z)] : sc.get(x, y, z);
      };
      for (auto const& [k, nd] : local)
      {
        Density const& od = before.at(k);
        int32_t const base[3] = { k[0] * C, k[1] * C, k[2] * C };
        for (int32_t z = std::max(lo[2], base[2]); z <= std::min(hi[2], base[2] + C - 1); ++z)
          for (int32_t y = std::max(lo[1], base[1]); y <= std::min(hi[1], base[1] + C - 1); ++y)
            for (int32_t x = std::max(lo[0], base[0]); x <= std::min(hi[0], base[0] + C - 1); ++x)
            {
              size_t const i = owned_index(x - base[0], y - base[1], z - base[2]);
              if (nd[i] == od[i] || own.count(Key{ x, y, z }))
                continue;
              if (od[i] < 0)
                return false;                       // bit solid ground
              for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                  for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy || dz) && beforeAt(x + dx, y + dy, z + dz) < 0 && !own.count(Key{ x + dx, y + dy, z + dz }))
                      return false;                 // reshaped the surface next to solid ground
            }
      }
      for (Key const& s : must)
      {
        Key const k = chunkKey(s[0], s[1], s[2]);
        if (local.at(k)[localIndex(k, s[0], s[1], s[2])] < 0)
          return false;
      }
      for (auto& [k, nd] : local)
        sc.stage[k] = std::move(nd);
      return true;
    }

    bool planSubset(Scratch& sc, KeySet const& own, std::vector<Key> const& samples, std::vector<Edit>& out)
    {
      std::vector<Key> live;
      for (Key const& s : samples)
        if (sc.get(s[0], s[1], s[2]) < 0)
          live.push_back(s);
      if (live.empty())
        return true;
      Edit e;
      if (coverEdit(live, e) && tryDig(sc, e, own, live))
      {
        out.push_back(e);
        return true;
      }
      if (live.size() == 1)
        return false;
      int axis = 0;
      int32_t best = -1;
      for (int a = 0; a < 3; ++a)
      {
        int32_t lo = live[0][a], hi = live[0][a];
        for (Key const& p : live)
        {
          lo = std::min(lo, p[a]);
          hi = std::max(hi, p[a]);
        }
        if (hi - lo > best)
        {
          best = hi - lo;
          axis = a;
        }
      }
      std::sort(live.begin(), live.end(), [axis](Key const& p, Key const& q)
      {
        return std::array<int32_t, 3>{ p[axis], p[(axis + 1) % 3], p[(axis + 2) % 3] } < std::array<int32_t, 3>{ q[axis], q[(axis + 1) % 3], q[(axis + 2) % 3] };
      });
      size_t const half = live.size() / 2;
      std::vector<Key> const a(live.begin(), live.begin() + long(half)), b(live.begin() + long(half), live.end());
      return planSubset(sc, own, a, out) && planSubset(sc, own, b, out);
    }
  }

  FloaterPlan PlanFloaterRemoval(World& w, uint32_t maxSize, FloaterRegion const& region)
  {
    FloaterPlan plan;
    std::vector<Floater> const floaters = FindFloaters(w, maxSize, region);
    plan.found = uint32_t(floaters.size());
    Scratch sc(w);
    // Every floater may be changed by the digs of its neighbour floaters (they are all going away).
    KeySet own;
    for (Floater const& f : floaters)
      own.insert(f.samples.begin(), f.samples.end());
    for (Floater const& f : floaters)
    {
      if (f.touching)
        ++plan.touching;
      sc.stage.clear();
      std::vector<Edit> digs;
      if (!planSubset(sc, own, f.samples, digs))
      {
        ++plan.skippedUnsafe;
        if (f.touching)
          ++plan.skippedTouching;
        sc.stage.clear();
        continue;
      }
      for (auto& [k, d] : sc.stage)
        sc.committed[k] = std::move(d);
      sc.stage.clear();
      plan.digs.insert(plan.digs.end(), digs.begin(), digs.end());
      ++plan.removable;
    }
    return plan;
  }
}
