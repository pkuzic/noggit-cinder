// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Golden-vector check of the voxel field against the game client's output, voxel-core/golden-v3.11.txt (or
// v3.10 / v3.9 / v3.6) (written by benilla-voxel tests/golden_v3.rs). Same synthetic height function, same edit list
// (read back from the file by bit pattern), same activation; every chunk's density + mesh FNV, the down-ray
// height hash, the density hash and the closure rule must match. CONTRACT v3.10 files also carry the mode-7
// 3D paint lines (p3param, paint3_chunks, paint3, paint3_sample, paint3_hash): the canonical paint, the
// closure rebuild (density AND paint), the incremental path and the out-of-order dirtying must reproduce
// them. CONTRACT v3.11 files (smooth passes, 16 yd + HARD paint) add `hard` to p3param and per-seq `case` lines
// (density + paint hash of the pipeline over edits 1..=seq); older files (<= v3.10) are replayed through the game's
// v310_compat shim (golden_v3.rs): the pass bits of mode 2 and bit 15 of mode 7 are cleared and a mode-7 radius in
// (8, 16] becomes 16.5, so every RESULT line keeps its frozen value. Runs in the standalone test exe and, through the smoke hook, inside noggit.exe on the very object
// code the editor uses.
#include <noggit/everwood/VoxelField.hpp>
#include <noggit/everwood/VoxelPaint3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace Noggit::Everwood::Voxel
{
  namespace
  {
    // golden_v3.rs height: arithmetic only (+,*), bit-identical in Rust and MSVC by construction.
    bool goldenHeight(float x, float y, float& h)
    {
      float const dx = x + 8940.0f;
      float const dy = y + 70.0f;
      if (dx > 9.0f && dx < 11.0f && dy > -3.0f && dy < -1.0f)
        return false;
      h = 80.0f + 0.0625f * dx - 0.03125f * dy + 0.0009765625f * dx * dy - 0.00048828125f * dx * dx;
      return true;
    }

    float bitsToFloat(uint32_t u)
    {
      float f;
      std::memcpy(&f, &u, 4);
      return f;
    }

    uint32_t floatToBits(float f)
    {
      uint32_t u;
      std::memcpy(&u, &f, 4);
      return u;
    }

    std::string hex16(uint64_t v)
    {
      char b[32];
      std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
      return b;
    }

    // golden_v3.rs SAMPLES3: the 12 paint3_sample lattice points.
    int32_t const SAMPLES3[12][3] = {
      { -17800, -80, 155 }, { -17800, -80, 153 }, { -17800, -80, 161 }, { -17800, -80, 165 },
      { -17796, -80, 165 }, { -17792, -80, 155 }, { -17793, -80, 155 }, { -17812, -80, 155 },
      { -17800, -80, 140 }, { -17794, -74, 156 }, { -17794, -86, 156 }, { -17804, -84, 155 },
    };

    std::array<uint8_t, 4> rgbaAt(Paint3Map const& paint, int32_t x, int32_t y, int32_t z)
    {
      Key const k{ chunk_of(x), chunk_of(y), chunk_of(z) };
      auto it = paint.find(k);
      if (it == paint.end())
        return { 0, 0, 0, 0 };
      return it->second.Rgba(owned_index(x - k[0] * C, y - k[1] * C, z - k[2] * C));
    }

    bool samePaint(ChunkPaint3 const* a, ChunkPaint3 const* b)
    {
      bool const ea = !a || a->IsEmpty(), eb = !b || b->IsEmpty();
      if (ea || eb)
        return ea == eb;
      return *a == *b;
    }
  }

  namespace
  {
    // golden_v3.rs prefix(): the golden pipeline (activation + canonical build + canonical paint) over `edits`.
    void pipeline(std::vector<Edit> const& edits, uint64_t& density, uint64_t& paint)
    {
      World w(goldenHeight);
      for (Edit const& e : edits)
        if (e.IsApplied())
          for (Key const& k : e.TouchedChunks())
            w.Activate(k, nullptr);
      std::vector<Key> keys;
      for (auto const& kv : w.ChunksRef())
        keys.push_back(kv.first);
      std::sort(keys.begin(), keys.end());
      Journal j;
      for (Edit const& e : edits)
        j.Add(e);
      std::vector<Edit const*> all;
      for (Edit const& e : j.Edits())
        all.push_back(&e);
      w.RebuildPaint(keys, j.Paint3Keys(), all);
      density = ChunksHash(w.ChunksRef());
      paint = Paint3Hash(w.PaintRef());
    }

    // v3.11 partial rebuild: an earlier dig A in chunk X+1 next to the border, an 8-pass smooth B whose brush ends
    // 2 voxels from the border (inside the 8-voxel grown gather), a soft 4-pass smooth, a later build C. Rebuilding
    // only B's read closure (alone, or as the undo / redo of C) must give the full replay's bytes.
    bool partialRebuildCheck(std::ostringstream& out)
    {
      auto mk = [](uint32_t seq, float x, float y, float z, float r, uint8_t mode, uint8_t str, uint16_t param)
      {
        Edit e;
        e.seq = seq;
        e.pos[0] = x; e.pos[1] = y; e.pos[2] = z;
        e.radius = r; e.mode = mode; e.strength = str; e.param = param;
        return e;
      };
      // Chunk border at lattice -17856 = x -8928 yd (chunks -559 | -558). z 80 yd = lattice 160 (chunk 5).
      std::vector<Edit> const base = {
        mk(1, -8927.0f, -70.0f, 80.0f, 3.0f, MODE_DIG, 255, 0),
        mk(2, -8932.0f, -70.0f, 80.0f, 3.0f, MODE_SMOOTH, 255, uint16_t(3 << SMOOTH_PASSES_SHIFT)),
        mk(3, -8931.0f, -69.0f, 80.5f, 4.0f, MODE_SMOOTH, 200, uint16_t(SOFT_FLAG | (2 << SMOOTH_PASSES_SHIFT))),
        mk(4, -8922.0f, -70.0f, 80.0f, 3.0f, MODE_BUILD, 255, 0),
      };
      bool ok = true;
      uint64_t fullD = 0, fullP = 0;
      pipeline(base, fullD, fullP);

      World w(goldenHeight);
      for (Edit const& e : base)
        for (Key const& k : e.TouchedChunks())
          w.Activate(k, nullptr);
      std::vector<Key> keys;
      for (auto const& kv : w.ChunksRef())
        keys.push_back(kv.first);
      Journal j;
      for (Edit const& e : base)
        j.Add(e);
      std::vector<Edit const*> all;
      for (Edit const& e : j.Edits())
        all.push_back(&e);
      w.Rebuild(keys, all);
      uint64_t const fullHash = ChunksHash(w.ChunksRef());
      if (fullHash != fullD)
        ok = false;

      size_t bad = 0, rebuilds = 0;
      // (1) the read closure of every chunk B touches, rebuilt alone, on top of the finished world.
      for (Key const& t : base[1].TouchedChunks())
      {
        std::set<Key> const clo = j.ReadClosure({ t });
        std::vector<Key> set, cv(clo.begin(), clo.end());
        for (Key const& k : clo)
          if (w.IsActive(k))
            set.push_back(k);
        w.Rebuild(set, j.EditsFor(cv));
        ++rebuilds;
        if (ChunksHash(w.ChunksRef()) != fullHash)
          ++bad;
      }
      // (2) undo C (rebuild its closure without it), then redo it.
      {
        size_t const cPos = base.size() - 1;
        Journal j2;
        for (Edit const& e : base)
          j2.Add(e);
        j2.Remove(cPos);
        std::vector<Key> const ct = base[cPos].TouchedChunks();
        for (int pass = 0; pass < 2; ++pass)
        {
          std::set<Key> const clo = j2.ReadClosure(ct);
          std::vector<Key> set, cv(clo.begin(), clo.end());
          for (Key const& k : clo)
            if (w.IsActive(k))
              set.push_back(k);
          w.Rebuild(set, j2.EditsFor(cv));
          ++rebuilds;
          if (pass == 0)
            j2.Insert(cPos, base[cPos]);        // redo
          else if (ChunksHash(w.ChunksRef()) != fullHash)
            ++bad;
        }
      }
      if (bad)
        ok = false;
      out << "v3.11 partial rebuild (8-pass smooth next to a chunk with an earlier edit): " << rebuilds
          << " closure rebuilds, differing " << bad << " (full " << hex16(fullHash) << ")\n";
      return ok;
    }
  }

  bool RunGolden(char const* path, std::string& report)
  {
    std::ifstream in(path);
    if (!in)
    {
      report = std::string("cannot open ") + path;
      return false;
    }

    std::vector<Edit> edits;
    std::vector<std::string> expected;       // every chunk / height / heights / density_hash line
    std::vector<std::string> expectedP3;     // CONTRACT v3.10: every p3param / paint3* line
    std::vector<std::string> expectedCases;  // CONTRACT v3.11: every `case <seq> density_hash .. paint3_hash ..` line
    bool v311 = false;                       // header names v3.11: the edit list is read with v3.11 meanings
    std::string closureLine, activeLine;
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.rfind("# MONKEY voxel golden v3.11", 0) == 0)
        v311 = true;
      if (line.rfind("case ", 0) == 0)
      {
        expectedCases.push_back(line);
      }
      else if (line.rfind("edit ", 0) == 0)
      {
        unsigned seq, mode, strength;
        unsigned x, y, z, r;
        int param;
        if (std::sscanf(line.c_str(), "edit %u %u %x %x %x %x %u %d", &seq, &mode, &x, &y, &z, &r, &strength, &param) != 8)
        {
          report = "bad edit line: " + line;
          return false;
        }
        Edit e;
        e.seq = seq;
        e.mode = uint8_t(mode);
        e.pos[0] = bitsToFloat(x);
        e.pos[1] = bitsToFloat(y);
        e.pos[2] = bitsToFloat(z);
        e.radius = bitsToFloat(r);
        e.strength = uint8_t(strength);
        e.param = uint16_t(int16_t(param));
        edits.push_back(e);
      }
      else if (line.rfind("chunk ", 0) == 0 || line.rfind("height", 0) == 0 || line.rfind("density_hash", 0) == 0 || line.rfind("chunks ", 0) == 0)
      {
        expected.push_back(line);
      }
      else if (line.rfind("p3param ", 0) == 0 || line.rfind("paint3", 0) == 0)
      {
        expectedP3.push_back(line);
      }
      else if (line.rfind("closure_rebuild", 0) == 0)
      {
        closureLine = line;
      }
      else if (line.rfind("active_chunks", 0) == 0)
      {
        activeLine = line;
      }
    }

    // v3.10 and older files: the game's v310_compat shim (echoEdits = the inputs as written, for the p3param echo).
    std::vector<Edit> const echoEdits = edits;
    if (!v311)
      for (Edit& e : edits)
      {
        if (e.mode == MODE_SMOOTH)
          e.param = uint16_t(e.param & ~SMOOTH_PASSES_MASK);
        if (e.mode == MODE_PAINT3)
        {
          e.param = uint16_t(e.param & ~PAINT3_HARD);
          if (e.radius > 8.0f && e.radius <= 16.0f)
            e.radius = 16.5f;
        }
      }

    // golden_v3.rs activate: the client's want + expand_column over every applied edit.
    World w(goldenHeight);
    for (Edit const& e : edits)
      if (e.IsApplied())
        for (Key const& k : e.TouchedChunks())
          w.Activate(k, nullptr);

    std::vector<Key> keys;
    for (auto const& kv : w.ChunksRef())
      keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());

    Journal journal;
    for (Edit const& e : edits)
      journal.Add(e);

    // The canonical build of the whole set (+ v3.10: the canonical 3D paint of every chunk a dab touches,
    // golden_v3.rs golden_paint: the dabs replayed in place, outside the active set = the seed).
    std::vector<Key> const paintKeys = journal.Paint3Keys();
    {
      std::vector<Edit const*> all;
      for (Edit const& e : journal.Edits())
        all.push_back(&e);
      w.RebuildPaint(keys, paintKeys, all);
    }
    Paint3Map const fullPaint = w.PaintRef();
    uint64_t const fullPaintHash = Paint3Hash(fullPaint);

    std::vector<std::string> got;
    got.push_back("chunks " + std::to_string(keys.size()));
    std::vector<std::array<std::array<float, 3>, 3>> tris;
    std::vector<int8_t> window(WINDOW);
    for (Key const& k : keys)
    {
      w.Window(k, window.data());
      Mesh m;
      SurfaceNets(window.data(), m);
      uint64_t h = 0xcbf29ce484222325ull;
      for (float v : m.pos)
      {
        uint32_t const u = floatToBits(v);
        h = Fnv(&u, 4, h);
      }
      for (uint32_t i : m.idx)
        h = Fnv(&i, 4, h);
      Density const& d = w.ChunksRef().at(k);
      got.push_back("chunk " + std::to_string(k[0]) + " " + std::to_string(k[1]) + " " + std::to_string(k[2]) + " "
                    + hex16(Fnv(d.data(), d.size())) + " mesh " + hex16(h)
                    + " v" + std::to_string(m.pos.size() / 3) + " t" + std::to_string(m.idx.size() / 3));
      auto const o = WindowOrigin(k);
      for (size_t t = 0; t + 2 < m.idx.size(); t += 3)
      {
        std::array<std::array<float, 3>, 3> tri;
        for (int c = 0; c < 3; ++c)
        {
          size_t const i = m.idx[t + c];
          tri[c] = { o[0] + m.pos[i * 3], o[1] + m.pos[i * 3 + 1], o[2] + m.pos[i * 3 + 2] };
        }
        tris.push_back(tri);
      }
    }

    uint64_t h = 0xcbf29ce484222325ull;
    int n = 0;
    for (int j = 0; j < 48; ++j)
      for (int i = 0; i < 48; ++i)
      {
        float const x = -8964.0f + float(i) * 1.03f;
        float const y = -94.0f + float(j) * 1.03f;
        bool found = false;
        float best = 0.0f;
        for (auto const& t : tris)
        {
          float z;
          if (TriangleZAt(t[0].data(), t[1].data(), t[2].data(), x, y, z) && (!found || z > best))
          {
            best = z;
            found = true;
          }
        }
        float const z = found ? best : -200000.0f;
        if (found)
          ++n;
        uint32_t const u = floatToBits(z);
        h = Fnv(&u, 4, h);
        if ((i + j * 48) % 397 == 0)
        {
          char b[64];
          std::snprintf(b, sizeof(b), "height %d %d %.5f", i, j, double(z));
          got.push_back(b);
        }
      }
    got.push_back("heights " + std::to_string(n) + " " + hex16(h));
    uint64_t const fullHash = ChunksHash(w.ChunksRef());
    got.push_back("density_hash " + hex16(fullHash));

    // Compare, order-insensitively within the file's own order (both lists follow the golden's order).
    std::ostringstream out;
    size_t diffs = 0;
    size_t const count = std::min(got.size(), expected.size());
    for (size_t i = 0; i < count; ++i)
    {
      if (got[i] != expected[i])
      {
        if (diffs < 8)
          out << "DIFF expected '" << expected[i] << "' got '" << got[i] << "'\n";
        ++diffs;
      }
    }
    if (got.size() != expected.size())
    {
      out << "DIFF line count expected " << expected.size() << " got " << got.size() << "\n";
      ++diffs;
    }

    // CONTRACT v3.10 paint3 lines (golden_v3.rs v310_paint3_vectors_replay_exactly_and_are_written).
    std::vector<std::string> gotP3;
    for (size_t i = 0; i < edits.size(); ++i)
    {
      Edit const& e = v311 ? edits[i] : echoEdits[i];          // old files echo the inputs as written
      if (!IsPaint3(e))
        continue;
      Paint3Param const pp = Paint3Param::FromParam(e.param);
      char b[192];
      std::snprintf(b, sizeof(b), "p3param %u %04x id %u falloff %u erase %u reveal %u valid %u", unsigned(e.seq), unsigned(e.param),
                    unsigned(pp.id), unsigned(pp.falloff), unsigned(pp.erase), unsigned(pp.reveal), unsigned(edits[i].IsValid()));
      std::string l = b;
      if (v311)
        l += " hard " + std::to_string(pp.hard);
      gotP3.push_back(l);
    }
    {
      std::vector<Key> pk;
      for (auto const& kv : fullPaint)
        pk.push_back(kv.first);
      std::sort(pk.begin(), pk.end());
      gotP3.push_back("paint3_chunks " + std::to_string(pk.size()));
      for (Key const& k : pk)
      {
        ChunkPaint3 const& c = fullPaint.at(k);
        std::string mats;
        for (size_t i = 0; i < c.mats.size(); ++i)
          mats += (i ? "," : "") + std::to_string(c.mats[i]);
        gotP3.push_back("paint3 " + std::to_string(k[0]) + " " + std::to_string(k[1]) + " " + std::to_string(k[2]) + " " + mats + " "
                        + hex16(c.Hash()));
      }
      for (auto const& sp : SAMPLES3)
      {
        auto const v = rgbaAt(fullPaint, sp[0], sp[1], sp[2]);
        char b[128];
        std::snprintf(b, sizeof(b), "paint3_sample %d %d %d %u %u %u %u", sp[0], sp[1], sp[2], unsigned(v[0]), unsigned(v[1]),
                      unsigned(v[2]), unsigned(v[3]));
        gotP3.push_back(b);
      }
      gotP3.push_back("paint3_hash " + hex16(fullPaintHash));
    }
    bool const hasP3 = !expectedP3.empty();
    size_t p3Match = 0, p3Diff = 0;
    if (hasP3)
    {
      size_t const n3 = std::min(gotP3.size(), expectedP3.size());
      for (size_t i = 0; i < n3; ++i)
      {
        if (gotP3[i] == expectedP3[i])
          ++p3Match;
        else
        {
          if (p3Diff < 8)
            out << "DIFF expected '" << expectedP3[i] << "' got '" << gotP3[i] << "'\n";
          ++p3Diff;
        }
      }
      if (gotP3.size() != expectedP3.size())
      {
        out << "DIFF paint3 line count expected " << expectedP3.size() << " got " << gotP3.size() << "\n";
        ++p3Diff;
      }
    }

    // The closure rule, density AND paint: rebuilding each edit's read closure (+ the dabs' own chunks as
    // paint keys) reproduces the full replay.
    size_t closureSets = 0, closureBad = 0, closurePaintBad = 0, closureChunks = 0;
    std::set<Key> const paintKeySet(paintKeys.begin(), paintKeys.end());
    for (Edit const& e : journal.Edits())
    {
      std::vector<Key> const touched = e.TouchedChunks();
      std::set<Key> const closure = journal.ReadClosure(touched);
      std::vector<Key> set, pk, all(closure.begin(), closure.end());
      for (Key const& k : closure)
      {
        if (w.IsActive(k))
          set.push_back(k);
        if (paintKeySet.count(k))
          pk.push_back(k);
      }
      // The set is reseeded and replayed in place, the rest of the world stays as it is.
      w.RebuildPaint(set, pk, journal.EditsFor(all));
      ++closureSets;
      closureChunks += set.size();
      if (ChunksHash(w.ChunksRef()) != fullHash)
        ++closureBad;
      for (Key const& k : pk)
      {
        auto it = fullPaint.find(k);
        if (!samePaint(w.PaintOf(k), it == fullPaint.end() ? nullptr : &it->second))
          ++closurePaintBad;
      }
    }

    // The incremental path the editor's brush uses (activate + apply, edit by edit) gives the same field.
    World inc(goldenHeight);
    for (Edit const& e : edits)
    {
      if (IsPaint3(e))
        inc.ApplyPaint3(e);                 // reads the live field: exact, every dab is the newest edit
      else
        inc.ApplyEdit(e);
    }
    // Same activation set as the canonical run: compare densities on it.
    for (Key const& k : keys)
      inc.Activate(k, nullptr);
    uint64_t const incHash = ChunksHash(inc.ChunksRef());
    uint64_t const incPaintHash = Paint3Hash(inc.PaintRef());
    bool const incOk = incHash == fullHash && incPaintHash == fullPaintHash && inc.PaintRef().size() == fullPaint.size();

    // Out of order (v3.10): the ceiling-opening dig (seq 142) arrives LAST. Its insert dirties exactly what
    // must replay; rebuilding the dirty chunks (their read closure) restores the full replay. Then: a density
    // edit appended after every dab dirties nothing, an appended dab dirties exactly its own chunks.
    bool oooOk = true;
    std::string oooText;
    size_t oooPos = edits.size();
    for (size_t i = 0; i < edits.size(); ++i)
      if (edits[i].seq == 142 && !IsPaint3(edits[i]))
        oooPos = i;
    if (hasP3 && oooPos < edits.size())
    {
      Journal j2;
      for (size_t i = 0; i < edits.size(); ++i)
        if (i != oooPos)
          j2.Add(edits[i]);
      j2.TakePaint3Dirty();
      j2.Insert(oooPos, edits[oooPos]);
      std::set<Key> const dirtySet = j2.TakePaint3Dirty();
      std::vector<Key> const dirty(dirtySet.begin(), dirtySet.end());
      bool const mainDirty = dirtySet.count(Key{ -557, -3, 4 }) != 0;
      std::set<Key> const closure = j2.ReadClosure(dirty);
      std::vector<Key> const closureV(closure.begin(), closure.end());

      World w2(goldenHeight);
      for (Key const& k : keys)
        w2.Activate(k, nullptr);
      std::vector<Edit const*> stalePtrs, allPtrs;
      for (size_t i = 0; i < edits.size(); ++i)
      {
        allPtrs.push_back(&edits[i]);
        if (i != oooPos)
          stalePtrs.push_back(&edits[i]);
      }
      w2.RebuildPaint(keys, paintKeys, stalePtrs);           // the paint without the opening
      uint64_t const staleHash = Paint3Hash(w2.PaintRef());
      w2.Rebuild(keys, allPtrs);                              // the final density, the stale paint
      std::vector<Key> set;
      for (Key const& k : closure)
        if (w2.IsActive(k))
          set.push_back(k);
      w2.RebuildPaint(set, dirty, j2.EditsFor(closureV));
      uint64_t const fixedHash = Paint3Hash(w2.PaintRef());

      Edit tail{};
      for (Edit const& e : edits)
        if (e.seq == 141)
          tail = e;
      tail.seq = 1000;
      j2.Add(tail);
      bool const tailClean = j2.TakePaint3Dirty().empty();
      Edit tailDab{};
      for (Edit const& e : edits)
        if (e.seq == 143)
          tailDab = e;
      tailDab.seq = 1001;
      j2.Add(tailDab);
      std::set<Key> const dabDirty = j2.TakePaint3Dirty();
      std::vector<Key> const own = tailDab.TouchedChunks();
      bool const dabOk = dabDirty == std::set<Key>(own.begin(), own.end());

      oooOk = mainDirty && staleHash != fullPaintHash && fixedHash == fullPaintHash && tailClean && dabOk;
      std::ostringstream o;
      o << "out-of-order seq 142: dirty " << dirty.size() << " (main chunk " << (mainDirty ? "yes" : "NO") << "), closure "
        << closure.size() << ", stale paint " << hex16(staleHash) << (staleHash != fullPaintHash ? " != " : " == ")
        << "full, dirty rebuild " << hex16(fixedHash) << (fixedHash == fullPaintHash ? " == " : " != ") << "full; tail edit dirties "
        << (tailClean ? "nothing" : "SOMETHING") << ", tail dab dirties " << (dabOk ? "exactly its chunks" : "WRONG SET") << "\n";
      oooText = o.str();
    }

    // CONTRACT v3.11 case lines + the partial rebuild next to a chunk with an earlier edit.
    size_t caseDiff = 0;
    for (std::string const& exp : expectedCases)
    {
      unsigned seq = 0;
      if (std::sscanf(exp.c_str(), "case %u ", &seq) != 1 || seq == 0 || seq > edits.size())
      {
        out << "DIFF bad case line '" << exp << "'\n";
        ++caseDiff;
        continue;
      }
      std::vector<Edit> pre(edits.begin(), edits.begin() + seq);
      uint64_t d = 0, p = 0;
      pipeline(pre, d, p);
      std::string const gotCase = "case " + std::to_string(seq) + " density_hash " + hex16(d) + " paint3_hash " + hex16(p);
      if (gotCase != exp)
      {
        if (caseDiff < 8)
          out << "DIFF expected '" << exp << "' got '" << gotCase << "'\n";
        ++caseDiff;
      }
    }
    bool partialOk = true;
    if (v311)
      partialOk = partialRebuildCheck(out);
    if (!expectedCases.empty())
      out << "case lines compared " << expectedCases.size() << ", differing " << caseDiff << "\n";
    out << (v311 ? "edit list read with v3.11 meanings\n" : "edit list replayed through the v3.10 compat shim\n");

    out << "golden lines compared " << count << ", differences " << diffs << "\n";
    out << "density_hash " << hex16(fullHash) << "\n";
    char avgText[32];
    std::snprintf(avgText, sizeof(avgText), "%.1f", closureSets ? double(float(closureChunks) / float(closureSets)) : 0.0);
    out << "closure sets " << closureSets << " avg_chunks " << avgText << ", differing " << closureBad
        << " (file says: " << closureLine << ")\n";
    out << "incremental replay hash " << hex16(incHash) << (incHash == fullHash ? " == " : " != ") << "canonical\n";
    out << "active chunks " << keys.size() << " (file says: " << activeLine << ")\n";
    if (hasP3)
    {
      out << "paint3 lines compared " << expectedP3.size() << ": matching " << p3Match << ", differing " << p3Diff << "\n";
      out << "paint3_hash " << hex16(fullPaintHash) << " (" << fullPaint.size() << " painted chunks)\n";
      out << "closure paint chunks differing " << closurePaintBad << "\n";
      out << "incremental paint hash " << hex16(incPaintHash) << (incPaintHash == fullPaintHash ? " == " : " != ") << "canonical\n";
      out << oooText;
    }
    report = out.str();
    return diffs == 0 && closureBad == 0 && incOk && p3Diff == 0 && closurePaintBad == 0 && oooOk && caseDiff == 0 && partialOk;
  }
}
