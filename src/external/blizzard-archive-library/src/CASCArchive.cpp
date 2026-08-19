#include <CASCArchive.hpp>

#include <Exception.hpp>
#include <CascLib.h>


using namespace BlizzardArchive::Archive;

CASCArchive::CASCArchive(std::string const& path
                         , std::string const& cache_path
                         , Locale locale
                         , OpenMode open_mode
                         , Listfile::Listfile* listfile)
  : BaseArchive(path, locale, listfile)
{
  switch (open_mode)
  {
    case OpenMode::REMOTE:
    {
      CASC_OPEN_STORAGE_ARGS args;
      args.Size = sizeof(CASC_OPEN_STORAGE_ARGS);
      args.szLocalPath = cache_path.c_str();
      args.szCodeName = "wow";
      args.szRegion = "us";
      args.PfnProgressCallback = nullptr;
      args.PtrProgressParam = nullptr;
      args.PfnProductCallback = nullptr;
      args.PtrProductParam = nullptr;
      args.dwLocaleMask = 0;  // TODO: pass locale
      args.szBuildKey = nullptr;
      args.szCdnHostUrl = path.c_str();


      if (!CascOpenStorageEx(nullptr, &args, true, &_handle))
      {
        throw Exceptions::Archive::ArchiveOpenError("Error opening CASC archive: " + path
                                                    + ". Error code: " + std::to_string(GetCascError()));
      }

      break;
    }
    case OpenMode::LOCAL:
    {
      CASC_OPEN_STORAGE_ARGS args;
      args.Size = sizeof(CASC_OPEN_STORAGE_ARGS);
      args.szLocalPath = path.c_str();
      args.szCodeName = "wow";
      args.szRegion = "us";
      args.PfnProgressCallback = nullptr;
      args.PtrProgressParam = nullptr;
      args.PfnProductCallback = nullptr;
      args.PtrProductParam = nullptr;
      args.dwLocaleMask = 0;  // TODO: pass locale
      args.szBuildKey = nullptr;
      args.szCdnHostUrl = nullptr;


      if (!CascOpenStorageEx(nullptr, &args, false, &_handle))
      {
        throw Exceptions::Archive::ArchiveOpenError("Error opening CASC archive: " + path
                                                    + ". Error code: " + std::to_string(GetCascError()));
      }

      break;
    }
  }

}

bool CASCArchive::openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const
{
  std::uint32_t file_data_id;

  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    file_data_id = file_key.fileDataID();
  }
  else
  {
    file_data_id = _listfile->getFileDataID(file_key.filepath());
  }


  return CascOpenFile(_handle, CASC_FILE_DATA_ID(file_data_id), 0, 3, file_handle);
}

bool CASCArchive::readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const
{
  assert(file_handle);
  return CascReadFile(file_handle, buffer, static_cast<DWORD>(buf_size), nullptr);
}

bool CASCArchive::closeFile(HANDLE file_handle) const
{
  assert(file_handle);
  return CascCloseFile(file_handle);
}

std::uint64_t CASCArchive::getFileSize(HANDLE file_handle) const
{
  assert(file_handle);
  unsigned long long size;

  CascGetFileSize64(file_handle, static_cast<PULONGLONG>(&size));

  return size;
}

bool CASCArchive::exists(Listfile::FileKey const& file_key, Locale locale) const
{
  HANDLE file_handle = nullptr;
  std::uint32_t file_data_id;

  assert(file_key.hasFileDataID() || file_key.hasFilepath());

  if (file_key.hasFileDataID())
  {
    assert(file_key.fileDataID());

    file_data_id = file_key.fileDataID();
  }
  else
  {
    file_data_id = _listfile->getFileDataID(file_key.filepath());
  }


  bool status = CascOpenFile(_handle, CASC_FILE_DATA_ID(file_data_id), 0, 3, &file_handle);
  CascCloseFile(file_handle);
  return status;

}

CASCArchive::~CASCArchive()
{
  if (_handle)
    CascCloseStorage(_handle);
}