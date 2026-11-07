// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Everwood 3D voxel paint, journal mode 7 "PAINT3" (voxel CONTRACT v3.10). A replica of benilla-voxel
// src/paint3.rs (+ the paint.rs falloff / inc / dec it shares with mode 4), bit for bit: the golden
// vectors (voxel-core/golden-v3.10.txt) compare the dense bytes and hashes.
//
// A mode-7 dab paints the VOLUME: per voxel chunk (16 yd cube) up to 4 palette materials and one RGBA
// byte per owned sample (channel k = slot k's opacity over everything below it). The dab FLOODS from
// its origin through the air inside its sphere (never through a ceiling or a wall) and paints that air
// plus the solid samples bordering it. It never changes density (Edit::IsApplied() is false).
//
// Exactness: IEEE-754 binary32 in exactly the Rust op order, NO FMA / contraction (MSVC /fp:precise on
// x64 = SSE2 scalar, no contraction), std::round = half away from zero, inc/dec in u32 integers.
//
// Pure C++ with no Noggit, Qt or OpenGL headers, like VoxelField (the standalone golden test compiles it).
#pragma once

#include <noggit/everwood/VoxelField.hpp>

#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Noggit::Everwood::Voxel
{
  constexpr float PAINT3_MAX_RADIUS = 8.0f;       // the largest valid mode-7 radius, yards
  constexpr uint16_t PAINT_ID_MASK = 0x07ff;      // param bits 0-10: palette id
  constexpr uint16_t PAINT_FALLOFF_SHIFT = 11;    // bits 11-12: falloff (3 = smooth)
  constexpr uint16_t PAINT_ERASE = 1 << 13;       // bit 13: erase that id
  constexpr uint16_t PAINT3_REVEAL = 1 << 14;     // bit 14: lower every slot (id + erase ignored)
  constexpr size_t PAINT3_MAX_SLOTS = 4;          // 3D materials per voxel chunk (mirrors MCLY's 4)
  constexpr size_t PAINT3_BYTES = OWNED * 4;      // dense RGBA per owned sample = 131072
  constexpr size_t PAINT3_LAYER = size_t(C) * C;  // samples of one z-layer (the lazy storage unit)
  constexpr size_t PAINT3_LAYER_BYTES = PAINT3_LAYER * 4;

  // paint.rs falloff_amount: u = 1 - d/r; smooth (u*u)*(3-2u), linear u, flat 1; round((s*w)*255) 0..255.
  uint8_t PaintFalloffAmount(float s, uint8_t falloff, float d, float r);
  // paint.rs inc / dec (u32 integer maths).
  uint8_t PaintInc(uint8_t a, uint8_t amt);
  uint8_t PaintDec(uint8_t a, uint8_t amt);

  // paint.rs DabOutcome
  enum class DabOutcome { Untouched, NoOp, Refused, Applied };

  // paint3.rs Paint3Param: a decoded mode-7 param (bit 15 ignored).
  struct Paint3Param
  {
    uint16_t id = 0;
    uint8_t falloff = 0;                          // 0 smooth, 1 linear, 2 flat (3 decodes as smooth)
    bool erase = false;
    bool reveal = false;                          // wins over erase
    static Paint3Param FromParam(uint16_t p);
    uint16_t ToParam() const;
  };

  inline bool IsPaint3(Edit const& e) { return e.mode == MODE_PAINT3; }

  // One owned sample's amount: (owned_index, A 1..=255).
  using Paint3Amount = std::pair<uint32_t, uint8_t>;
  // owner chunk -> its amounts (key order = Rust BTreeMap order; samples in z-y-x order).
  using Paint3Amounts = std::map<Key, std::vector<Paint3Amount>>;
  // The canonical density of a lattice sample at the dab's journal position.
  using DensityFn = std::function<int8_t(int32_t, int32_t, int32_t)>;

  // paint3.rs ChunkPaint3: up to 4 palette ids (slot order) + RGBA per owned sample, stored lazily per
  // z-layer (32x32x4 = 4 KB) but every comparison / hash is over the dense 131072 bytes.
  class ChunkPaint3
  {
  public:
    std::vector<uint16_t> mats;                 // slot 0 first, at most PAINT3_MAX_SLOTS

    bool IsEmpty() const { return mats.empty(); }
    std::array<uint8_t, 4> Rgba(size_t idx) const;
    uint8_t const* Layer(size_t z) const;       // PAINT3_LAYER_BYTES (a shared zero layer when unset)
    std::vector<uint8_t> Dense() const;
    uint64_t Hash() const;                      // FNV-1a-64 over the dense bytes
    int SlotOf(uint16_t id) const;              // -1 = absent
    bool WouldRefuse(Paint3Param param, std::vector<Paint3Amount> const& amounts) const;
    DabOutcome Apply(Paint3Param param, std::vector<Paint3Amount> const& amounts);
    bool operator==(ChunkPaint3 const& o) const;
    bool operator!=(ChunkPaint3 const& o) const { return !(*this == o); }

  private:
    uint8_t* PxMut(size_t idx);
    void Prune();
    std::vector<std::vector<uint8_t>> m_layers; // empty, or C entries (an empty entry = all 0)
  };

  // Paint3Map (chunk -> ChunkPaint3) is declared in VoxelField.hpp (the World's paint store).

  // paint3.rs dab3_amounts: empty when `e` is not a valid mode-7 dab, paints an id past the palette, or
  // finds no air seed. `density` is only asked about samples of R.
  Paint3Amounts Dab3Amounts(Edit const& e, DensityFn const& density);

  // paint3.rs apply_dab3: apply `e` to the chunks of `paint` that `filter` admits (absent = unpainted; a
  // chunk left with no slot is removed). Returns the chunks it applied to, sorted.
  std::vector<Key> ApplyDab3(Edit const& e, DensityFn const& density, Paint3Map& paint,
                             std::function<bool(Key const&)> const& filter = nullptr);

  // paint3.rs would_refuse3: the editor's up-front check (any touched chunk already holds 4 others).
  std::vector<Key> RefusingChunks3(Edit const& e, DensityFn const& density, Paint3Map const& paint);
  inline bool WouldRefuse3(Edit const& e, DensityFn const& density, Paint3Map const& paint)
  {
    return !RefusingChunks3(e, density, paint).empty();
  }

  // paint3.rs paint3_hash: FNV-1a-64 over the painted chunks in key order: kx ky kz (i32 LE), mats.len()
  // (u8), each mat (u16 LE), the 131072 dense bytes. Unpainted chunks are skipped.
  uint64_t Paint3Hash(Paint3Map const& paint);
}
