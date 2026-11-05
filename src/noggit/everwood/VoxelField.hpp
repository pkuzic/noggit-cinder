// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// The Everwood voxel field: the same density maths as the game client (benilla-voxel world.rs,
// CONTRACT v3, v3.3 soft brushes, v3.6 shapes) so an edit made here lands on exactly the same samples.
// Ported from the server replica MonkeyVoxelField, which itself names its Rust originals; the Rust
// sources are Apache-2.0 / MIT, see EVERWOOD-MODE.md.
//
// Pure C++ with no Noggit, Qt or OpenGL headers: the standalone golden-vector test compiles this very
// file (voxel-core/golden-v3.6.txt).
//
// Determinism: every op is float (never double), x*x for powi(2), std::sqrt on float, std::round (half
// away from zero == Rust f32::round), clamp, cast. MSVC x64 /fp:precise uses SSE2 and contracts no FMA.
// Lattice I = round(wow/0.5) (a lattice point i sits at wow i*0.5); chunk = I.div_euclid(32) = 16-yd
// cube; density i8 SDF in 1/64 voxel units, negative = solid, 127 = air. All coordinates here are WoW
// axes (x north, y west, z up), never Noggit's.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Noggit::Everwood::Voxel
{
  constexpr int32_t C = 32;                       // voxels per chunk edge
  constexpr int32_t SAMPLES = C + 2;              // mesh window edge (1-sample apron)
  constexpr int32_t SEED_W = C + 4;               // seed window edge (apron + smooth read ring)
  constexpr float VOXEL_SIZE = 0.5f;
  constexpr float CHUNK_YARDS = 16.0f;
  constexpr float DENSITY_SCALE = 64.0f;
  constexpr float SEED_LIFT = 0.15f;
  constexpr float RENDER_DROP = SEED_LIFT + 0.02f; // the mesh is drawn this far below its density
  constexpr int8_t AIR = 127;
  constexpr size_t OWNED = size_t(C) * C * C;
  constexpr size_t WINDOW = size_t(SAMPLES) * SAMPLES * SAMPLES;

  using Key = std::array<int32_t, 3>;             // chunk key (kx, ky, kz)
  using ColKey = std::array<int32_t, 2>;          // chunk column (kx, ky)

  struct KeyHash
  {
    size_t operator()(Key const& k) const
    {
      uint64_t h = (uint64_t(uint32_t(k[0])) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(uint32_t(k[1])) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(uint32_t(k[2])) * 0x165667B19E3779F9ull);
      return size_t(h ^ (h >> 29));
    }
  };

  struct ColHash
  {
    size_t operator()(ColKey const& k) const
    {
      uint64_t h = (uint64_t(uint32_t(k[0])) << 32) | uint32_t(k[1]);
      h *= 0x9E3779B97F4A7C15ull;
      return size_t(h ^ (h >> 31));
    }
  };

  // div_euclid(i, 32) = floor(i / 32) = arithmetic shift. NOT `i / C` + `i % C` fix-up: MSVC 14.42 /O2
  // mis-optimizes that form inside the smooth loop (returned -559 for -17856, caught by the golden vectors).
  static_assert(C == 32, "chunk_of shifts by 5");
  inline int32_t chunk_of(int32_t i) { return i >> 5; }
  inline float lattice_to_wow(int32_t i) { return float(i) * VOXEL_SIZE; }
  inline size_t owned_index(int32_t x, int32_t y, int32_t z) { return size_t(x + C * (y + C * z)); }
  inline size_t win_index(int32_t x, int32_t y, int32_t z) { return size_t(x + SAMPLES * (y + SAMPLES * z)); }
  int8_t quant(float v);                          // round(v*64) clamped to i8
  int8_t seed_density(float h, int32_t iz);       // h = -inf -> AIR

  // Edit modes (CONTRACT 9.3). 4 paint / 5 roughen are valid on the wire but not applied by any client.
  constexpr uint8_t MODE_DIG = 0;
  constexpr uint8_t MODE_BUILD = 1;
  constexpr uint8_t MODE_SMOOTH = 2;
  constexpr uint8_t MODE_FLATTEN = 3;
  constexpr uint8_t MODE_MAX = 5;
  constexpr uint16_t SOFT_FLAG = 0x8000;

  constexpr uint8_t FALLOFF_SMOOTH = 0;
  constexpr uint8_t FALLOFF_LINEAR = 1;
  constexpr uint8_t FALLOFF_FLAT = 2;

  // CONTRACT v3.6: param bits 9-10 on modes 0 / 1. 3 is reserved and treated as a sphere.
  constexpr uint8_t SHAPE_SPHERE = 0;
  constexpr uint8_t SHAPE_CUBE = 1;
  constexpr uint8_t SHAPE_CYLINDER = 2;
  constexpr uint16_t SHAPE_SHIFT = 9;
  constexpr uint16_t SHAPE_MASK = 0x3 << SHAPE_SHIFT;

  uint8_t ShapeOfParam(uint16_t param);
  float ShapeDist(uint8_t shape, float dx, float dy, float dz);
  uint16_t SoftParam(uint8_t falloff, uint8_t hardness);
  uint16_t WithShape(uint16_t param, uint8_t shape);

  struct SoftBrush
  {
    uint8_t falloff = 0;                        // 0 smooth, 1 linear, 2 flat
    uint8_t hardness = 0;                       // percent 0..100
    static bool FromParam(uint16_t param, SoftBrush& out);
    float Weight(float dist, float r) const;
  };

  struct Edit
  {
    uint32_t seq = 0;
    uint16_t nonce = 0;
    uint16_t stroke = 0;
    uint16_t map = 0;
    float pos[3] = { 0, 0, 0 };
    float radius = 0;
    uint8_t mode = 0;
    uint8_t strength = 255;
    uint16_t param = 0;

    bool IsValid() const;
    bool IsApplied() const { return mode <= MODE_FLATTEN; }
    bool Soft(SoftBrush& b) const { return mode <= MODE_SMOOTH && SoftBrush::FromParam(param, b); }
    uint8_t Shape() const { return mode <= MODE_BUILD ? ShapeOfParam(param) : SHAPE_SPHERE; }
    // Smooth and every soft edit read samples they do not write (the canonical rebuild closure rule).
    bool ReadsNeighbours() const { SoftBrush b; return mode == MODE_SMOOTH || (mode <= MODE_BUILD && Soft(b)); }
    void Bbox(Key& lo, Key& hi) const;          // inclusive lattice box: centre +- r +- 1 voxel
    bool Touches(Key const& k) const;
    std::vector<Key> TouchedChunks() const;
  };

  // EDIT3 wire record (CONTRACT 9.1), 27 bytes little-endian: the exact form an edit is stored and
  // exported in, so it survives a round trip bit for bit.
  constexpr size_t EDIT3_LEN = 27;
  constexpr uint8_t EDIT3_OP = 0x02;
  std::array<uint8_t, EDIT3_LEN> ToWire3(Edit const& e);
  bool FromWire3(uint8_t const* bytes, Edit& out);

  std::vector<Key> ChunksOfBox(Key const& lo, Key const& hi);

  // Raw terrain height of a WoW column (the ADT height, NO lift). false = hole / no ground (air).
  using HeightFn = std::function<bool(float x, float y, float& h)>;

  struct ColumnSeed
  {
    ColKey key{};
    std::vector<float> heights;                 // SEED_W^2, lift included, -inf = air
    bool hasSurface = false;
    int32_t surfLo = 0, surfHi = 0;             // lattice z range over the own footprint + apron ring

    ColumnSeed(ColKey k, HeightFn const& height);
    bool Covers(int32_t ix, int32_t iy) const;
    float HeightLat(int32_t ix, int32_t iy) const;
    int8_t Value(int32_t ix, int32_t iy, int32_t iz) const { return seed_density(HeightLat(ix, iy), iz); }
    bool SurfaceChunks(int32_t& lo, int32_t& hi) const;   // chunk z range bracketing the surface
  };

  using Density = std::vector<int8_t>;            // OWNED samples, x fastest
  using Chunks = std::unordered_map<Key, Density, KeyHash>;
  using Outside = std::function<int8_t(int32_t, int32_t, int32_t)>;

  // world.rs apply_edit_set: apply one edit to the chunks of `set` (owned samples only). `outside`
  // answers any sample whose owner is not in `set`. Returns the keys whose density changed.
  std::vector<Key> ApplyEditSet(Edit const& e, Chunks& set, Outside const& outside);

  // One map's density world: seeds (lazy, from the height function) + active chunk densities.
  class World
  {
  public:
    explicit World(HeightFn h) : m_height(std::move(h)) {}

    std::shared_ptr<ColumnSeed const> Seed(ColKey const& col);       // creates on demand
    int8_t Sample(int32_t x, int32_t y, int32_t z);                 // chunk -> seed
    bool IsActive(Key const& k) const { return m_chunks.count(k) != 0; }

    // Activate (seed) the chunks of edit `e` incl. the surface-column expansion (CONTRACT 1), appended to
    // `added`, then apply it to the active set. Returns the keys whose density changed.
    std::vector<Key> ApplyEdit(Edit const& e, std::vector<Key>* added = nullptr);
    void Activate(Key const& k, std::vector<Key>* added);

    // world.rs build + VoxelBuildJob::run, the canonical operation: each active key of `keys` is reseeded,
    // then every edit of `edits` (in canonical order) touching any of them is applied to the whole set in
    // lockstep, reading other active chunks / seeds outside the set.
    void Rebuild(std::vector<Key> const& keys, std::vector<Edit const*> const& edits);

    void Window(Key const& k, int8_t* out /*WINDOW*/);              // 34^3 mesh window
    void WindowSeedHeights(Key const& k, float* out /*SAMPLES^2*/); // seeded surface under the window

    // Interpolated SDF (voxel units, negative = solid) at a WoW point.
    float DensityAt(float x, float y, float z);

    Chunks const& ChunksRef() const { return m_chunks; }
    size_t SeedCount() const { return m_seeds.size(); }
    std::unordered_set<int32_t> const* WantedInColumn(ColKey const& col) const;

    // Drop every seed and density (the terrain under the field changed).
    void Clear();

  private:
    HeightFn m_height;
    std::unordered_map<ColKey, std::shared_ptr<ColumnSeed const>, ColHash> m_seeds;
    Chunks m_chunks;
    std::unordered_map<ColKey, std::unordered_set<int32_t>, ColHash> m_wanted;   // column -> active kz
  };

  // fast-surface-nets 0.2.1 port (no normals in the hash; the eval-max-plane feature off, as benilla
  // builds it). Positions are window-local YARDS (sample coords * 0.5), add WindowOrigin(k) for world.
  struct Mesh
  {
    std::vector<float> pos;                     // xyz triplets
    std::vector<float> nrm;                     // xyz triplets, the surface-nets gradient (not unit length)
    std::vector<uint32_t> idx;                  // triangle list
  };
  void SurfaceNets(int8_t const* window, Mesh& out);
  std::array<float, 3> WindowOrigin(Key const& k);

  // world.rs edited_weights / seed_tilt_cos: per vertex, how edited the surface is (0 on the seeded
  // ground, 1 far off it) and the cosine between its normal and the seeded ground's normal.
  void EditedWeights(Mesh const& m, int8_t const* window, float const* seedHeights, float originZ, std::vector<float>& out);
  void SeedTiltCos(Mesh const& m, float const* seedHeights, std::vector<float>& out);

  // benilla-formats triangle_z_at (barycentric, false outside the XY projection).
  bool TriangleZAt(float const* a, float const* b, float const* c, float x, float y, float& z);

  // FNV-1a 64 over bytes (golden vectors).
  uint64_t Fnv(void const* data, size_t n, uint64_t h = 0xcbf29ce484222325ull);
  // world.rs chunks_hash: FNV over the chunks in key order (key as i32 LE, then the density bytes).
  uint64_t ChunksHash(Chunks const& chunks);

  // world.rs MapJournal: edits in canonical order with a per-chunk index, and the rebuild closure.
  class Journal
  {
  public:
    void Clear();
    void Add(Edit const& e);                    // appended in canonical order (seq ascending)
    std::vector<Edit> const& Edits() const { return m_edits; }
    std::vector<Edit>& MutableEdits() { return m_edits; }
    void Reindex();                             // after MutableEdits() was changed
    // Every edit touching any of `keys`, in canonical order, each once.
    std::vector<Edit const*> EditsFor(std::vector<Key> const& keys) const;
    // read_closure: `keys` grown by every chunk linked through a neighbour-reading edit. Sorted.
    std::set<Key> ReadClosure(std::vector<Key> const& keys) const;

  private:
    std::vector<Edit> m_edits;
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> m_index;
  };

  // Golden-vector check against voxel-core/golden-v3.6.txt (see VoxelGolden.cpp). Returns true when every
  // chunk, mesh, height and hash line matches; `report` receives a summary and the first differences.
  bool RunGolden(char const* path, std::string& report);
}
