// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBCFile.h>
#include <noggit/Log.h>
#include <noggit/project/CurrentProject.hpp>

#include <ClientFile.hpp>

#include <QDir>

#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>


template<typename T> inline
auto write(std::ostream& stream, T const& val) -> void
{
  stream.write(reinterpret_cast<char const*>(&val), sizeof(T));
}

// Defaults to the WotLK/TBC 16-locale layout; OpenDBs() lowers it to 8 for vanilla.
int DBCFile::LocaleCount = 16;

int DBCFile::localeSlot(int client_locale_id)
{
  if (client_locale_id >= 0 && client_locale_id < LocaleCount)
    return client_locale_id;
  if (LocaleCount == 8 && client_locale_id == 8)
    return 5;
  return 0;
}

DBCFile::DBCFile(const std::string& _filename)
  : filename(_filename)
{
}

void DBCFile::open(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  BlizzardArchive::ClientFile f(filename, clientData.get());

  if (f.isEof())
  {
    LogError << "The DBC file \"" << filename << "\" could not be opened. This application may crash soon as the file is most likely needed." << std::endl;
    return;
  }
  LogDebug << "Opening DBC \"" << filename << "\"" << std::endl;

  char header[4];

  f.read(header, 4); // Number of records
  assert(header[0] == 'W' && header[1] == 'D' && header[2] == 'B' && header[3] == 'C');
  f.read(&recordCount, 4);
  f.read(&fieldCount, 4);
  f.read(&recordSize, 4);
  f.read(&stringSize, 4);

  if (!fieldCount || !recordSize)
  {
    throw std::logic_error("DBC error, field count or record size is 0 : " + filename);
  }

  if (fieldCount * 4 != recordSize)
  {
    throw std::logic_error("non four-byte-columns not supported : " + filename);
  }

  data.resize(recordSize * recordCount);
  f.read(data.data(), data.size());

  stringTable.resize(stringSize);
  f.read(stringTable.data(), stringTable.size());

  f.close();
}

void DBCFile::loadFromMemory(std::uint32_t field_count, std::vector<unsigned char> record_data,
                             std::vector<char> string_table)
{
  assert(field_count && record_data.size() % (field_count * 4) == 0);

  fieldCount = field_count;
  recordSize = field_count * 4;
  recordCount = static_cast<std::uint32_t>(record_data.size() / recordSize);
  data = std::move(record_data);
  stringTable = std::move(string_table);
  stringSize = static_cast<std::uint32_t>(stringTable.size());
}

void DBCFile::save()
{
  QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(str.endsWith('\\') || str.endsWith('/')))
  {
    str += "/";
  }

  std::string filename_proj = BlizzardArchive::ClientData::normalizeFilenameUnix(str.toStdString() + filename);
  QDir dir(str + "/DBFilesClient/");
  if (!dir.exists())
    dir.mkpath(".");

  std::ofstream stream(filename_proj, std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);

  stream << 'W' << 'D' << 'B' << 'C';


  write(stream, recordCount);
  write(stream, fieldCount);
  write(stream, recordSize);
  write(stream, stringSize);

  stream.write(reinterpret_cast<char*>(data.data()), data.size());
  stream.write(stringTable.data(), stringSize);
  stream.close();
}

void DBCFile::overwriteWith(DBCFile const& file)
{
  filename = file.filename;
  recordSize = file.recordSize;
  recordCount = file.recordCount;
  fieldCount = file.fieldCount;
  stringSize = file.stringSize;
  data = file.data;
  stringTable = file.stringTable;
}

DBCFile DBCFile::createNew(std::string filename, std::uint32_t fieldCount, std::uint32_t recordSize)
{
  DBCFile file{};
  file.filename = std::move(filename);
  file.recordSize = recordSize;
  file.fieldCount = fieldCount;
  return file;
}

DBCFile::Record DBCFile::getRecord(size_t id)
{
  if (id >= recordCount)
    throw NotFound(); // row index past the table (also: table not loaded)
  return Record(*this, data.data() + id * recordSize);
}

DBCFile::Iterator DBCFile::begin()
{
  return Iterator(*this, data.data());
}

DBCFile::Iterator DBCFile::end()
{
  return Iterator(*this, data.data() + data.size());
}

size_t DBCFile::getRecordCount() const
{
  return recordCount;
}

size_t DBCFile::getFieldCount() const
{
  return fieldCount;
}

size_t DBCFile::getRecordSize() const
{
  return recordSize;
}

DBCFile::Record DBCFile::getByID(unsigned int id, size_t field)
{
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return (*i);
  }
  LogDebug << "Tried to get a not existing row in " << filename << " (ID = " << id << ")!" << std::endl;
  throw NotFound();
}

bool DBCFile::CheckIfIdExists(unsigned int id, size_t field)
{
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return (true);
  }
  return (false);
}

int DBCFile::getRecordRowId(unsigned int id, size_t field)
{
  int row_id = 0;
  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(field) == id)
      return row_id;

    row_id++;
  }
  LogError << "Tried to get a not existing row in " << filename << " (ID = " << id << ")!" << std::endl;
  throw NotFound();
}

DBCFile::Record DBCFile::addRecord(size_t id, size_t id_field)
{
  assert(recordSize > 0);
  assert(id_field < fieldCount);
  if (recordSize == 0 || id_field >= fieldCount)
    throw NotFound(); // table not loaded / wrong id column: nothing to append to

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();
  }

  size_t old_size = data.size();
  data.resize(old_size + recordSize);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  recordCount++;

  return Record(*this, data.data() + old_size);
}

DBCFile::Record DBCFile::addRecordCopy(size_t id, size_t id_from, size_t id_field)
{
  bool from_found = false;
  size_t from_idx = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
      throw AlreadyExists();

    if (i->getUInt(id_field) == id_from)
    {
      from_found = true;
    }

    if (!from_found)
    {
      from_idx++;
    }
  }

  if (!from_found)
  {
    throw NotFound();
  }

  recordCount++; // only once the copy is certain (an early throw used to leave the count off by one)

  size_t old_size = data.size();
  data.resize(old_size + recordSize);

  Record record_from = getRecord(from_idx);
  std::copy(data.data() + from_idx * recordSize, data.data() + from_idx * recordSize + recordSize, data.data() + old_size);
  *reinterpret_cast<unsigned int*>(data.data() + old_size + id_field * sizeof(std::uint32_t)) = static_cast<unsigned int>(id);

  return Record(*this, data.data() + old_size);
}

void DBCFile::removeRecord(size_t id, size_t id_field)
{
  if (recordCount == 0)
  {
    throw NotFound();
  }

  size_t row_counter = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    if (i->getUInt(id_field) == id)
    {
      size_t initial_size = data.size();

      size_t row_position = row_counter * recordSize; // position of the record to remove

      size_t datasizeafterRow = recordSize * (recordCount - row_counter); // size of the data after the row that needs to be moved at the old row's position

      // assert(initial_size >= (datasizeafterRow + row_position));
      if ((row_position + datasizeafterRow) > initial_size)
      {
        throw std::out_of_range("Attempting to remove more data than available");
      }

      // size_t numRecordsToMove = recordCount - row_counter; // Number of records to move down

      unsigned char* record = data.data() + row_position; // data to remove at position

      // Move all data after the row to the row's position
      // only do it if it wasn't the last row
      if (row_position + recordSize < initial_size)
      {
        assert(row_counter < recordCount);
        std::memmove(record, record + recordSize, datasizeafterRow);
      }
      data.resize(initial_size - recordSize);

      recordCount--;
      return;
    }

    row_counter++;

  }

  throw NotFound();

}

int DBCFile::getEmptyRecordID(size_t id_field)
{

  unsigned int id = 0;

  for (Iterator i = begin(); i != end(); ++i)
  {
    id = std::max(i->getUInt(id_field), id);
  }

  return static_cast<int>(++id);
}

namespace
{
  const float zero_float = 0.f;
  const unsigned int zero_uint = 0u;
  const int zero_int = 0;
}

bool DBCFile::Record::checkField(size_t field, char const* op) const
{
  if (field < file.fieldCount && offset)
    return true;

  static std::mutex reported_mutex; // async loaders read DBCs too
  static std::set<std::string> reported;
  std::string const key = file.filename + ":" + std::to_string(field) + ":" + op;
  std::lock_guard<std::mutex> lock(reported_mutex);
  if (reported.insert(key).second)
  {
    LogError << "DBC \"" << file.filename << "\": " << op << " of field " << field
             << " refused (record has " << file.fieldCount << " fields). A hard-coded column does not"
             << " match this client's DBC layout." << std::endl;
  }
  return false;
}

const float& DBCFile::Record::getFloat(size_t field) const
{
    if (!checkField(field, "read"))
      return zero_float;
    return *reinterpret_cast<float*>(offset + field * 4);
}

const unsigned int& DBCFile::Record::getUInt(size_t field) const
{
  if (!checkField(field, "read"))
    return zero_uint;
  return *reinterpret_cast<unsigned int*>(offset + field * 4);
}

const int& DBCFile::Record::getInt(size_t field) const
{
  if (!checkField(field, "read"))
    return zero_int;
  return *reinterpret_cast<int*>(offset + field * 4);
}

const char* DBCFile::Record::getString(size_t field) const
{
  if (!checkField(field, "read"))
    return "";
  size_t stringOffset = getUInt(field);
  if (stringOffset >= file.stringTable.size())
    return "";
  return file.stringTable.data() + stringOffset;
}

const char* DBCFile::Record::getLocalizedString(size_t field, int locale) const
{
  int loc = locale;
  if (locale == -1)
  {
    // scan the locale columns for the first populated one (typically enUS at loc 0).
    // vanilla has 8 locale columns, TBC+ has 16 — driven by DBCFile::LocaleCount.
    for (loc = 0; loc < DBCFile::LocaleCount; loc++)
    {
      if (field + loc >= file.fieldCount)
        break;
      size_t stringOffset = getUInt(field + loc);
      if (stringOffset != 0)
        break;
    }
    if (loc >= DBCFile::LocaleCount)
      loc = 0; // nothing populated: the first (empty) slot
  }

  if (loc < 0 || !checkField(field + loc, "read"))
    return "";
  size_t stringOffset = getUInt(field + loc);
  if (stringOffset >= file.stringTable.size())
    return "";
  return file.stringTable.data() + stringOffset;
}

void DBCFile::Record::writeString(size_t field, const std::string& val)
{
  if (!checkField(field, "write"))
    return;

  if (!val.size())
  {
    *reinterpret_cast<unsigned int*>(offset + field * 4) = 0;
    return;
  }

  size_t old_size = file.stringTable.size();
  *reinterpret_cast<unsigned int*>(offset + field * 4) = static_cast<unsigned int>(file.stringTable.size());
  file.stringTable.resize(old_size + val.size() + 1);
  std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
  file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
}

void DBCFile::Record::writeLocalizedString(size_t field, const std::string& val, unsigned int locale)
{
  if (static_cast<int>(locale) >= DBCFile::LocaleCount)
  {
    // e.g. a 16-slot widget on a vanilla (8-locale) file: those slots do not exist in the DBC
    return;
  }
  if (!checkField(field + locale, "write"))
    return;

  if (!val.size())
  {
    *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = 0;
    return;
  }

  size_t old_size = file.stringTable.size();
  *reinterpret_cast<unsigned int*>(offset + ((field + locale) * 4)) = static_cast<unsigned int>(file.stringTable.size());
  file.stringTable.resize(old_size + val.size() + 1);
  std::copy(val.c_str(), val.c_str() + val.size() + 1, file.stringTable.data() + old_size);
  file.stringSize += static_cast<std::uint32_t>(val.size() + 1);
}
