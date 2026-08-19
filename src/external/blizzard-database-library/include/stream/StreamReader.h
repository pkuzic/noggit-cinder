#pragma once
#include <fstream>
#include <memory>
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <streambuf>
#include <iomanip>
#include <cstring>
#include <structures/Types.h>


namespace BlizzardDatabaseLib {
    namespace Stream {


        struct IMemBuf: std::streambuf
        {
          IMemBuf(const char* base, size_t size)
          {
            _buf = new char[size];
            _size = size;
            std::memcpy(_buf, base, size);
            this->setg(_buf, _buf, _buf + size);
          }

          virtual pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which = std::ios_base::in) override
          {
            if(dir == std::ios_base::cur)
              gbump(static_cast<int>(off));
            else if(dir == std::ios_base::end)
              setg(_buf, _buf + _size + off, _buf + _size);
            else if(dir == std::ios_base::beg)
              setg(_buf, _buf+off, _buf + _size);

            return gptr() - eback();
          }

          virtual pos_type seekpos(std::streampos pos, std::ios_base::openmode mode) override
          {
            return seekoff(pos - pos_type(off_type(0)), std::ios_base::beg, mode);
          }

          ~IMemBuf()
          {
            delete[] _buf;
          }

        private:
          char* _buf;
          std::size_t _size;
        };

        struct IMemStream: virtual IMemBuf, std::istream
        {
          IMemStream(const char* mem, size_t size) :
              IMemBuf(mem, size),
              std::istream(static_cast<std::streambuf*>(this))
          {
          }
        };

        class StreamReader
        {
        private:
            std::shared_ptr<IMemStream> _underlyingStream;
        public:
            StreamReader(std::shared_ptr<IMemStream> stream);
            ~StreamReader();
            template <typename T> T Read()
            {
                auto type = T();
                auto size = sizeof(T);
                auto pointer = reinterpret_cast<char*>(&type);
                _underlyingStream->read(pointer, size);
                return type;
            }

            template <typename T> std::vector<T> ReadArray(std::size_t length)
            {
                auto vector = std::vector<T>(length);
                auto size = sizeof(T);
                auto pointer = reinterpret_cast<char*>(vector.data());
                _underlyingStream->read(pointer, size * length);
                return vector;
            }

            std::unique_ptr<char[]> ReadBlock(std::size_t length);
            std::string ReadString();
            std::string ReadString(std::size_t length);
            void Jump(std::streampos position);
            void JumpEnd();
            void JumpStart();
            std::size_t Length();
            std::streampos Position();
            bool Good();
        };
    }
}