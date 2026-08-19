// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/Log.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <streambuf>
#include <string>

std::ostream& _LogError(const char * pFile, int pLine)
{
  return std::cerr << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Error] ";
}
std::ostream& _LogDebug(const char * pFile, int pLine)
{
  return std::clog << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): [Debug] ";
}
std::ostream& _Log(const char * pFile, int pLine)
{
  return std::cout << clock() * 1000 / CLOCKS_PER_SEC << " - (" << ((strrchr(pFile, '/') ? strrchr(pFile, '/') : (strrchr(pFile, '\\') ? strrchr(pFile, '\\') : pFile - 1)) + 1) << ":" << pLine << "): ";
}

#if DEBUG__LOGGINGTOCONSOLE
void InitLogging()
{
  LogDebug << "Logging to console window." << std::endl;
}
#else
namespace
{
  // The old setup pointed cout/clog/cerr at ONE shared std::filebuf. std::filebuf is NOT
  // thread-safe, and noggit logs concurrently from the async loader threads, the tile update
  // thread and the main thread (worst case: the ~600-line doodad-UID burst while the Stormwind
  // tile loads). Racing operator<< on a shared filebuf corrupts its put-area pointers -> writes
  // past its internal buffer -> HEAP CORRUPTION that later kills unrelated code (observed as
  // poisoned Qt object internals + wild -1 pointer reads), and interleaved/lost lines.
  //
  // This streambuf assembles each THREAD's output in a thread_local line and appends complete
  // lines to the file under a mutex, flushing immediately -- so concurrent logging is safe,
  // lines never interleave mid-line, and a crash cannot lose the tail of the log.
  class SyncedLineBuf : public std::streambuf
  {
  public:
    explicit SyncedLineBuf(std::ofstream* file) : _file(file) {}

    int_type overflow(int_type ch) override
    {
      if (traits_type::eq_int_type(ch, traits_type::eof()))
        return traits_type::not_eof(ch);
      std::string& line = buffer();
      line.push_back(traits_type::to_char_type(ch));
      if (traits_type::to_char_type(ch) == '\n')
        emit_line(line);
      return ch;
    }

    int sync() override
    {
      std::string& line = buffer();
      if (!line.empty())
        emit_line(line);
      return 0;
    }

  private:
    static std::string& buffer()
    {
      thread_local std::string line;
      return line;
    }

    void emit_line(std::string& line)
    {
      std::lock_guard<std::mutex> const lock(_mutex);
      _file->write(line.data(), static_cast<std::streamsize>(line.size()));
      _file->flush();
      line.clear();
    }

    std::ofstream* _file;
    std::mutex _mutex;
  };
}

void InitLogging()
{
  // deliberately leaked: iostreams flush during static destruction, so the sink must outlive
  // every static object -- a heap allocation that is never freed is the only safe lifetime.
  auto* log_stream = new std::ofstream("log.txt", std::ios_base::out | std::ios_base::trunc);
  if (*log_stream)
  {
    auto* synced = new SyncedLineBuf(log_stream);
    std::cout.rdbuf(synced);
    std::clog.rdbuf(synced);
    std::cerr.rdbuf(synced);
  }
}
#endif
