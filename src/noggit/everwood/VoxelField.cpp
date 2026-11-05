// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// See VoxelField.hpp. Every function names its Rust original (benilla-voxel world.rs / lib.rs,
// fast-surface-nets 0.2.1). Keep the operation ORDER identical: the golden vectors compare bytes.
#include <noggit/everwood/VoxelField.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Noggit::Everwood::Voxel
{
    // (v * DENSITY_SCALE).round().clamp(i8::MIN, i8::MAX) as i8
    int8_t quant(float v)
    {
        float r = std::round(v * DENSITY_SCALE);
        if (r < -128.0f) r = -128.0f;
        if (r > 127.0f) r = 127.0f;
        return int8_t(r);
    }

    static int8_t quantRaw(float r)                 // (x).round().clamp() as i8 (already scaled)
    {
        r = std::round(r);
        if (r < -128.0f) r = -128.0f;
        if (r > 127.0f) r = 127.0f;
        return int8_t(r);
    }

    // world.rs seed_density
    int8_t seed_density(float h, int32_t iz)
    {
        if (h == -std::numeric_limits<float>::infinity())
            return AIR;
        float sdf = (lattice_to_wow(iz) - h) / VOXEL_SIZE;
        return quant(sdf);
    }

    // ---------------------------------------------------------------------------------------- Edit
    bool Edit::IsValid() const
    {
        for (float v : pos)
            if (!std::isfinite(v) || std::fabs(v) >= 40000.0f)
                return false;
        return std::isfinite(radius) && radius > 0.0f && radius <= 16.0f && mode <= MODE_MAX;
    }

    // world.rs shape_of_param (bits 9-10; 3 = sphere)
    uint8_t ShapeOfParam(uint16_t param)
    {
        uint8_t const s = uint8_t((param & SHAPE_MASK) >> SHAPE_SHIFT);
        return s == SHAPE_CUBE || s == SHAPE_CYLINDER ? s : SHAPE_SPHERE;
    }

    // world.rs shape_dist. max(a, b) = a >= b ? a : b (inputs are always finite).
    float ShapeDist(uint8_t shape, float dx, float dy, float dz)
    {
        auto mx = [](float a, float b) { return a >= b ? a : b; };
        switch (shape)
        {
        case SHAPE_CUBE:
            return mx(mx(std::fabs(dx), std::fabs(dy)), std::fabs(dz));
        case SHAPE_CYLINDER:
            return mx(std::sqrt(dx * dx + dy * dy), std::fabs(dz));
        default:
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    uint16_t SoftParam(uint8_t falloff, uint8_t hardness)
    {
        return uint16_t(SOFT_FLAG | std::min<uint8_t>(falloff, 2) | (uint16_t(std::min<uint8_t>(hardness, 100)) << 2));
    }

    uint16_t WithShape(uint16_t param, uint8_t shape)
    {
        return uint16_t((param & ~SHAPE_MASK) | (uint16_t(shape) << SHAPE_SHIFT));
    }

    // SoftBrush::from_param
    bool SoftBrush::FromParam(uint16_t param, SoftBrush& out)
    {
        if (!(param & SOFT_FLAG))
            return false;
        uint8_t const f = uint8_t(param & 0x3);
        out.falloff = f == 3 ? 0 : f;
        uint8_t const h = uint8_t((param >> 2) & 0x7f);
        out.hardness = h > 100 ? 100 : h;
        return true;
    }

    // SoftBrush::weight (exact op order, no FMA)
    float SoftBrush::Weight(float dist, float r) const
    {
        if (dist >= r)
            return 0.0f;
        float const inner = r * (float(hardness) / 100.0f);
        if (dist <= inner)
            return 1.0f;
        float const t = (dist - inner) / (r - inner);
        if (falloff == 1)
            return 1.0f - t;
        if (falloff == 2)
            return 1.0f;
        float const u = 1.0f - t;
        return (u * u) * (3.0f - 2.0f * u);
    }

    // VoxelEditOp::lattice_bbox
    void Edit::Bbox(Key& lo, Key& hi) const
    {
        float r = radius / VOXEL_SIZE + 1.0f;
        for (int a = 0; a < 3; ++a)
        {
            float c = pos[a] / VOXEL_SIZE;
            lo[a] = int32_t(std::floor(c - r));
            hi[a] = int32_t(std::ceil(c + r));
        }
    }

    bool Edit::Touches(Key const& k) const
    {
        Key lo, hi;
        Bbox(lo, hi);
        for (int a = 0; a < 3; ++a)
            if (!(hi[a] >= k[a] * C && lo[a] <= k[a] * C + C - 1))
                return false;
        return true;
    }

    std::vector<Key> Edit::TouchedChunks() const
    {
        Key lo, hi;
        Bbox(lo, hi);
        return ChunksOfBox(lo, hi);
    }

    // world.rs chunks_of_box (z, y, x order)
    std::vector<Key> ChunksOfBox(Key const& lo, Key const& hi)
    {
        std::vector<Key> out;
        for (int32_t z = chunk_of(lo[2]); z <= chunk_of(hi[2]); ++z)
            for (int32_t y = chunk_of(lo[1]); y <= chunk_of(hi[1]); ++y)
                for (int32_t x = chunk_of(lo[0]); x <= chunk_of(hi[0]); ++x)
                    out.push_back(Key{ x, y, z });
        return out;
    }

    // VoxelEditOp::to_wire3 (CONTRACT 9.1)
    std::array<uint8_t, EDIT3_LEN> ToWire3(Edit const& e)
    {
        std::array<uint8_t, EDIT3_LEN> b{};
        auto u16 = [&](size_t at, uint16_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); };
        auto f32 = [&](size_t at, float v)
        {
            uint32_t u;
            std::memcpy(&u, &v, 4);
            b[at] = uint8_t(u); b[at + 1] = uint8_t(u >> 8); b[at + 2] = uint8_t(u >> 16); b[at + 3] = uint8_t(u >> 24);
        };
        b[0] = EDIT3_OP;
        u16(1, e.nonce);
        u16(3, e.stroke);
        u16(5, e.map);
        f32(7, e.pos[0]);
        f32(11, e.pos[1]);
        f32(15, e.pos[2]);
        f32(19, e.radius);
        b[23] = e.mode;
        b[24] = e.strength;
        u16(25, e.param);
        return b;
    }

    // VoxelEditOp::from_wire3
    bool FromWire3(uint8_t const* b, Edit& out)
    {
        if (b[0] != EDIT3_OP)
            return false;
        auto u16 = [&](size_t at) { return uint16_t(b[at] | (uint16_t(b[at + 1]) << 8)); };
        auto f32 = [&](size_t at)
        {
            uint32_t const u = uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
            float v;
            std::memcpy(&v, &u, 4);
            return v;
        };
        out.nonce = u16(1);
        out.stroke = u16(3);
        out.map = u16(5);
        out.pos[0] = f32(7);
        out.pos[1] = f32(11);
        out.pos[2] = f32(15);
        out.radius = f32(19);
        out.mode = b[23];
        out.strength = b[24];
        out.param = u16(25);
        return out.IsValid();
    }

    // ---------------------------------------------------------------------------------- ColumnSeed
    // ColumnSeed::from_fn
    ColumnSeed::ColumnSeed(ColKey k, HeightFn const& height) : key(k), heights(size_t(SEED_W) * SEED_W, -std::numeric_limits<float>::infinity())
    {
        int32_t bx = k[0] * C - 2, by = k[1] * C - 2;
        float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
        for (int32_t j = 0; j < SEED_W; ++j)
            for (int32_t i = 0; i < SEED_W; ++i)
            {
                float h;
                if (height(lattice_to_wow(bx + i), lattice_to_wow(by + j), h))
                {
                    h = h + SEED_LIFT;
                    heights[size_t(i) + size_t(j) * SEED_W] = h;
                    if (i >= 1 && i < SEED_W - 1 && j >= 1 && j < SEED_W - 1)
                    {
                        lo = std::min(lo, h);
                        hi = std::max(hi, h);
                    }
                }
            }
        hasSurface = lo <= hi;
        if (hasSurface)
        {
            surfLo = int32_t(std::floor(lo / VOXEL_SIZE));
            surfHi = int32_t(std::ceil(hi / VOXEL_SIZE));
        }
    }

    bool ColumnSeed::Covers(int32_t ix, int32_t iy) const
    {
        int32_t i = ix - (key[0] * C - 2), j = iy - (key[1] * C - 2);
        return i >= 0 && i < SEED_W && j >= 0 && j < SEED_W;
    }

    float ColumnSeed::HeightLat(int32_t ix, int32_t iy) const
    {
        int32_t i = ix - (key[0] * C - 2), j = iy - (key[1] * C - 2);
        return heights[size_t(i) + size_t(j) * SEED_W];
    }

    bool ColumnSeed::SurfaceChunks(int32_t& lo, int32_t& hi) const
    {
        if (!hasSurface)
            return false;
        lo = chunk_of(surfLo - 1);
        hi = chunk_of(surfHi + 1);
        return true;
    }

    // ---------------------------------------------------------------------------------------- World
    std::shared_ptr<ColumnSeed const> World::Seed(ColKey const& col)
    {
        auto it = m_seeds.find(col);
        if (it != m_seeds.end())
            return it->second;
        auto s = std::make_shared<ColumnSeed const>(col, m_height);
        m_seeds.emplace(col, s);
        return s;
    }

    // sample_in (active chunk -> owner column seed)
    int8_t World::Sample(int32_t x, int32_t y, int32_t z)
    {
        Key k{ chunk_of(x), chunk_of(y), chunk_of(z) };
        auto it = m_chunks.find(k);
        if (it != m_chunks.end())
            return it->second[owned_index(x - k[0] * C, y - k[1] * C, z - k[2] * C)];
        return Seed(ColKey{ k[0], k[1] })->Value(x, y, z);
    }

    // benilla-world voxel.rs want + expand_column (CONTRACT §1 surface-column activation)
    void World::Activate(Key const& k, std::vector<Key>* added)
    {
        ColKey col{ k[0], k[1] };
        auto& kzs = m_wanted[col];
        if (kzs.count(k[2]))
            return;
        kzs.insert(k[2]);
        std::shared_ptr<ColumnSeed const> seed = Seed(col);
        // seed_chunk
        Density d(OWNED, AIR);
        int32_t const bx = k[0] * C, by = k[1] * C, bz = k[2] * C;
        for (int32_t z = 0; z < C; ++z)
            for (int32_t y = 0; y < C; ++y)
                for (int32_t x = 0; x < C; ++x)
                    d[owned_index(x, y, z)] = seed->Value(bx + x, by + y, bz + z);
        m_chunks[k] = std::move(d);
        if (added)
            added->push_back(k);
        int32_t lo, hi;
        if (!seed->SurfaceChunks(lo, hi))
            return;
        bool nearSurf = false;
        for (int32_t kz : m_wanted[col])
            if (kz >= lo - 1 && kz <= hi + 1) { nearSurf = true; break; }
        if (nearSurf)
            for (int32_t kz = lo; kz <= hi; ++kz)
                Activate(Key{ col[0], col[1], kz }, added);
    }

    // apply_edit_set
    std::vector<Key> ApplyEditSet(Edit const& e, Chunks& set, Outside const& outside)
    {
        std::vector<Key> changed;
        Key lo, hi;
        e.Bbox(lo, hi);
        float const c[3] = { e.pos[0] / VOXEL_SIZE, e.pos[1] / VOXEL_SIZE, e.pos[2] / VOXEL_SIZE };
        float const r = e.radius / VOXEL_SIZE;
        SoftBrush soft;
        bool const isSoft = e.Soft(soft);
        uint8_t const shape = e.Shape();
        if ((e.mode == 0 || e.mode == 1) && !isSoft)
        {
            for (Key const& k : ChunksOfBox(lo, hi))
            {
                auto it = set.find(k);
                if (it == set.end())
                    continue;
                Density& d = it->second;
                int32_t const base[3] = { k[0] * C, k[1] * C, k[2] * C };
                bool any = false;
                for (int32_t z = std::max(lo[2], base[2]); z <= std::min(hi[2], base[2] + C - 1); ++z)
                    for (int32_t y = std::max(lo[1], base[1]); y <= std::min(hi[1], base[1] + C - 1); ++y)
                        for (int32_t x = std::max(lo[0], base[0]); x <= std::min(hi[0], base[0] + C - 1); ++x)
                        {
                            float const dx = float(x) - c[0], dy = float(y) - c[1], dz = float(z) - c[2];
                            // v3.6: the shape's distance minus r (the sphere keeps its v2 expression bit for bit).
                            float const brush = (shape == SHAPE_SPHERE ? std::sqrt(dx * dx + dy * dy + dz * dz) : ShapeDist(shape, dx, dy, dz)) - r;
                            size_t const idx = owned_index(x - base[0], y - base[1], z - base[2]);
                            float const old = float(d[idx]) / DENSITY_SCALE;
                            // f32::max / f32::min (no NaN can reach here)
                            float const nw = e.mode == 0 ? (old > -brush ? old : -brush) : (old < brush ? old : brush);
                            int8_t const q = quant(nw);
                            if (q != d[idx])
                            {
                                d[idx] = q;
                                any = true;
                            }
                        }
                if (any)
                    changed.push_back(k);
            }
            return changed;
        }

        if (e.mode == MODE_FLATTEN)
        {
            // world.rs MODE_FLATTEN: per-sample. zp lattice plane, band = max(r*0.25, 1), f = min((r-d)/band, 1).
            float const zp = (e.pos[2] + float(int16_t(e.param)) / 100.0f) / VOXEL_SIZE;
            float const s = float(e.strength) / 255.0f;
            float const band = std::max(r * 0.25f, 1.0f);
            for (Key const& k : ChunksOfBox(lo, hi))
            {
                auto it = set.find(k);
                if (it == set.end())
                    continue;
                Density& d = it->second;
                int32_t const base[3] = { k[0] * C, k[1] * C, k[2] * C };
                bool any = false;
                for (int32_t z = std::max(lo[2], base[2]); z <= std::min(hi[2], base[2] + C - 1); ++z)
                {
                    float t = std::round((float(z) - zp) * DENSITY_SCALE);
                    if (t < -128.0f) t = -128.0f;
                    if (t > 127.0f) t = 127.0f;
                    for (int32_t y = std::max(lo[1], base[1]); y <= std::min(hi[1], base[1] + C - 1); ++y)
                        for (int32_t x = std::max(lo[0], base[0]); x <= std::min(hi[0], base[0] + C - 1); ++x)
                        {
                            float const dx = float(x) - c[0], dy = float(y) - c[1], dz = float(z) - c[2];
                            float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                            float f = (r - dist) / band;
                            if (f > 1.0f) f = 1.0f;
                            if (f <= 0.0f)
                                continue;
                            size_t const idx = owned_index(x - base[0], y - base[1], z - base[2]);
                            float const old = float(d[idx]);
                            int8_t const q = quantRaw(old + (t - old) * (s * f));
                            if (q != d[idx])
                            {
                                d[idx] = q;
                                any = true;
                            }
                        }
                }
                if (any)
                    changed.push_back(k);
            }
            return changed;
        }

        // Smooth (hard or soft) + v3.3 soft lower / raise: Jacobi over the pre-edit gather (brush box + one
        // read ring), across chunk borders.
        float const strength = float(e.strength) / 255.0f;
        int32_t const shiftDz = isSoft ? (e.mode == 0 ? 1 : (e.mode == 1 ? -1 : 0)) : 0;
        Key const glo{ lo[0] - 1, lo[1] - 1, lo[2] - 1 }, ghi{ hi[0] + 1, hi[1] + 1, hi[2] + 1 };
        size_t const dim[3] = { size_t(ghi[0] - glo[0] + 1), size_t(ghi[1] - glo[1] + 1), size_t(ghi[2] - glo[2] + 1) };
        auto at = [&](int32_t x, int32_t y, int32_t z) -> size_t
        {
            return size_t(x - glo[0]) + dim[0] * (size_t(y - glo[1]) + dim[1] * size_t(z - glo[2]));
        };
        std::vector<int8_t> grid(dim[0] * dim[1] * dim[2], 0);
        for (int32_t z = glo[2]; z <= ghi[2]; ++z)
            for (int32_t y = glo[1]; y <= ghi[1]; ++y)
                for (int32_t x = glo[0]; x <= ghi[0]; ++x)
                {
                    Key const k{ chunk_of(x), chunk_of(y), chunk_of(z) };
                    auto it = set.find(k);
                    grid[at(x, y, z)] = it != set.end()
                        ? it->second[owned_index(x - k[0] * C, y - k[1] * C, z - k[2] * C)]
                        : outside(x, y, z);
                }
        float const r2 = r * r;
        std::unordered_set<Key, KeyHash> touched;
        std::vector<Key> order;
        for (int32_t z = lo[2]; z <= hi[2]; ++z)
            for (int32_t y = lo[1]; y <= hi[1]; ++y)
                for (int32_t x = lo[0]; x <= hi[0]; ++x)
                {
                    float const dx = float(x) - c[0], dy = float(y) - c[1], dz = float(z) - c[2];
                    float a = strength;
                    if (isSoft)
                    {
                        // v3.6: soft lower / raise weigh by the shape's distance (smooth is always a sphere).
                        float const w = soft.Weight(ShapeDist(shape, dx, dy, dz), r);
                        if (w <= 0.0f)
                            continue;
                        a = strength * w;
                    }
                    else
                    {
                        float const d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 > r2)
                            continue;
                    }
                    Key const k{ chunk_of(x), chunk_of(y), chunk_of(z) };
                    auto it = set.find(k);
                    if (it == set.end())
                        continue;
                    size_t const gi = at(x, y, z), sx = 1, sy = dim[0], sz = dim[0] * dim[1];
                    float const old = float(grid[gi]);
                    float nv;
                    if (shiftDz != 0)
                    {
                        float const n = float(grid[shiftDz > 0 ? gi + sz : gi - sz]);
                        nv = old + (n - old) * a;   // soft lower (+z) / raise (-z): moves the field by a voxels
                    }
                    else
                    {
                        float const avg = (float(grid[gi + sx]) + float(grid[gi - sx]) + float(grid[gi + sy]) + float(grid[gi - sy])
                            + float(grid[gi + sz]) + float(grid[gi - sz])) / 6.0f;
                        nv = isSoft ? old + (avg - old) * a : old * (1.0f - strength) + avg * strength;
                    }
                    int8_t const nw = quantRaw(nv);
                    int32_t const lx = x - k[0] * C, ly = y - k[1] * C, lz = z - k[2] * C;
                    if (lx < 0 || lx >= C || ly < 0 || ly >= C || lz < 0 || lz >= C)
                    {
                        continue;                   // cannot happen (chunk_of); keeps the write provably in bounds
                    }
                    size_t const idx = owned_index(lx, ly, lz);
                    if (nw != it->second[idx])
                    {
                        it->second[idx] = nw;
                        if (touched.insert(k).second)
                            order.push_back(k);
                    }
                }
        return order;
    }

    std::vector<Key> World::ApplyEdit(Edit const& e, std::vector<Key>* added)
    {
        if (!e.IsValid() || !e.IsApplied())
            return {};                              // 4 paint / 5 roughen: journalled, never activate
        for (Key const& k : e.TouchedChunks())
            Activate(k, added);
        return ApplyEditSet(e, m_chunks, [this](int32_t x, int32_t y, int32_t z)
        {
            return Seed(ColKey{ chunk_of(x), chunk_of(y) })->Value(x, y, z);
        });
    }

    // VoxelWorldData::build + VoxelBuildJob::run
    void World::Rebuild(std::vector<Key> const& keys, std::vector<Edit const*> const& edits)
    {
        Chunks set;
        std::vector<Key> live;
        for (Key const& k : keys)
        {
            if (!IsActive(k))
                continue;
            std::shared_ptr<ColumnSeed const> seed = Seed(ColKey{ k[0], k[1] });
            Density d(OWNED, AIR);
            int32_t const bx = k[0] * C, by = k[1] * C, bz = k[2] * C;
            for (int32_t z = 0; z < C; ++z)
                for (int32_t y = 0; y < C; ++y)
                    for (int32_t x = 0; x < C; ++x)
                        d[owned_index(x, y, z)] = seed->Value(bx + x, by + y, bz + z);
            set.emplace(k, std::move(d));
            live.push_back(k);
        }
        if (set.empty())
            return;
        // Outside the set: the other active chunks as they are now, else the seed.
        Outside const outside = [this](int32_t x, int32_t y, int32_t z) { return Sample(x, y, z); };
        for (Edit const* e : edits)
        {
            bool touches = false;
            for (Key const& k : live)
                if (e->Touches(k)) { touches = true; break; }
            if (touches && e->IsValid() && e->IsApplied())
                ApplyEditSet(*e, set, outside);
        }
        for (auto& kv : set)
            m_chunks[kv.first] = std::move(kv.second);
    }

    std::unordered_set<int32_t> const* World::WantedInColumn(ColKey const& col) const
    {
        auto it = m_wanted.find(col);
        return it == m_wanted.end() ? nullptr : &it->second;
    }

    void World::Clear()
    {
        m_seeds.clear();
        m_chunks.clear();
        m_wanted.clear();
    }

    // VoxelWorldData::window_i8 — own 32^3 + the apron from the owning neighbours (active density, else the
    // seed; the seed of a lattice column is a pure function of the column, so the own window serves).
    void World::Window(Key const& k, int8_t* w)
    {
        int32_t const base[3] = { k[0] * C - 1, k[1] * C - 1, k[2] * C - 1 };
        for (int32_t lz = 0; lz < SAMPLES; ++lz)
            for (int32_t ly = 0; ly < SAMPLES; ++ly)
                for (int32_t lx = 0; lx < SAMPLES; ++lx)
                    w[win_index(lx, ly, lz)] = Sample(base[0] + lx, base[1] + ly, base[2] + lz);
    }

    // VoxelWorldData::window_seed_heights (a column's seed serves its own and the neighbours' apron)
    void World::WindowSeedHeights(Key const& k, float* out)
    {
        int32_t const bx = k[0] * C - 1, by = k[1] * C - 1;
        for (int32_t ly = 0; ly < SAMPLES; ++ly)
            for (int32_t lx = 0; lx < SAMPLES; ++lx)
            {
                int32_t const ix = bx + lx, iy = by + ly;
                out[size_t(lx) + size_t(ly) * SAMPLES] = Seed(ColKey{ chunk_of(ix), chunk_of(iy) })->HeightLat(ix, iy);
            }
    }

    // VoxelWorldData::density_at
    float World::DensityAt(float wx, float wy, float wz)
    {
        float const p[3] = { wx / VOXEL_SIZE, wy / VOXEL_SIZE, wz / VOXEL_SIZE };
        int32_t const i0[3] = { int32_t(std::floor(p[0])), int32_t(std::floor(p[1])), int32_t(std::floor(p[2])) };
        float const f[3] = { p[0] - float(i0[0]), p[1] - float(i0[1]), p[2] - float(i0[2]) };
        float acc = 0.0f;
        for (int dz = 0; dz < 2; ++dz)
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    float const w = (dx == 1 ? f[0] : 1.0f - f[0]) * (dy == 1 ? f[1] : 1.0f - f[1]) * (dz == 1 ? f[2] : 1.0f - f[2]);
                    acc += w * float(Sample(i0[0] + dx, i0[1] + dy, i0[2] + dz));
                }
        return acc / DENSITY_SCALE;
    }

    std::array<float, 3> WindowOrigin(Key const& k)
    {
        return { lattice_to_wow(k[0] * C - 1), lattice_to_wow(k[1] * C - 1), lattice_to_wow(k[2] * C - 1) };
    }

    // ------------------------------------------------------------------------- fast-surface-nets 0.2.1
    namespace
    {
        uint32_t const NULL_VERTEX = 0xFFFFFFFFu;
        int const CUBE_CORNERS[8][3] = { {0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1} };
        int const CUBE_EDGES[12][2] = { {0,1},{0,2},{0,4},{1,3},{1,5},{2,3},{2,6},{3,7},{4,5},{4,6},{5,7},{6,7} };
        inline uint32_t lin(int x, int y, int z) { return uint32_t(x + SAMPLES * (y + SAMPLES * z)); }
        // glam Vec3A::distance_squared on SSE2 = (x*x + y*y) + z*z
        inline float dist2(float const* a, float const* b)
        {
            float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            float s = dx * dx + dy * dy;
            return s + dz * dz;
        }
    }

    void SurfaceNets(int8_t const* window, Mesh& out)
    {
        out.pos.clear();
        out.nrm.clear();
        out.idx.clear();
        // No sign change anywhere: no surface, skip the work (the result is the same empty mesh).
        {
            size_t neg = 0;
            for (size_t i = 0; i < WINDOW; ++i)
                if (window[i] < 0)
                    ++neg;
            if (neg == 0 || neg == WINDOW)
                return;
        }
        std::vector<float> sdf(WINDOW);
        for (size_t i = 0; i < WINDOW; ++i)
            sdf[i] = float(window[i]) / DENSITY_SCALE;
        std::vector<uint32_t> strideToIndex(WINDOW, NULL_VERTEX);
        std::vector<std::array<int, 3>> points;
        std::vector<uint32_t> strides;
        std::vector<float> pos;                     // sample-space positions
        std::vector<float> nrm;                     // sdf_gradient, not used by any hash

        int const mn = 0, mx = SAMPLES - 1;         // surface_nets(min 0, max 33)
        for (int z = mn; z < mx; ++z)
            for (int y = mn; y < mx; ++y)
                for (int x = mn; x < mx; ++x)
                {
                    uint32_t const stride = lin(x, y, z);
                    float d[8];
                    int neg = 0;
                    for (int i = 0; i < 8; ++i)
                    {
                        d[i] = sdf[stride + lin(CUBE_CORNERS[i][0], CUBE_CORNERS[i][1], CUBE_CORNERS[i][2])];
                        if (d[i] < 0.0f)
                            ++neg;
                    }
                    if (neg == 0 || neg == 8)
                    {
                        strideToIndex[stride] = NULL_VERTEX;
                        continue;
                    }
                    // centroid_of_edge_intersections
                    int count = 0;
                    float sum[3] = { 0.0f, 0.0f, 0.0f };
                    for (int e = 0; e < 12; ++e)
                    {
                        int const c1 = CUBE_EDGES[e][0], c2 = CUBE_EDGES[e][1];
                        float const d1 = d[c1], d2 = d[c2];
                        if ((d1 < 0.0f) != (d2 < 0.0f))
                        {
                            ++count;
                            float const i1 = d1 / (d1 - d2);
                            float const i2 = 1.0f - i1;
                            for (int a = 0; a < 3; ++a)
                                sum[a] += i2 * float(CUBE_CORNERS[c1][a]) + i1 * float(CUBE_CORNERS[c2][a]);
                        }
                    }
                    float const cnt = float(count);
                    strideToIndex[stride] = uint32_t(pos.size() / 3);
                    float const s[3] = { sum[0] / cnt, sum[1] / cnt, sum[2] / cnt };
                    pos.push_back(float(x) + s[0]);
                    pos.push_back(float(y) + s[1]);
                    pos.push_back(float(z) + s[2]);
                    // sdf_gradient: bilinear between the 4 cube edges along each axis (corner bit0 x, bit1 y, bit2 z)
                    {
                        float const n[3] = { 1.0f - s[0], 1.0f - s[1], 1.0f - s[2] };
                        nrm.push_back(n[1] * n[2] * (d[1] - d[0]) + n[1] * s[2] * (d[5] - d[4]) + s[1] * n[2] * (d[3] - d[2]) + s[1] * s[2] * (d[7] - d[6]));
                        nrm.push_back(n[2] * n[0] * (d[2] - d[0]) + n[2] * s[0] * (d[3] - d[1]) + s[2] * n[0] * (d[6] - d[4]) + s[2] * s[0] * (d[7] - d[5]));
                        nrm.push_back(n[0] * n[1] * (d[4] - d[0]) + n[0] * s[1] * (d[6] - d[2]) + s[0] * n[1] * (d[5] - d[1]) + s[0] * s[1] * (d[7] - d[3]));
                    }
                    points.push_back({ x, y, z });
                    strides.push_back(stride);
                }

        uint32_t const xs = 1, ys = SAMPLES, zs = SAMPLES * SAMPLES;
        auto quad = [&](uint32_t p1, uint32_t p2, uint32_t bs, uint32_t cs)
        {
            float const d1 = sdf[p1], d2 = sdf[p2];
            bool negFace;
            if (d1 < 0.0f && !(d2 < 0.0f)) negFace = false;
            else if (!(d1 < 0.0f) && d2 < 0.0f) negFace = true;
            else return;
            uint32_t const v1 = strideToIndex[p1], v2 = strideToIndex[p1 - bs], v3 = strideToIndex[p1 - cs], v4 = strideToIndex[p1 - bs - cs];
            float const* P1 = &pos[size_t(v1) * 3];
            float const* P2 = &pos[size_t(v2) * 3];
            float const* P3 = &pos[size_t(v3) * 3];
            float const* P4 = &pos[size_t(v4) * 3];
            uint32_t q[6];
            if (dist2(P1, P4) < dist2(P2, P3))
            {
                if (negFace) { uint32_t t[6] = { v1, v4, v2, v1, v3, v4 }; std::copy(t, t + 6, q); }
                else         { uint32_t t[6] = { v1, v2, v4, v1, v4, v3 }; std::copy(t, t + 6, q); }
            }
            else if (negFace) { uint32_t t[6] = { v2, v3, v4, v2, v1, v3 }; std::copy(t, t + 6, q); }
            else              { uint32_t t[6] = { v2, v4, v3, v2, v3, v1 }; std::copy(t, t + 6, q); }
            out.idx.insert(out.idx.end(), q, q + 6);
        };
        for (size_t i = 0; i < points.size(); ++i)
        {
            int const x = points[i][0], y = points[i][1], z = points[i][2];
            uint32_t const p = strides[i];
            if (y != mn && z != mn && x != mx - 1)
                quad(p, p + xs, ys, zs);
            if (x != mn && z != mn && y != mx - 1)
                quad(p, p + ys, zs, xs);
            if (x != mn && y != mn && z != mx - 1)
                quad(p, p + zs, xs, ys);
        }
        out.pos.resize(pos.size());
        for (size_t i = 0; i < pos.size(); ++i)
            out.pos[i] = pos[i] * VOXEL_SIZE;
        out.nrm = std::move(nrm);
    }

    // benilla-formats terrain.rs triangle_z_at
    bool TriangleZAt(float const* a, float const* b, float const* c, float x, float y, float& z)
    {
        float const det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(det) < 1.0e-9f)
            return false;
        float const l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
        float const l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
        float const l3 = 1.0f - l1 - l2;
        if (l1 < 0.0f || l2 < 0.0f || l3 < 0.0f)
            return false;
        z = l1 * a[2] + l2 * b[2] + l3 * c[2];
        return true;
    }

    // world.rs edited_weights
    void EditedWeights(Mesh const& m, int8_t const* win, float const* seedH, float originZ, std::vector<float>& out)
    {
        constexpr float EDITED_FROM = 0.3f, EDITED_RAMP = 0.5f, DOWN_FACING_Z = -0.3f;
        constexpr float EDITED_DENSITY_FROM = 0.25f, EDITED_DENSITY_RAMP = 0.5f;
        int32_t const s = SAMPLES;
        size_t const nv = m.pos.size() / 3;
        out.assign(nv, 0.0f);
        auto at = [&](int32_t x, int32_t y) { return seedH[size_t(std::min(x, s - 1)) + size_t(std::min(y, s - 1)) * s]; };
        auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        int32_t const iz0 = int32_t(std::round(originZ / VOXEL_SIZE));
        std::vector<float> diff(WINDOW);
        for (int32_t z = 0; z < s; ++z)
            for (int32_t y = 0; y < s; ++y)
                for (int32_t x = 0; x < s; ++x)
                {
                    size_t const i = win_index(x, y, z);
                    int8_t const seed = seed_density(at(x, y), iz0 + z);
                    diff[i] = std::fabs(float(win[i]) - float(seed)) / DENSITY_SCALE;
                }
        for (size_t vi = 0; vi < nv; ++vi)
        {
            float const* p = &m.pos[vi * 3];
            float const fx = p[0] / VOXEL_SIZE, fy = p[1] / VOXEL_SIZE;
            int32_t const ix = int32_t(std::max(std::floor(fx), 0.0f)), iy = int32_t(std::max(std::floor(fy), 0.0f));
            float const tx = clamp01(fx - float(ix)), ty = clamp01(fy - float(iy));
            int32_t const iz = std::min(int32_t(std::max(std::floor(p[2] / VOXEL_SIZE), 0.0f)), s - 2);
            int32_t const cx = std::min(ix, s - 2), cy = std::min(iy, s - 2);
            float mx = 0.0f;
            for (int c = 0; c < 8; ++c)
                mx = std::max(mx, diff[win_index(cx + (c & 1), cy + ((c >> 1) & 1), iz + (c >> 2))]);
            float const densityW = clamp01((mx - EDITED_DENSITY_FROM) / EDITED_DENSITY_RAMP);
            if (densityW <= 0.0f)
                continue;
            float const* n = &m.nrm[vi * 3];
            float const nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl > 1.0e-6f && n[2] / nl < DOWN_FACING_Z)
            {
                out[vi] = densityW;
                continue;
            }
            float const hs[4] = { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1) };
            if (!std::isfinite(hs[0]) || !std::isfinite(hs[1]) || !std::isfinite(hs[2]) || !std::isfinite(hs[3]))
            {
                out[vi] = densityW;
                continue;
            }
            float const h = (hs[0] * (1.0f - tx) + hs[1] * tx) * (1.0f - ty) + (hs[2] * (1.0f - tx) + hs[3] * tx) * ty;
            float const gx = ((hs[1] - hs[0]) * (1.0f - ty) + (hs[3] - hs[2]) * ty) / VOXEL_SIZE;
            float const gy = ((hs[2] - hs[0]) * (1.0f - tx) + (hs[3] - hs[1]) * tx) / VOXEL_SIZE;
            float const dz = std::fabs(p[2] + originZ - h) / std::sqrt(1.0f + gx * gx + gy * gy);
            out[vi] = std::min(clamp01((dz - EDITED_FROM) / EDITED_RAMP), densityW);
        }
    }

    // world.rs seed_tilt_cos
    void SeedTiltCos(Mesh const& m, float const* seedH, std::vector<float>& out)
    {
        int32_t const s = SAMPLES;
        size_t const nv = m.pos.size() / 3;
        out.assign(nv, 1.0f);
        auto at = [&](int32_t x, int32_t y) { return seedH[size_t(std::min(x, s - 1)) + size_t(std::min(y, s - 1)) * s]; };
        auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
        for (size_t vi = 0; vi < nv; ++vi)
        {
            float const* p = &m.pos[vi * 3];
            float const* n = &m.nrm[vi * 3];
            float const fx = p[0] / VOXEL_SIZE, fy = p[1] / VOXEL_SIZE;
            int32_t const ix = int32_t(std::max(std::floor(fx), 0.0f)), iy = int32_t(std::max(std::floor(fy), 0.0f));
            float const tx = clamp01(fx - float(ix)), ty = clamp01(fy - float(iy));
            float const hs[4] = { at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1) };
            if (!std::isfinite(hs[0]) || !std::isfinite(hs[1]) || !std::isfinite(hs[2]) || !std::isfinite(hs[3]))
            {
                out[vi] = 0.0f;
                continue;
            }
            float const gx = ((hs[1] - hs[0]) * (1.0f - ty) + (hs[3] - hs[2]) * ty) / VOXEL_SIZE;
            float const gy = ((hs[2] - hs[0]) * (1.0f - tx) + (hs[3] - hs[1]) * tx) / VOXEL_SIZE;
            float const ls = std::sqrt(gx * gx + gy * gy + 1.0f);
            float const ln = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (ln < 1.0e-6f)
                continue;
            out[vi] = (-gx * n[0] - gy * n[1] + n[2]) / (ls * ln);
        }
    }

    uint64_t ChunksHash(Chunks const& chunks)
    {
        std::vector<Key> keys;
        keys.reserve(chunks.size());
        for (auto const& kv : chunks)
            keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        uint64_t h = 0xcbf29ce484222325ull;
        for (Key const& k : keys)
        {
            for (int32_t a : k)
                h = Fnv(&a, 4, h);
            Density const& d = chunks.at(k);
            h = Fnv(d.data(), d.size(), h);
        }
        return h;
    }

    // --------------------------------------------------------------------------------------- Journal
    void Journal::Clear()
    {
        m_edits.clear();
        m_index.clear();
    }

    void Journal::Add(Edit const& e)
    {
        m_edits.push_back(e);
        if (e.IsValid())
            for (Key const& k : e.TouchedChunks())
                m_index[k].push_back(uint32_t(m_edits.size() - 1));
    }

    void Journal::Reindex()
    {
        m_index.clear();
        for (size_t i = 0; i < m_edits.size(); ++i)
            if (m_edits[i].IsValid())
                for (Key const& k : m_edits[i].TouchedChunks())
                    m_index[k].push_back(uint32_t(i));
    }

    std::vector<Edit const*> Journal::EditsFor(std::vector<Key> const& keys) const
    {
        std::set<uint32_t> ids;
        for (Key const& k : keys)
        {
            auto it = m_index.find(k);
            if (it != m_index.end())
                ids.insert(it->second.begin(), it->second.end());
        }
        std::vector<Edit const*> out;
        out.reserve(ids.size());
        for (uint32_t i : ids)
            out.push_back(&m_edits[i]);
        return out;
    }

    std::set<Key> Journal::ReadClosure(std::vector<Key> const& keys) const
    {
        std::set<Key> set(keys.begin(), keys.end());
        std::vector<Key> queue(set.begin(), set.end());
        while (!queue.empty())
        {
            Key const k = queue.back();
            queue.pop_back();
            auto it = m_index.find(k);
            if (it == m_index.end())
                continue;
            for (uint32_t i : it->second)
            {
                Edit const& e = m_edits[i];
                if (!e.ReadsNeighbours())
                    continue;
                for (Key const& t : e.TouchedChunks())
                    if (set.insert(t).second)
                        queue.push_back(t);
            }
        }
        return set;
    }

    uint64_t Fnv(void const* data, size_t n, uint64_t h)
    {
        unsigned char const* p = static_cast<unsigned char const*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }
}
