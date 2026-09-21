#include "pia_packet.h"
#include "crypto.h"

#include <algorithm>
#include <cstring>

namespace pia
{
namespace
{
constexpr uint8_t kMagic[] = {0x32, 0xAB, 0x98, 0x64};

void SetError(std::string* error, const char* text)
{
    if (error)
        *error = text;
}

uint16_t ReadBe16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint64_t ReadBe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | data[i];
    return value;
}

void WriteBe16(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteBe64(uint8_t* data, uint64_t value)
{
    for (int i = 7; i >= 0; --i)
    {
        data[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

size_t Align(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool IsPadding(const uint8_t* data, size_t size)
{
    return std::all_of(data, data + size, [](uint8_t value) { return value == 0xFF; });
}
} // namespace

bool DeriveLanSessionKey(const std::vector<uint8_t>& game_key,
                         const std::array<uint8_t, 32>& session_key_param,
                         std::array<uint8_t, 16>& session_key,
                         std::string* error)
{
    if (game_key.empty())
    {
        SetError(error, "game-specific key is empty");
        return false;
    }

    std::array<uint8_t, 32> adjusted = session_key_param;
    ++adjusted.back();

    std::array<uint8_t, 32> digest = {};
    if (!crypto::HmacSha256(game_key.data(), game_key.size(), adjusted.data(),
                            adjusted.size(), digest))
    {
        SetError(error, "HMAC-SHA256 session-key derivation failed");
        return false;
    }
    std::copy_n(digest.begin(), session_key.size(), session_key.begin());
    return true;
}

Pia59PacketCodec::Pia59PacketCodec(const std::array<uint8_t, 16>& session_key,
                                   const std::array<uint8_t, 4>& source_ipv4)
    : session_key_(session_key), source_ipv4_(source_ipv4)
{
}

bool Pia59PacketCodec::Crypt(bool encrypt, const uint8_t* input, size_t input_size,
                             const std::array<uint8_t, 12>& nonce,
                             uint8_t* tag, std::vector<uint8_t>& output,
                             std::string* error) const
{
    output.resize(input_size);
    if (!crypto::AesGcm(encrypt, session_key_.data(), nonce.data(), input, input_size,
                        nullptr, 0, tag, output.data()))
    {
        output.clear();
        SetError(error, encrypt ? "AES-GCM encryption failed" : "AES-GCM authentication failed");
        return false;
    }
    return true;
}

bool Pia59PacketCodec::Encode(const Pia59Packet& packet, std::vector<uint8_t>& output,
                              std::string* error) const
{
    std::vector<uint8_t> plaintext;
    for (const Pia59Message& message : packet.messages)
    {
        if (message.payload.size() > 0xFFFF)
        {
            SetError(error, "PIA message payload exceeds 65535 bytes");
            return false;
        }
        const size_t start = plaintext.size();
        const size_t message_size = Align(kPia59MessageHeaderSize + message.payload.size(), 4);
        plaintext.resize(start + message_size, 0);
        uint8_t* header = plaintext.data() + start;
        header[0] = message.flags;
        WriteBe16(header + 1, static_cast<uint16_t>(message.payload.size()));
        WriteBe64(header + 3, message.destination);
        WriteBe64(header + 11, message.source);
        header[19] = message.protocol_type;
        header[20] = message.protocol_port;
        std::copy(message.payload.begin(), message.payload.end(),
                  plaintext.begin() + start + kPia59MessageHeaderSize);
    }
    if (plaintext.empty())
    {
        SetError(error, "PIA packet contains no messages");
        return false;
    }
    plaintext.resize(Align(plaintext.size(), 16), 0xFF);

    std::array<uint8_t, 12> nonce = {};
    std::copy(source_ipv4_.begin(), source_ipv4_.end(), nonce.begin());
    nonce[4] = packet.connection_id;
    std::copy(packet.nonce.begin() + 1, packet.nonce.end(), nonce.begin() + 5);

    output.assign(kPia59HeaderSize, 0);
    std::copy(std::begin(kMagic), std::end(kMagic), output.begin());
    output[4] = kPiaEncrypted;
    output[5] = packet.connection_id;
    WriteBe16(output.data() + 6, packet.packet_id);
    WriteBe16(output.data() + 8, packet.source_timer);
    WriteBe16(output.data() + 10, packet.destination_timer);
    std::copy(packet.nonce.begin(), packet.nonce.end(), output.begin() + 12);

    std::vector<uint8_t> ciphertext;
    if (!Crypt(true, plaintext.data(), plaintext.size(), nonce, output.data() + 20,
               ciphertext, error))
    {
        output.clear();
        return false;
    }
    output.insert(output.end(), ciphertext.begin(), ciphertext.end());
    return true;
}

bool Pia59PacketCodec::Decode(const uint8_t* data, size_t size, Pia59Packet& output,
                              std::string* error) const
{
    output = {};
    if (!data || size < kPia59HeaderSize ||
        !std::equal(std::begin(kMagic), std::end(kMagic), data))
    {
        SetError(error, "invalid PIA 5.9 packet header");
        return false;
    }
    if (data[4] != kPiaEncrypted)
    {
        SetError(error, "PIA packet is not AES-GCM encrypted");
        return false;
    }

    output.connection_id = data[5];
    output.packet_id = ReadBe16(data + 6);
    output.source_timer = ReadBe16(data + 8);
    output.destination_timer = ReadBe16(data + 10);
    std::copy_n(data + 12, output.nonce.size(), output.nonce.begin());

    std::array<uint8_t, 12> nonce = {};
    std::copy(source_ipv4_.begin(), source_ipv4_.end(), nonce.begin());
    nonce[4] = output.connection_id;
    std::copy(output.nonce.begin() + 1, output.nonce.end(), nonce.begin() + 5);
    std::array<uint8_t, 16> tag = {};
    std::copy_n(data + 20, tag.size(), tag.begin());

    std::vector<uint8_t> plaintext;
    if (!Crypt(false, data + kPia59HeaderSize, size - kPia59HeaderSize,
               nonce, tag.data(), plaintext, error))
        return false;

    size_t offset = 0;
    while (offset < plaintext.size())
    {
        if (IsPadding(plaintext.data() + offset, plaintext.size() - offset))
            break;
        if (plaintext.size() - offset < kPia59MessageHeaderSize)
        {
            SetError(error, "truncated PIA message header");
            return false;
        }
        const uint8_t* header = plaintext.data() + offset;
        const size_t payload_size = ReadBe16(header + 1);
        const size_t message_size = Align(kPia59MessageHeaderSize + payload_size, 4);
        if (message_size > plaintext.size() - offset)
        {
            SetError(error, "truncated PIA message payload");
            return false;
        }
        Pia59Message message;
        message.flags = header[0];
        message.destination = ReadBe64(header + 3);
        message.source = ReadBe64(header + 11);
        message.protocol_type = header[19];
        message.protocol_port = header[20];
        message.payload.assign(header + kPia59MessageHeaderSize,
                               header + kPia59MessageHeaderSize + payload_size);
        output.messages.push_back(std::move(message));
        offset += message_size;
    }
    if (output.messages.empty())
    {
        SetError(error, "PIA packet contains no messages");
        return false;
    }
    return true;
}
} // namespace pia
