#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace recomp::mods {
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t compressed_size = 0;
    uint32_t size = 0;
    uint32_t local_offset = 0;
    bool directory = false;
};
bool parse_zip(const std::vector<uint8_t>& bytes, std::vector<ZipEntry>& entries,
               std::string* error);
// Pass the same archive bytes and unchanged entries produced by parse_zip.
// The caller owns an isolated staging target and any publication/cleanup.
bool extract_zip(const std::vector<uint8_t>& bytes, const std::vector<ZipEntry>& entries,
                 const std::filesystem::path& target, std::string* error);
}
