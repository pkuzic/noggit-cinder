#include <MPQArchive.hpp>
#include <Exception.hpp>
#include <StormLib.h>

#include <algorithm>
#include <string>


using namespace BlizzardArchive::Archive;

MPQArchive::MPQArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile)
: BaseArchive(path, locale, listfile)
{
  if (!SFileOpenArchive(path.c_str(), 0, MPQ_OPEN_NO_LISTFILE | STREAM_FLAG_READ_ONLY, &_handle))
  {
    DWORD nError = GetLastError();
    throw Exceptions::Archive::ArchiveOpenError("Error opening archive: " + path + "\nMake sure it isn't opened by another tool.");
  }

  // handle listfiles
  HANDLE fh;
  if (SFileOpenFileEx(_handle, "(listfile)", 0, &fh))
  {
    size_t filesize = SFileGetFileSize(fh, nullptr);

    // SFILE_INVALID_SIZE (0xFFFFFFFF) means the size query FAILED -- allocating it as a real
    // size means a 4 GB buffer and a garbage read (see ClientData::readFile).
    if (filesize == 0 || filesize == 0xFFFFFFFFull || filesize > (256ull * 1024ull * 1024ull))
    {
      SFileCloseFile(fh);
      return;
    }

    std::vector<char> readbuffer(filesize);
    if (SFileReadFile(fh, readbuffer.data(), static_cast<DWORD>(filesize), nullptr, nullptr))
    {
      listfile->initFromFileList(readbuffer);
    }
    SFileCloseFile(fh);
  }
  else
  {
      // DWORD nError = GetLastError();
      return;
  }
}

bool MPQArchive::openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const
{
  assert(file_key.hasFilepath());
  return SFileOpenFileEx(_handle, ClientData::normalizeFilenameWoW(file_key.filepath()).c_str(), 0, file_handle);
}

bool BlizzardArchive::Archive::MPQArchive::writeFile(Listfile::FileKey const& file_key, const char* file_data,
    std::size_t buf_size, Locale locale, unsigned long skip_error, bool compress)
{
    // archive must have been open in non STREAM_FLAG_READ_ONLY mode or  SFileCreateFile will return ERROR_ACCESS_DENIED

    assert(file_key.hasFilepath());
    assert(buf_size);

    HANDLE hFile = NULL;

    // DWORD dwFileSize = (DWORD)strlen(file_data);
    DWORD dwFlags = 0x0;
    DWORD dwCompression = 0x0;

    // compression benchmarks for 1000 files :
    // BZIP 2 : 83sec, ZLIB 18.2sec, no compression = 6.7s
    if (compress)
    {
        dwFlags |= MPQ_FILE_COMPRESS; //Blizzard seems to use MPQ_FILE_SECTOR_CRC. // MPQ_FILE_ENCRYPTED
        dwCompression |= MPQ_COMPRESSION_ZLIB; // MPQ_COMPRESSION_BZIP2 is super slow
    }

    // static_cast<int>(locale) - 1 // use neutral locale

    // archived name must be backslash-separated so its hash matches the client's lookup
    // (see addFile() -- StormLib does not normalise '/'->'\\' on add)
    std::string archived_name = file_key.filepath();
    std::replace(archived_name.begin(), archived_name.end(), '/', '\\');

    if (SFileCreateFile(_handle, archived_name.c_str(), 0, buf_size, 0, dwFlags, &hFile))
    {
        // Write the file
        if (hFile != 0 && !SFileWriteFile(hFile, file_data, buf_size, dwCompression))
        {
            // exception "Failed to write data to the MPQ"
            auto nError = GetLastError();
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileWriteFile: Error creating file: " +
                archived_name + "in archive" + _path);
        }

        // do this now or after checks?
        _listfile->addFile(archived_name);

        if (!SFileFinishFile(hFile))
        {
            auto nError = GetLastError();
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileFinishFile: Error creating file: " +
            file_key.filepath() + "in archive" + _path);
        }
    }
    else
    {
        DWORD const error = GetLastError();

        if (skip_error == error)
        {
            // don't allow chain crashing on the same error
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileCreateFile: Error creating file: " +
                file_key.filepath() + "in archive" + _path + ", got the same error twice.", error);
        }

        // handle the various errors
        if (error == ERROR_DISK_FULL)
        {
            // increase archive size.

            DWORD dwMaxFileCount = SFileGetMaxFileCount(_handle);
            if (!SFileSetMaxFileCount(_handle, dwMaxFileCount + 1))
                return true;
            // add file again
            return writeFile(file_key, file_data, buf_size, locale, error, compress);
        }
        else if (error == ERROR_ALREADY_EXISTS)
        {
            // delete file
            if (!SFileRemoveFile(_handle, file_key.filepath().c_str(), 0))
                throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileRemoveFile: Error replacing file: " +
                    file_key.filepath() + "in archive" + _path, error);

            return writeFile(file_key, file_data, buf_size, locale, error, compress);
        }
        else if (error == ERROR_ACCESS_DENIED)
        {
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileCreateFile: Error creating file: " +
                file_key.filepath() + "in archive" + _path + "\nMPQ Archive ACCESS DENIED, make sure no other application is using this MPQ.", error);
        }
        else
        {
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::writeFile() SFileCreateFile: Error creating file: " +
                file_key.filepath() + "in archive" + _path + ", unhandled error", error);
        }
    }
    return true;
}

bool BlizzardArchive::Archive::MPQArchive::addFile(Listfile::FileKey const& wow_path, std::string file_disk_path, Locale locale, unsigned long skip_error, bool compress)
{
    // archive must have been open in non STREAM_FLAG_READ_ONLY mode or  SFileCreateFile will return ERROR_ACCESS_DENIED

        // SFileSetAddFileCallback(_handle, AddFileCallback, pLogger);
    // SFileAddFileEx() https://github.com/ladislav-zezula/StormLib/blob/master/src/SFileAddFile.cpp#L896
    // if (!SFileAddFileEx(_handle, disk_path, file_key.filepath().c_str(), dwFlags, dwCompression, MPQ_COMPRESSION_NEXT_SAME))


    assert(wow_path.hasFilepath());

    // CRITICAL: the archived name must use BACKSLASHES. MPQ file lookup hashes the name, and the
    // game (and every correct reader) normalises '/'->'\\' before hashing -- but the bundled
    // StormLib does NOT normalise on add, so a name coming in as a forward-slash generic_string()
    // (which is what the exporter passes) gets hashed as "WORLD/MAPS/..." while the client looks
    // it up as "WORLD\\MAPS\\...". The hashes differ, so the client finds NONE of the files and
    // the whole exported patch is invisible in game. Convert to backslashes here so the stored
    // hash matches the client's lookup. (Verified against a real failing 900MB patch: 0/2871 files
    // resolved with forward-slash hashing, 2871/2871 with backslash.)
    std::string archived_name = wow_path.filepath();
    std::replace(archived_name.begin(), archived_name.end(), '/', '\\');

    HANDLE hFile = NULL;
    DWORD dwFlags = 0x0;
    DWORD dwCompression = 0x0;

    // compression benchmarks for 1000 files :
    // BZIP 2 : 83sec, ZLIB 18.2sec, no compression = 6.7s
    if (compress)
    {
        dwFlags |= MPQ_FILE_COMPRESS | MPQ_FILE_SECTOR_CRC; //Blizzard seems to use MPQ_FILE_SECTOR_CRC. // MPQ_FILE_ENCRYPTED support ?
        dwCompression |= MPQ_COMPRESSION_ZLIB; // MPQ_COMPRESSION_BZIP2 is super slow
    }


    auto has_file = SFileHasFile(_handle, archived_name.c_str());
    if (has_file)
        // delete file
        if (!SFileRemoveFile(_handle, archived_name.c_str(), 0))
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::addFile() SFileRemoveFile: Error replacing file: " +
                archived_name + "in archive" + _path);

    if (SFileAddFileEx(_handle, file_disk_path.c_str(), archived_name.c_str(), dwFlags, dwCompression, MPQ_COMPRESSION_NEXT_SAME))
    {
        _listfile->addFile(archived_name);

        // auto dwVerifyResult = SFileVerifyFile(_handle, wow_path.filepath().c_str(), MPQ_ATTRIBUTE_CRC32 | MPQ_ATTRIBUTE_MD5);
        return true;
    }
    else
    {
        DWORD const error = GetLastError();

        if (skip_error == error)
        {
            // don't allow chain crashing on the same error
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::addFile() SFileAddFileEx: Error adding file: " +
                wow_path.filepath() + "to archive" + _path + ", got the same error twice.", error);
        }

        // handle the various errors
        if (error == ERROR_DISK_FULL)
        {
            // increase archive size.
            DWORD dwMaxFileCount = SFileGetMaxFileCount(_handle);
            if (!SFileSetMaxFileCount(_handle, dwMaxFileCount + 1))
                return false; // failed to increase archive size, just give up...
            // add file again
            return addFile(wow_path, file_disk_path, locale, error, compress);
        }
        else if (error == ERROR_ALREADY_EXISTS)
        {
            // delete file
            if (!SFileRemoveFile(_handle, wow_path.filepath().c_str(), 0))
                throw Exceptions::Archive::FileWriteFailedError("MPQArchive::addFile() SFileRemoveFile: Error replacing file: " +
                    wow_path.filepath() + "in archive" + _path, error);

            return addFile(wow_path, file_disk_path, locale, error, compress);
        }
        else if (error == ERROR_ACCESS_DENIED)
        {
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::addFile() SFileAddFileEx: Error creating file: " +
                wow_path.filepath() + "in archive" + _path + "\nMPQ Archive ACCESS DENIED, make sure no other application is using this MPQ.", error);
        }
        else
        {
            throw Exceptions::Archive::FileWriteFailedError("MPQArchive::addFile() SFileAddFileEx: Error creating file: " +
                wow_path.filepath() + "in archive" + _path + ", unhandled error", error);
        }
    }

    return true;
}

bool BlizzardArchive::Archive::MPQArchive::flushArchive() const
{
    return SFileFlushArchive(_handle);
}

bool BlizzardArchive::Archive::MPQArchive::compactArchive() const
{
    // archive must be opened to compress
    if (!_handle)
        return false;

    // optional :  callback
    // SFileSetCompactCallback(_handle, CompactCallback, &Logger);


    if (!SFileCompactArchive(_handle, NULL, false))
    {
        auto error = GetLastError();
        throw Exceptions::Archive::ArchiveOpenError("MPQArchive::compactArchive(): Error compacting archive: " + _path);
    }

    return true;
}

bool MPQArchive::readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const
{
  assert(file_handle);
  return SFileReadFile(file_handle, buffer, static_cast<DWORD>(buf_size), nullptr, nullptr);
}

bool MPQArchive::closeFile(HANDLE file_handle) const
{
  assert(file_handle);
  return SFileCloseFile(file_handle);
}

bool BlizzardArchive::Archive::MPQArchive::openForWritting()
{
    // close the archive and reopen it without read-only flag

    if (_handle)
    {
        if (!SFileCloseArchive(_handle))
        {
            auto error = GetLastError();
            // ERROR_SUCCESS
            throw Exceptions::Archive::ArchiveCloseError("PQArchive::openForWritting(): Error closing archive: " + _path);
        }
    }

    _handle = nullptr; // todo verify if nullptr

    if (!SFileOpenArchive(_path.c_str(), 0, 0, &_handle))
    {
        auto error = GetLastError();
        throw Exceptions::Archive::ArchiveOpenError("MPQArchive::openForWritting(): Error opening archive: " + _path);
    }

    return true;
}

bool BlizzardArchive::Archive::MPQArchive::closeToReadOnly()
{
    // close the archive and reopen it without read-only flag

    if (_handle)
    {
        if (!SFileCloseArchive(_handle))
        {
            auto error = GetLastError();
            // ERROR_SUCCESS
            throw Exceptions::Archive::ArchiveCloseError("MPQArchive::closeToReadOnly(): Error closing archive: " + _path);
        }
    }

    // _handle = nullptr;

    if (!SFileOpenArchive(_path.c_str(), 0, MPQ_OPEN_NO_LISTFILE | STREAM_FLAG_READ_ONLY, &_handle))
    {
        auto error = GetLastError();
        throw Exceptions::Archive::ArchiveOpenError("Error opening archive: " + _path);
    }
    // don't need to read listfile again, should be added when adding files in writeFile()

    return true;
}

std::uint64_t MPQArchive::getFileSize(HANDLE file_handle) const
{
  assert(file_handle);
  return SFileGetFileSize(file_handle, nullptr);
}

bool MPQArchive::exists(Listfile::FileKey const& file_key, Locale locale) const
{
  assert(file_key.hasFilepath());
  return SFileHasFile(_handle, ClientData::normalizeFilenameWoW(file_key.filepath()).c_str());
}

MPQArchive::~MPQArchive()
{
  if (_handle)
    SFileCloseArchive(_handle);
}