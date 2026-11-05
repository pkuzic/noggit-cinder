// This file is part of Noggit3, licensed under GNU General Public License (version 3).
//
// Golden-vector check of the voxel field against the game client's output, voxel-core/golden-v3.6.txt
// (written by benilla-voxel tests/golden_v3.rs). Same synthetic height function, same edit list (read back
// from the file by bit pattern), same activation; every chunk's density + mesh FNV, the down-ray height
// hash, the density hash and the closure rule must match. Runs in the standalone test exe and, through
// the smoke hook, inside noggit.exe on the very object code the editor uses.
#include <noggit/everwood/VoxelField.hpp>

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
    std::string closureLine, activeLine;
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.rfind("edit ", 0) == 0)
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
      else if (line.rfind("closure_rebuild", 0) == 0)
      {
        closureLine = line;
      }
      else if (line.rfind("active_chunks", 0) == 0)
      {
        activeLine = line;
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

    // The canonical build of the whole set.
    {
      std::vector<Edit const*> all;
      for (Edit const& e : journal.Edits())
        all.push_back(&e);
      w.Rebuild(keys, all);
    }

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

    // The closure rule: rebuilding each edit's closure reproduces the full replay.
    size_t closureSets = 0, closureBad = 0;
    for (Edit const& e : journal.Edits())
    {
      std::vector<Key> const touched = e.TouchedChunks();
      std::vector<Key> set;
      for (Key const& k : journal.ReadClosure(touched))
        if (w.IsActive(k))
          set.push_back(k);
      // The set is reseeded and replayed in place, the rest of the world stays as it is.
      w.Rebuild(set, journal.EditsFor(set));
      ++closureSets;
      if (ChunksHash(w.ChunksRef()) != fullHash)
        ++closureBad;
    }

    // The incremental path the editor's brush uses (activate + apply, edit by edit) gives the same field.
    World inc(goldenHeight);
    for (Edit const& e : edits)
      inc.ApplyEdit(e);
    // Same activation set as the canonical run: compare densities on it.
    for (Key const& k : keys)
      inc.Activate(k, nullptr);
    uint64_t const incHash = ChunksHash(inc.ChunksRef());
    bool const incOk = incHash == fullHash;

    out << "golden lines compared " << count << ", differences " << diffs << "\n";
    out << "density_hash " << hex16(fullHash) << "\n";
    out << "closure sets " << closureSets << ", differing " << closureBad
        << " (file says: " << closureLine << ")\n";
    out << "incremental replay hash " << hex16(incHash) << (incOk ? " == " : " != ") << "canonical\n";
    out << "active chunks " << keys.size() << " (file says: " << activeLine << ")\n";
    report = out.str();
    return diffs == 0 && closureBad == 0 && incOk;
  }
}
