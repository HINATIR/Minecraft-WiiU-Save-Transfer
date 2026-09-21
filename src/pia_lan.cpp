#include "pia_lan.h"
#include "crypto.h"

#include <algorithm>
#include <cstring>

namespace pia
{
namespace
{
constexpr size_t kCriteriaSize = 0x23A;
constexpr size_t kChallengeSize = 0x12A;
constexpr size_t kChallengeHeaderSize = 0x2A;
constexpr size_t kResponseSize = 0x3A;

void SetError(std::string* error, const char* message)
{
    if (error)
        *error = message;
}

uint32_t ReadBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

void WriteBe16(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteBe32(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

void WriteBe64(uint8_t* data, uint64_t value)
{
    for (int index = 7; index >= 0; --index)
    {
        data[index] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

bool RandomBytes(uint8_t* output, size_t size)
{
    return crypto::RandomBytes(output, size);
}

bool HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* input,
                size_t input_size, std::array<uint8_t, 32>& output)
{
    return crypto::HmacSha256(key, key_size, input, input_size, output);
}

bool AesEcbEncryptBlock(const uint8_t* key, const uint8_t* input,
                        std::array<uint8_t, 16>& output)
{
    return crypto::AesEcbEncryptBlock(key, input, output);
}

bool AesGcm(bool encrypt, const uint8_t* key, const uint8_t* nonce,
            const uint8_t* input, size_t input_size, uint8_t* tag,
            std::vector<uint8_t>& output)
{
    output.resize(input_size);
    if (!crypto::AesGcm(encrypt, key, nonce, input, input_size, nullptr, 0, tag, output.data()))
    {
        output.clear();
        return false;
    }
    return true;
}

void WriteStationLocation(uint8_t* output, const LanHostConfig59& config,
                          uint64_t constant_id, uint32_t service_variable_id)
{
    std::copy(config.address.begin(), config.address.end(), output);
    WriteBe16(output + 4, config.port);
    WriteBe64(output + 6, constant_id);
    WriteBe32(output + 14, config.variable_id);
    WriteBe32(output + 18, service_variable_id);
    output[22] = 0;
    output[23] = 0;
    output[24] = 0;
    output[25] = 1;
    output[26] = 2;
    output[27] = 3;
    output[28] = 0;
}
} // namespace

bool BuildLanBrowseReply59(const uint8_t* request, size_t request_size,
                           const LanHostConfig59& config,
                           const std::vector<uint8_t>& game_key,
                           LanBrowseResult59& output, std::string* error)
{
    output = {};
    if (!request || request_size != kLanBrowseRequestSize59 || request[0] != 0 ||
        ReadBe32(request + 1) != kCriteriaSize)
    {
        SetError(error, "not a PIA 5.9 LAN browse request");
        return false;
    }
    if (game_key.size() != 16 || config.port == 0)
    {
        SetError(error, "invalid LAN host configuration or game key");
        return false;
    }

    const uint8_t* challenge = request + 5 + kCriteriaSize;
    if (challenge[0] != 1 || challenge[1] != 1)
    {
        SetError(error, "unsupported LAN crypto challenge");
        return false;
    }

    std::array<uint8_t, 16> challenge_aes_key{};
    if (!AesEcbEncryptBlock(game_key.data(), challenge + 0x0A, challenge_aes_key))
    {
        SetError(error, "could not derive challenge AES key");
        return false;
    }
    std::array<uint8_t, 12> request_nonce = {config.address[0], config.address[1],
                                             config.address[2], 0xFF};
    std::copy_n(challenge + 2, 8, request_nonce.begin() + 4);
    std::array<uint8_t, 16> request_tag{};
    std::copy_n(challenge + 0x1A, 16, request_tag.begin());
    std::vector<uint8_t> plaintext;
    if (!AesGcm(false, challenge_aes_key.data(), request_nonce.data(), challenge + kChallengeHeaderSize,
                256, request_tag.data(), plaintext))
    {
        SetError(error, "LAN crypto challenge authentication failed");
        return false;
    }
    std::array<uint8_t, 32> challenge_digest{};
    if (!HmacSha256(game_key.data(), game_key.size(), plaintext.data(), plaintext.size(),
                    challenge_digest))
    {
        SetError(error, "could not calculate LAN challenge response");
        return false;
    }

    if (!RandomBytes(output.session_key_parameter.data(), 16))
    {
        SetError(error, "could not generate LAN session parameter");
        return false;
    }
    std::copy_n(challenge + 0x0A, 16, output.session_key_parameter.begin() + 16);

    const uint32_t address = ReadBe32(config.address.data());
    output.network_id = ((address & 0xFFFFu) << 16) | config.port;
    output.constant_id = (static_cast<uint64_t>(address) << 32) | config.port;
    output.service_variable_id = ((address & 0xFFFFu) << 16) | config.port;

    output.reply.assign(kLanBrowseReplySize59, 0);
    output.reply[0] = 1;
    WriteBe32(output.reply.data() + 1, static_cast<uint32_t>(kLanSessionInfoSize59));
    uint8_t* session = output.reply.data() + 5;
    WriteBe32(session, config.game_mode);
    WriteBe32(session + 4, output.network_id);
    WriteBe16(session + 0x20, 1);
    WriteBe16(session + 0x22, 0);
    WriteBe16(session + 0x24, config.maximum_participants);
    session[0x26] = 5;
    session[0x27] = config.application_version;
    WriteBe16(session + 0x28, 0);
    std::copy(config.application_data.begin(), config.application_data.end(), session + 0x2A);
    WriteBe32(session + 0x1AA, static_cast<uint32_t>(config.application_data.size()));
    session[0x1AE] = 1;
    WriteStationLocation(session + 0x1AF, config, output.constant_id,
                         output.service_variable_id);
    uint8_t* station = session + 0x1D2;
    station[0] = 1;
    station[1] = 1;
    const size_t username_size = std::min<size_t>(40, config.username.size());
    std::copy_n(config.username.begin(), username_size, station + 2);
    WriteBe64(station + 0x2A, output.constant_id);
    std::copy(output.session_key_parameter.begin(), output.session_key_parameter.end(),
              session + 0x4F2);

    uint8_t* response = session + kLanSessionInfoSize59;
    response[0] = 1;
    response[1] = 1;
    response[9] = 1;
    std::copy_n(output.session_key_parameter.begin(), 16, response + 0x0A);
    std::array<uint8_t, 32> response_key_digest{};
    if (!HmacSha256(game_key.data(), game_key.size(), output.session_key_parameter.data(),
                    output.session_key_parameter.size(), response_key_digest))
    {
        SetError(error, "could not derive LAN response key");
        output = {};
        return false;
    }
    std::array<uint8_t, 12> response_nonce = {config.address[0], config.address[1],
                                              config.address[2], 0xFF};
    response_nonce[11] = 1;
    std::vector<uint8_t> encrypted_response;
    if (!AesGcm(true, response_key_digest.data(), response_nonce.data(), challenge_digest.data(),
                16, response + 0x1A, encrypted_response))
    {
        SetError(error, "could not encrypt LAN challenge response");
        output = {};
        return false;
    }
    std::copy(encrypted_response.begin(), encrypted_response.end(), response + kChallengeHeaderSize);
    return true;
}
} // namespace pia
