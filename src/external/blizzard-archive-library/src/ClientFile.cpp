#include <ClientFile.hpp>
#include <Exception.hpp>
#include <fstream>
#include <iostream>
#include <system_error>
#include <cstring>

using namespace BlizzardArchive;

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data)
  : _file_key(file_key)
  , _eof(true)
  , _pointer(0)
  , _external(false)
{

  if (client_data->version() != ClientVersion::WOTLK)
  {
    _file_key.deduceOtherComponent(client_data->listfile());
  }
  
  _disk_path = client_data->getDiskPath(_file_key);

  std::ifstream input(_disk_path.string(), std::ios_base::binary | std::ios_base::in);
  if (input.is_open())
  {
    _external = true;
    _eof = false;

    input.seekg(0, std::ios::end);
    _buffer.resize(input.tellg());
    input.seekg(0, std::ios::beg);
    input.read(_buffer.data(), _buffer.size());
    input.close();
    return;
  }

  if (client_data->readFile(file_key, _buffer))
  {
    _eof = false;
    return;
  }
 
  throw Exceptions::FileReadFailedError(
    "File '"
    + (file_key.hasFilepath() ? file_key.filepath() : std::to_string(file_key.fileDataID()))
    + "' does not exist or some other error occured.");
}

ClientFile::ClientFile(Listfile::FileKey const& file_key, ClientData* client_data, NEW_FILE_T)
: _file_key(file_key)
, _eof(true)
, _pointer(0)
, _external(false)
{
  if (client_data->version() != ClientVersion::WOTLK)
  {
    _file_key.deduceOtherComponent(client_data->listfile());
  }

  _disk_path = client_data->getDiskPath(_file_key);
}


std::size_t ClientFile::read(void* dest, size_t bytes)
{
  // Short/eof reads MUST NOT leave the destination untouched: callers read whole structs
  // without checking the return value, and an unfilled struct is stack garbage -- one garbage
  // MHDR turned a vanilla tile into a fake MH2O water parse with wild offsets and heap
  // corruption. Zero-fill guarantees "missing data reads as zeros", which every parser
  // treats as absent/empty.
  if (_eof || !bytes)
  {
    if (bytes)
      std::memset(dest, 0, bytes);
    return 0;
  }

  size_t const requested = bytes;
  size_t rpos = _pointer + bytes;
  if (rpos > _buffer.size()) {
    bytes = _buffer.size() - _pointer;
    _eof = true;
  }

  std::memcpy(dest, &(_buffer[_pointer]), bytes);
  if (bytes < requested)
  {
    std::memset(static_cast<char*>(dest) + bytes, 0, requested - bytes);
  }

  _pointer = rpos;

  return bytes;
}

bool ClientFile::isEof() const
{
  return _eof;
}

void ClientFile::seek(std::size_t offset)
{
  _pointer = offset;
  _eof = (_pointer >= _buffer.size());
}

void ClientFile::seekRelative(std::size_t offset)
{
  _pointer += offset;
  _eof = (_pointer >= _buffer.size());
}

void ClientFile::close()
{
  _eof = true;
}

std::size_t ClientFile::getSize() const
{
  return _buffer.size();
}

std::size_t ClientFile::getPos() const
{
  return _pointer;
}

char const* ClientFile::getBuffer() const
{
  return _buffer.data();
}

char const* ClientFile::getPointer() const
{
  return _buffer.data() + _pointer;
}

void ClientFile::save()
{

  std::cout << "Saving file to: " << _disk_path << std::endl;

  auto const directory_name(_disk_path.parent_path());
  std::error_code ec;
  std::filesystem::create_directories(directory_name, ec);

  if (ec)
  {
    std::cout << "Error: Creating directory \"" << directory_name << "\" failed: " << ec << ". Saving is highly likely to fail." << std::endl;
  }

  std::ofstream output(_disk_path.string(), std::ios_base::binary | std::ios_base::out);
  if (output.is_open())
  {
    output.write(_buffer.data(), _buffer.size());
    output.close();

    _external = true;
  }
  else
  {

    std::cout << "Error saving file to: " << _disk_path << std::endl;
  }
  
}