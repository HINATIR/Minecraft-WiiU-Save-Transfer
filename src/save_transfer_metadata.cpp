#include "save_transfer_metadata.h"

#include <algorithm>
#include <cerrno>
#include <codecvt>
#include <cstdlib>
#include <limits>
#include <locale>
#include <cstring>

namespace save_transfer
{
namespace
{

uint32_t ReadBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void WriteBe16(uint8_t* output, uint16_t value)
{
    output[0] = static_cast<uint8_t>(value >> 8);
    output[1] = static_cast<uint8_t>(value);
}

void WriteBe32(uint8_t* output, uint32_t value)
{
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

bool ParseUnsigned(const std::string& text, int base, uint64_t& value)
{
    if (text.empty())
        return false;

    errno = 0;
    char* end = nullptr;
    if (text[0] == '-')
    {
        // 4J_SEED is stored as a signed decimal value in some .ext files,
        // but the protocol carries its two's-complement 64-bit bit pattern.
        const long long parsed = std::strtoll(text.c_str(), &end, base);
        if (errno == ERANGE || end == text.c_str() || *end != '\0')
            return false;
        value = static_cast<uint64_t>(parsed);
        return true;
    }

    const unsigned long long parsed = std::strtoull(text.c_str(), &end, base);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
        return false;
    value = static_cast<uint64_t>(parsed);
    return true;
}

bool Fail(std::string* error, const char* message)
{
    if (error)
        *error = message;
    return false;
}

bool HasPngSignature(const std::vector<uint8_t>& data)
{
    static constexpr uint8_t kPngSignature[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    return data.size() >= sizeof(kPngSignature) &&
           std::equal(std::begin(kPngSignature), std::end(kPngSignature), data.begin());
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < size; ++index)
    {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(crc & 1u)));
    }
    return ~crc;
}

void AppendBe32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>(value >> 24));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value));
}

void AppendPngChunk(std::vector<uint8_t>& png, const char (&type)[5],
                    const std::vector<uint8_t>& data)
{
    AppendBe32(png, static_cast<uint32_t>(data.size()));
    const size_t chunk_start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    AppendBe32(png, Crc32(png.data() + chunk_start, 4 + data.size()));
}

void AppendPngText(std::vector<uint8_t>& png, const char* key, const char* value)
{
    std::vector<uint8_t> data;
    data.insert(data.end(), key, key + std::strlen(key));
    data.push_back(0);
    data.insert(data.end(), value, value + std::strlen(value));
    AppendPngChunk(png, "tEXt", data);
}

} // namespace

bool ParseExtensionFile(const std::vector<uint8_t>& extension, Metadata& metadata,
                        std::string* error)
{
    std::vector<uint8_t> png;
    if (!ExtractExtensionIconPng(extension, png, error))
        return false;
    if (!HasPngSignature(png))
        return Fail(error, "extension file is shorter than its 0x100-byte header");

    metadata = {};
    std::copy_n(extension.begin(), kWorldNameBytes, metadata.world_name_utf16be.begin());

    std::map<std::string, std::string> tags;
    size_t offset = 8;
    while (offset + 12 <= png.size())
    {
        const uint32_t length = ReadBe32(png.data() + offset);
        if (length > png.size() - offset - 12)
            return Fail(error, "PNG chunk extends beyond the extension file");

        const std::string type(reinterpret_cast<const char*>(png.data() + offset + 4), 4);
        if (type == "tEXt")
        {
            const uint8_t* data = png.data() + offset + 8;
            size_t cursor = 0;
            while (cursor < length)
            {
                const uint8_t* key_end = std::find(data + cursor, data + length, uint8_t{0});
                if (key_end == data + length)
                    break;
                const std::string key(reinterpret_cast<const char*>(data + cursor), key_end - (data + cursor));
                cursor = static_cast<size_t>(key_end - data) + 1;
                const uint8_t* value_end = std::find(data + cursor, data + length, uint8_t{0});
                const std::string value(reinterpret_cast<const char*>(data + cursor), value_end - (data + cursor));
                tags[key] = value;
                cursor = static_cast<size_t>(value_end - data);
                if (cursor < length)
                    ++cursor;
            }
        }
        offset += static_cast<size_t>(length) + 12;
        if (type == "IEND")
            break;
    }

    uint64_t seed = 0;
    uint64_t host_options = 0;
    uint64_t texture_pack = 0;
    if (!ParseUnsigned(tags["4J_SEED"], 10, seed))
        return Fail(error, "PNG metadata does not contain a valid 4J_SEED");
    if (!ParseUnsigned(tags["4J_HOSTOPTIONS"], 16, host_options) ||
        host_options > std::numeric_limits<uint32_t>::max())
        return Fail(error, "PNG metadata does not contain valid 4J_HOSTOPTIONS");
    if (!ParseUnsigned(tags["4J_TEXTUREPACK"], 16, texture_pack) ||
        texture_pack > std::numeric_limits<uint32_t>::max())
        return Fail(error, "PNG metadata does not contain a valid 4J_TEXTUREPACK");

    metadata.seed = seed;
    metadata.host_options = static_cast<uint32_t>(host_options);
    metadata.texture_pack = static_cast<uint32_t>(texture_pack);
    metadata.png_text_tags = std::move(tags);
    return true;
}

std::string DecodeWorldNameUtf8(const std::array<uint8_t, kWorldNameBytes>& world_name_utf16be)
{
    std::u16string utf16;
    for (size_t offset = 0; offset + 1 < world_name_utf16be.size(); offset += 2)
    {
        const char16_t value = static_cast<char16_t>(
            (static_cast<uint16_t>(world_name_utf16be[offset]) << 8) |
            world_name_utf16be[offset + 1]);
        if (value == 0)
            break;
        utf16.push_back(value);
    }

    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    return converter.to_bytes(utf16);
}

bool EncodeWorldNameUtf8(const std::string& world_name_utf8,
                         std::array<uint8_t, kWorldNameBytes>& output,
                         std::string* error)
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    std::u16string utf16;
    try
    {
        utf16 = converter.from_bytes(world_name_utf8);
    }
    catch (const std::range_error&)
    {
        return Fail(error, "world name is not valid UTF-8");
    }

    if (utf16.size() > kWorldNameBytes / 2)
        return Fail(error, "world name is longer than 64 UTF-16 code units");

    output.fill(0);
    for (size_t index = 0; index < utf16.size(); ++index)
        WriteBe16(output.data() + index * 2, static_cast<uint16_t>(utf16[index]));
    return true;
}

bool SetExtensionWorldName(std::vector<uint8_t>& extension, const std::string& world_name_utf8,
                           Metadata* metadata, std::string* error)
{
    if (extension.size() < kExtensionHeaderSize)
        return Fail(error, "extension file is shorter than its 0x100-byte header");

    std::array<uint8_t, kWorldNameBytes> encoded{};
    if (!EncodeWorldNameUtf8(world_name_utf8, encoded, error))
        return false;

    std::copy(encoded.begin(), encoded.end(), extension.begin());
    if (metadata)
        metadata->world_name_utf16be = encoded;
    return true;
}

bool ExtractExtensionIconPng(const std::vector<uint8_t>& extension,
                             std::vector<uint8_t>& png,
                             std::string* error)
{
    if (extension.size() < kExtensionHeaderSize + 8)
        return Fail(error, "extension file is shorter than its 0x100-byte header");

    png.assign(extension.begin() + kExtensionHeaderSize, extension.end());
    if (!HasPngSignature(png))
        return Fail(error, "extension payload is not a PNG file");
    return true;
}

bool ReplaceExtensionIconPng(std::vector<uint8_t>& extension,
                             const std::vector<uint8_t>& png,
                             std::string* error)
{
    if (extension.size() < kExtensionHeaderSize)
        return Fail(error, "extension file is shorter than its 0x100-byte header");
    if (!HasPngSignature(png))
        return Fail(error, "imported icon is not a PNG file");

    extension.resize(kExtensionHeaderSize);
    extension.insert(extension.end(), png.begin(), png.end());
    return true;
}

void BuildDefaultExtensionFile(std::vector<uint8_t>& extension)
{
    // A valid 1x1 grayscale-alpha PNG whose only pixel is fully transparent.
    static constexpr uint8_t kPngPrefix[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C, 0x02,
    };
    static constexpr uint8_t kTransparentIdat[] = {
        0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41, 0x54,
        0x78, 0xDA, 0x63, 0x64, 0xF8, 0x0F, 0x00, 0x01,
        0x05, 0x01, 0x01, 0x27, 0x18, 0xE3, 0x66,
    };
    static constexpr uint8_t kPngEnd[] = {
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };

    extension.assign(kExtensionHeaderSize, 0);
    std::vector<uint8_t> png(std::begin(kPngPrefix), std::end(kPngPrefix));
    AppendPngText(png, "4J_HOSTOPTIONS", "3e9c");
    AppendPngText(png, "4J_TEXTUREPACK", "0");
    AppendPngText(png, "4J_EXTRADATA", "79900a8");
    AppendPngText(png, "4J_#LOADS", "0");
    png.insert(png.end(), std::begin(kTransparentIdat), std::end(kTransparentIdat));
    png.insert(png.end(), std::begin(kPngEnd), std::end(kPngEnd));
    extension.insert(extension.end(), png.begin(), png.end());
}

void BuildDefaultMetadata(Metadata& metadata)
{
    metadata = {};
    metadata.host_options = kDefaultHostOptions;
    metadata.texture_pack = kDefaultTexturePack;
    metadata.png_text_tags["4J_HOSTOPTIONS"] = "3e9c";
    metadata.png_text_tags["4J_TEXTUREPACK"] = "0";
    metadata.png_text_tags["4J_EXTRADATA"] = "79900a8";
    metadata.png_text_tags["4J_#LOADS"] = "0";
}

bool BuildApplicationData(uint64_t save_size, uint64_t extension_payload_size,
                          const Metadata& metadata,
                          std::array<uint8_t, kApplicationDataSize>& output,
                          std::string* error)
{
    if (save_size > std::numeric_limits<uint32_t>::max() ||
        extension_payload_size > std::numeric_limits<uint32_t>::max())
        return Fail(error, "save component is too large for the Wii U metadata format");

    output.fill(0);
    WriteBe16(output.data(), 11);
    WriteBe16(output.data() + 2, 2);
    WriteBe32(output.data() + 4, static_cast<uint32_t>(save_size));
    WriteBe32(output.data() + 8, static_cast<uint32_t>(extension_payload_size));
    WriteBe32(output.data() + 0x10, static_cast<uint32_t>(metadata.seed >> 32));
    WriteBe32(output.data() + 0x14, static_cast<uint32_t>(metadata.seed));
    WriteBe32(output.data() + 0x18, metadata.host_options);
    WriteBe32(output.data() + 0x1C, metadata.texture_pack);
    std::copy(metadata.world_name_utf16be.begin(), metadata.world_name_utf16be.end(),
              output.begin() + 0x20);
    return true;
}

} // namespace save_transfer
