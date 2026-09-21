#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pia
{
constexpr size_t kPia59HeaderSize = 0x24;
constexpr size_t kPia59MessageHeaderSize = 0x18;
constexpr uint8_t kPiaEncrypted = 2;
constexpr uint8_t kReliableProtocolType = 0x7C;

struct Pia59Message
{
    uint8_t flags = 0;
    uint64_t destination = 0;
    uint64_t source = 0;
    uint8_t protocol_type = 0;
    uint8_t protocol_port = 0;
    std::vector<uint8_t> payload;
};

struct Pia59Packet
{
    uint8_t connection_id = 0;
    uint16_t packet_id = 0;
    uint16_t source_timer = 0;
    uint16_t destination_timer = 0;
    std::array<uint8_t, 8> nonce = {};
    std::vector<Pia59Message> messages;
};

// PIA 5.9 LAN uses the first 16 bytes of HMAC-SHA256 after incrementing
// the last byte of the 32-byte session key parameter.
bool DeriveLanSessionKey(const std::vector<uint8_t>& game_key,
                         const std::array<uint8_t, 32>& session_key_param,
                         std::array<uint8_t, 16>& session_key,
                         std::string* error = nullptr);

class Pia59PacketCodec
{
public:
    Pia59PacketCodec(const std::array<uint8_t, 16>& session_key,
                     const std::array<uint8_t, 4>& source_ipv4);

    bool Encode(const Pia59Packet& packet, std::vector<uint8_t>& output,
                std::string* error = nullptr) const;
    bool Decode(const uint8_t* data, size_t size, Pia59Packet& output,
                std::string* error = nullptr) const;

private:
    bool Crypt(bool encrypt, const uint8_t* input, size_t input_size,
               const std::array<uint8_t, 12>& nonce,
               uint8_t* tag, std::vector<uint8_t>& output,
               std::string* error) const;

    std::array<uint8_t, 16> session_key_;
    std::array<uint8_t, 4> source_ipv4_;
};
} // namespace pia
