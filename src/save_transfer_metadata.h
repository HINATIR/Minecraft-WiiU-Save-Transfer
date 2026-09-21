#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace save_transfer
{

constexpr size_t kExtensionHeaderSize = 0x100;
constexpr size_t kApplicationDataSize = 0xA0;
constexpr size_t kWorldNameBytes = 0x80;
constexpr uint32_t kDefaultHostOptions = 0x3e9c;
constexpr uint32_t kDefaultTexturePack = 0;
constexpr uint32_t kDefaultExtraData = 0x79900a8;
constexpr uint32_t kDefaultLoadCount = 0;

struct Metadata
{
    uint64_t seed = 0;
    uint32_t host_options = 0;
    uint32_t texture_pack = 0;
    std::array<uint8_t, kWorldNameBytes> world_name_utf16be{};
    std::map<std::string, std::string> png_text_tags;
};

bool ParseExtensionFile(const std::vector<uint8_t>& extension, Metadata& metadata,
                        std::string* error = nullptr);

std::string DecodeWorldNameUtf8(const std::array<uint8_t, kWorldNameBytes>& world_name_utf16be);
bool EncodeWorldNameUtf8(const std::string& world_name_utf8,
                         std::array<uint8_t, kWorldNameBytes>& output,
                         std::string* error = nullptr);
bool SetExtensionWorldName(std::vector<uint8_t>& extension, const std::string& world_name_utf8,
                           Metadata* metadata = nullptr, std::string* error = nullptr);
bool ExtractExtensionIconPng(const std::vector<uint8_t>& extension,
                             std::vector<uint8_t>& png,
                             std::string* error = nullptr);
bool ReplaceExtensionIconPng(std::vector<uint8_t>& extension,
                             const std::vector<uint8_t>& png,
                             std::string* error = nullptr);

void BuildDefaultExtensionFile(std::vector<uint8_t>& extension);
void BuildDefaultMetadata(Metadata& metadata);

bool BuildApplicationData(uint64_t save_size, uint64_t extension_payload_size,
                          const Metadata& metadata,
                          std::array<uint8_t, kApplicationDataSize>& output,
                          std::string* error = nullptr);

} // namespace save_transfer
