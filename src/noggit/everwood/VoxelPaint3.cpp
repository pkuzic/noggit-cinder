// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// See VoxelPaint3.hpp. Every function names its Rust original (benilla-voxel paint3.rs / paint.rs). Keep
// the operation ORDER identical: the golden vectors compare bytes.
#include <noggit/everwood/VoxelPaint3.hpp>
#include <noggit/everwood/PaintPalette.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Noggit::Everwood::Voxel
{
  // ------------------------------------------------------------------------------------- paint.rs
  // falloff_amount: `(s * w * 255.0).round().clamp(0.0, 255.0) as u8` (left to right, no FMA)
  uint8_t PaintFalloffAmount(float s, uint8_t falloff, float d, float r)
  {
    float const u = 1.0f - d / r;
    float w;
    if (falloff == FALLOFF_LINEAR)
      w = u;
    else if (falloff == FALLOFF_FLAT)
      w = 1.0f;
    else
      w = (u * u) * (3.0f - 2.0f * u);
    float const sw = s * w;
    float a = std::round(sw * 255.0f);
    if (a < 0.0f) a = 0.0f;
    if (a > 255.0f) a = 255.0f;
    return uint8_t(a);
  }

  uint8_t PaintInc(uint8_t a, uint8_t amt)
  {
    uint32_t const x = a, m = amt;
    return uint8_t(x + ((255u - x) * m + 127u) / 255u);
  }

  uint8_t PaintDec(uint8_t a, uint8_t amt)
  {
    uint32_t const x = a, m = amt;
    return uint8_t(x - (x * m + 127u) / 255u);
  }

  // ------------------------------------------------------------------------------------ Paint3Param
  Paint3Param Paint3Param::FromParam(uint16_t p)
  {
    Paint3Param o;
    uint8_t const f = uint8_t((p >> PAINT_FALLOFF_SHIFT) & 0x3);
    o.id = uint16_t(p & PAINT_ID_MASK);
    o.falloff = f == 3 ? FALLOFF_SMOOTH : f;
    o.erase = (p & PAINT_ERASE) != 0;
    o.reveal = (p & PAINT3_REVEAL) != 0;
    return o;
  }

  uint16_t Paint3Param::ToParam() const
  {
    return uint16_t((id & PAINT_ID_MASK) | (uint16_t(std::min<uint8_t>(falloff, 2)) << PAINT_FALLOFF_SHIFT)
                    | (erase ? PAINT_ERASE : 0) | (reveal ? PAINT3_REVEAL : 0));
  }

  namespace
  {
    enum class Op { None, Paint, Erase, Reveal };

    // Paint3Param::op: None = the dab paints nothing (a paint of an id past the palette's end).
    Op opOf(Paint3Param p)
    {
      if (p.reveal)
        return Op::Reveal;
      if (p.erase)
        return Op::Erase;
      if (size_t(p.id) < PAINT_PALETTE_LEN)
        return Op::Paint;
      return Op::None;
    }

    uint8_t const* zeroLayer()
    {
      static uint8_t const zero[PAINT3_LAYER_BYTES] = {};
      return zero;
    }

    void fnv(uint64_t& h, uint8_t const* d, size_t n)
    {
      for (size_t i = 0; i < n; ++i)
      {
        h ^= d[i];
        h *= 0x100000001b3ull;
      }
    }
  }

  // ------------------------------------------------------------------------------------ ChunkPaint3
  uint8_t const* ChunkPaint3::Layer(size_t z) const
  {
    if (z < m_layers.size() && !m_layers[z].empty())
      return m_layers[z].data();
    return zeroLayer();
  }

  std::array<uint8_t, 4> ChunkPaint3::Rgba(size_t idx) const
  {
    uint8_t const* l = Layer(idx / PAINT3_LAYER);
    size_t const o = (idx % PAINT3_LAYER) * 4;
    return { l[o], l[o + 1], l[o + 2], l[o + 3] };
  }

  uint8_t* ChunkPaint3::PxMut(size_t idx)
  {
    if (m_layers.empty())
      m_layers.resize(size_t(C));
    std::vector<uint8_t>& l = m_layers[idx / PAINT3_LAYER];
    if (l.empty())
      l.assign(PAINT3_LAYER_BYTES, 0);
    return l.data() + (idx % PAINT3_LAYER) * 4;
  }

  std::vector<uint8_t> ChunkPaint3::Dense() const
  {
    std::vector<uint8_t> v;
    v.reserve(PAINT3_BYTES);
    for (size_t z = 0; z < size_t(C); ++z)
    {
      uint8_t const* l = Layer(z);
      v.insert(v.end(), l, l + PAINT3_LAYER_BYTES);
    }
    return v;
  }

  uint64_t ChunkPaint3::Hash() const
  {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t z = 0; z < size_t(C); ++z)
      fnv(h, Layer(z), PAINT3_LAYER_BYTES);
    return h;
  }

  int ChunkPaint3::SlotOf(uint16_t id) const
  {
    for (size_t k = 0; k < mats.size(); ++k)
      if (mats[k] == id)
        return int(k);
    return -1;
  }

  bool ChunkPaint3::operator==(ChunkPaint3 const& o) const
  {
    if (mats != o.mats)
      return false;
    for (size_t z = 0; z < size_t(C); ++z)
      if (std::memcmp(Layer(z), o.Layer(z), PAINT3_LAYER_BYTES) != 0)
        return false;
    return true;
  }

  bool ChunkPaint3::WouldRefuse(Paint3Param param, std::vector<Paint3Amount> const& amounts) const
  {
    return opOf(param) == Op::Paint && !amounts.empty() && SlotOf(param.id) < 0 && mats.size() >= PAINT3_MAX_SLOTS;
  }

  DabOutcome ChunkPaint3::Apply(Paint3Param param, std::vector<Paint3Amount> const& amounts)
  {
    Op const op = opOf(param);
    if (op == Op::None || amounts.empty())
      return DabOutcome::Untouched;
    if (op == Op::Reveal)
    {
      size_t const n = mats.size();
      if (n == 0)
        return DabOutcome::NoOp;
      for (Paint3Amount const& am : amounts)
      {
        uint8_t* px = PxMut(am.first);
        for (size_t c = 0; c < n; ++c)
          px[c] = PaintDec(px[c], am.second);
      }
    }
    else if (op == Op::Erase)
    {
      int const k = SlotOf(param.id);
      if (k < 0)
        return DabOutcome::NoOp;
      for (Paint3Amount const& am : amounts)
      {
        uint8_t* px = PxMut(am.first);
        px[k] = PaintDec(px[k], am.second);
      }
    }
    else
    {
      int k = SlotOf(param.id);
      if (k < 0)
      {
        if (mats.size() >= PAINT3_MAX_SLOTS)
          return DabOutcome::Refused;
        // The channel of an unused slot is all 0 (prune keeps it so).
        mats.push_back(param.id);
        k = int(mats.size()) - 1;
      }
      size_t const n = mats.size();
      for (Paint3Amount const& am : amounts)
      {
        uint8_t* px = PxMut(am.first);
        px[k] = PaintInc(px[k], am.second);
        for (size_t c = size_t(k) + 1; c < n; ++c)
          px[c] = PaintDec(px[c], am.second);
      }
    }
    Prune();
    return DabOutcome::Applied;
  }

  // Remove every slot whose channel is 0 everywhere (the rest move down in order), free all-zero layers.
  void ChunkPaint3::Prune()
  {
    size_t const n = mats.size();
    bool used[PAINT3_MAX_SLOTS] = { false, false, false, false };
    for (std::vector<uint8_t> const& l : m_layers)
      for (size_t i = 0; i < l.size(); i += 4)
        for (size_t k = 0; k < n; ++k)
          used[k] = used[k] || l[i + k] != 0;
    bool anyUnused = false;
    for (size_t k = 0; k < n; ++k)
      anyUnused = anyUnused || !used[k];
    if (anyUnused)
    {
      std::vector<size_t> keep;
      for (size_t k = 0; k < n; ++k)
        if (used[k])
          keep.push_back(k);
      std::vector<uint16_t> nm;
      for (size_t k : keep)
        nm.push_back(mats[k]);
      mats = nm;
      for (std::vector<uint8_t>& l : m_layers)
        for (size_t i = 0; i < l.size(); i += 4)
        {
          uint8_t const old[4] = { l[i], l[i + 1], l[i + 2], l[i + 3] };
          for (size_t c = 0; c < 4; ++c)
            l[i + c] = c < keep.size() ? old[keep[c]] : 0;
        }
    }
    for (std::vector<uint8_t>& l : m_layers)
      if (!l.empty() && std::all_of(l.begin(), l.end(), [](uint8_t v) { return v == 0; }))
        std::vector<uint8_t>().swap(l);
    if (mats.empty())
      m_layers.clear();
  }

  // ---------------------------------------------------------------------------------- dab3_amounts
  Paint3Amounts Dab3Amounts(Edit const& e, DensityFn const& density)
  {
    Paint3Amounts out;
    if (!IsPaint3(e) || !e.IsValid())
      return out;
    Paint3Param const pp = Paint3Param::FromParam(e.param);
    if (opOf(pp) == Op::None)
      return out;
    Key lo, hi;
    e.Bbox(lo, hi);
    size_t const dim[3] = { size_t(hi[0] - lo[0] + 1), size_t(hi[1] - lo[1] + 1), size_t(hi[2] - lo[2] + 1) };
    auto at = [&](int32_t x, int32_t y, int32_t z, size_t& c) -> bool
    {
      int32_t const i = x - lo[0], j = y - lo[1], k = z - lo[2];
      if (i < 0 || j < 0 || k < 0 || size_t(i) >= dim[0] || size_t(j) >= dim[1] || size_t(k) >= dim[2])
        return false;
      c = size_t(i) + dim[0] * (size_t(j) + dim[1] * size_t(k));
      return true;
    };
    float const r = e.radius;
    // Per-axis squared offsets (yards): dx = p.x as f32 * 0.5 - pos.x.
    std::vector<float> sq[3];
    for (int a = 0; a < 3; ++a)
    {
      sq[a].resize(dim[a]);
      for (size_t i = 0; i < dim[a]; ++i)
      {
        float const px = float(lo[a] + int32_t(i)) * VOXEL_SIZE;
        float const d = px - e.pos[a];
        sq[a][i] = d * d;
      }
    }
    // Cell state: 0 outside R, 1 solid in R, 2 air in R, 3 air in F.
    constexpr uint8_t OUT = 0, SOLID = 1, AIR_R = 2, FLOOD = 3;
    size_t const n = dim[0] * dim[1] * dim[2];
    std::vector<uint8_t> st(n, OUT);
    std::vector<float> dist(n, 0.0f);
    for (size_t k = 0; k < dim[2]; ++k)
      for (size_t j = 0; j < dim[1]; ++j)
        for (size_t i = 0; i < dim[0]; ++i)
        {
          float const s2 = sq[0][i] + sq[1][j];
          float const s3 = s2 + sq[2][k];
          float const d = std::sqrt(s3);
          if (d < r)
          {
            size_t const c = i + dim[0] * (j + dim[1] * k);
            dist[c] = d;
            st[c] = density(lo[0] + int32_t(i), lo[1] + int32_t(j), lo[2] + int32_t(k)) >= 0 ? AIR_R : SOLID;
          }
        }
    // Seed: round(c) half away from zero, else its air 26-neighbours in R.
    int32_t const s0[3] = { int32_t(std::round(e.pos[0] / VOXEL_SIZE)), int32_t(std::round(e.pos[1] / VOXEL_SIZE)),
                            int32_t(std::round(e.pos[2] / VOXEL_SIZE)) };
    std::vector<std::array<int32_t, 3>> stack;
    size_t c0;
    if (at(s0[0], s0[1], s0[2], c0) && st[c0] == AIR_R)
    {
      st[c0] = FLOOD;
      stack.push_back({ s0[0], s0[1], s0[2] });
    }
    else
    {
      for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
          {
            if (dx == 0 && dy == 0 && dz == 0)
              continue;
            std::array<int32_t, 3> const p = { s0[0] + dx, s0[1] + dy, s0[2] + dz };
            size_t c;
            if (at(p[0], p[1], p[2], c) && st[c] == AIR_R)
            {
              st[c] = FLOOD;
              stack.push_back(p);
            }
          }
    }
    if (stack.empty())
      return out;
    // F: 6-connected air of R (a set; the visit order does not matter).
    static int32_t const N6[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    while (!stack.empty())
    {
      std::array<int32_t, 3> const p = stack.back();
      stack.pop_back();
      for (auto const& d : N6)
      {
        std::array<int32_t, 3> const q = { p[0] + d[0], p[1] + d[1], p[2] + d[2] };
        size_t c;
        if (at(q[0], q[1], q[2], c) && st[c] == AIR_R)
        {
          st[c] = FLOOD;
          stack.push_back(q);
        }
      }
    }
    // P = F + the solid samples of R with a 26-neighbour in F.
    float const s = float(e.strength) / 255.0f;
    for (size_t k = 0; k < dim[2]; ++k)
      for (size_t j = 0; j < dim[1]; ++j)
        for (size_t i = 0; i < dim[0]; ++i)
        {
          size_t const c = i + dim[0] * (j + dim[1] * k);
          int32_t const p[3] = { lo[0] + int32_t(i), lo[1] + int32_t(j), lo[2] + int32_t(k) };
          bool painted = false;
          if (st[c] == FLOOD)
            painted = true;
          else if (st[c] == SOLID)
          {
            for (int dz = -1; dz <= 1 && !painted; ++dz)
              for (int dy = -1; dy <= 1 && !painted; ++dy)
                for (int dx = -1; dx <= 1 && !painted; ++dx)
                {
                  size_t q;
                  if ((dx != 0 || dy != 0 || dz != 0) && at(p[0] + dx, p[1] + dy, p[2] + dz, q) && st[q] == FLOOD)
                    painted = true;
                }
          }
          if (!painted)
            continue;
          uint8_t const a = PaintFalloffAmount(s, pp.falloff, dist[c], r);
          if (a == 0)
            continue;
          Key const ck{ chunk_of(p[0]), chunk_of(p[1]), chunk_of(p[2]) };
          size_t const idx = owned_index(p[0] - ck[0] * C, p[1] - ck[1] * C, p[2] - ck[2] * C);
          out[ck].emplace_back(uint32_t(idx), a);
        }
    return out;
  }

  // ------------------------------------------------------------------------------------- apply_dab3
  std::vector<Key> ApplyDab3(Edit const& e, DensityFn const& density, Paint3Map& paint, std::function<bool(Key const&)> const& filter)
  {
    Paint3Param const pp = Paint3Param::FromParam(e.param);
    std::vector<Key> applied;
    for (auto const& kv : Dab3Amounts(e, density))
    {
      if (filter && !filter(kv.first))
        continue;
      ChunkPaint3& c = paint[kv.first];
      if (c.Apply(pp, kv.second) == DabOutcome::Applied)
        applied.push_back(kv.first);
      if (c.IsEmpty())
        paint.erase(kv.first);
    }
    return applied;
  }

  std::vector<Key> RefusingChunks3(Edit const& e, DensityFn const& density, Paint3Map const& paint)
  {
    std::vector<Key> out;
    if (!IsPaint3(e))
      return out;
    Paint3Param const pp = Paint3Param::FromParam(e.param);
    for (auto const& kv : Dab3Amounts(e, density))
    {
      auto it = paint.find(kv.first);
      if (it != paint.end() && it->second.WouldRefuse(pp, kv.second))
        out.push_back(kv.first);
    }
    return out;
  }

  // ------------------------------------------------------------------------------------ paint3_hash
  uint64_t Paint3Hash(Paint3Map const& paint)
  {
    std::vector<Key> keys;
    for (auto const& kv : paint)
      if (!kv.second.IsEmpty())
        keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (Key const& k : keys)
    {
      ChunkPaint3 const& c = paint.at(k);
      for (int32_t a : k)
      {
        uint8_t const b[4] = { uint8_t(uint32_t(a)), uint8_t(uint32_t(a) >> 8), uint8_t(uint32_t(a) >> 16), uint8_t(uint32_t(a) >> 24) };
        fnv(h, b, 4);
      }
      uint8_t const n = uint8_t(c.mats.size());
      fnv(h, &n, 1);
      for (uint16_t m : c.mats)
      {
        uint8_t const b[2] = { uint8_t(m), uint8_t(m >> 8) };
        fnv(h, b, 2);
      }
      for (size_t z = 0; z < size_t(C); ++z)
        fnv(h, c.Layer(z), PAINT3_LAYER_BYTES);
    }
    return h;
  }
}
