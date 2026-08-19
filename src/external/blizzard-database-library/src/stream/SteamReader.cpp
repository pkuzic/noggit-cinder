#include <stream/StreamReader.h>

namespace BlizzardDatabaseLib {
    namespace Stream {

        StreamReader::StreamReader(std::shared_ptr<IMemStream> stream) : _underlyingStream(stream)
        {

        }

        StreamReader::~StreamReader()
        {
            _underlyingStream.reset();
        }

        std::string StreamReader::ReadString()
        {
            std::string string;
            std::getline(*_underlyingStream, string, '\0');
            return string;
        }

        std::string StreamReader::ReadString(std::size_t length)
        {
            auto data = ReadBlock(length);
            return std::string(data.get(), length);
        }

        std::unique_ptr<char[]> StreamReader::ReadBlock(std::size_t length)
        {
            std::unique_ptr<char[]> blockArray(new char[length]);
            _underlyingStream->read(blockArray.get(), length);

            return blockArray;
        }

        std::streampos StreamReader::Position()
        {
            return _underlyingStream->tellg();
        }

        bool StreamReader::Good()
        {
            return _underlyingStream->good();
        }

        void StreamReader::JumpEnd()
        {

            _underlyingStream->seekg(0, _underlyingStream->end);
        }

        void StreamReader::JumpStart()
        {
            _underlyingStream->seekg(0, _underlyingStream->beg);
        }

        void StreamReader::Jump(std::streampos position)
        {
            _underlyingStream->seekg(position, _underlyingStream->beg);
        }

        std::size_t StreamReader::Length()
        {
            auto currentPosition = Position();
            JumpEnd();
            auto length = Position();
            Jump(currentPosition);

            return length;
        }
    }
}
