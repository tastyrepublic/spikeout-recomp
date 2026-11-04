// A small zip reader for ROM sets (spikeofe.zip): the central directory,
// files stored or deflated (RFC 1951; the decoder follows the structure of
// zlib's puff.c, Mark Adler's reference inflate), each checked against its
// CRC-32. No zip64, encryption or other methods: MAME's ROM zips need none.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace m3 {

class ZipReader {
public:
    struct Entry {
        std::string name; // the name inside the zip, without its folders
        uint64_t size = 0;
        uint64_t packed = 0;
        uint32_t crc = 0;
        uint16_t method = 0; // 0 stored, 8 deflated
        uint64_t header = 0; // the local header's offset
    };

    // Opens a zip and reads its directory; throws std::runtime_error if it isn't one it can read.
    explicit ZipReader(const std::filesystem::path& path);

    // The files, by lower-case name (a name in two folders: the first).
    const std::map<std::string, Entry>& entries() const { return entries_; }
    // A file's bytes (throws if it's damaged or packed some other way).
    std::vector<uint8_t> read(const Entry& e);

private:
    std::ifstream in_;
    std::map<std::string, Entry> entries_;
};

// Raw DEFLATE data into exactly `size` bytes; false if the data is bad.
bool inflate(const uint8_t* in, std::size_t in_size, std::vector<uint8_t>& out, std::size_t size);

} // namespace m3
