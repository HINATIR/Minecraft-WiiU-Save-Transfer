#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pia
{
constexpr size_t kLanApplicationDataSize = 0xA0;
constexpr size_t kLanSessionInfoSize59 = 0x512;
constexpr size_t kLanBrowseRequestSize59 = 0x369;
constexpr size_t kLanBrowseReplySize59 = 0x551;

struct LanHostConfig59
{
    std::array<uint8_t, 4> address{};
    uint16_t port = 0;
    uint32_t variable_id = 0;
    uint32_t game_mode = 0x4E;
    uint16_t maximum_participants = 8;
    uint8_t application_version = 0;
    std::array<uint8_t, kLanApplicationDataSize> application_data{};
    std::string username = "MCU Save Transfer";
};

struct LanBrowseResult59
{
    std::vector<uint8_t> reply;
    std::array<uint8_t, 32> session_key_parameter{};
    uint32_t network_id = 0;
    uint64_t constant_id = 0;
    uint32_t service_variable_id = 0;
};

bool BuildLanBrowseReply59(const uint8_t* request, size_t request_size,
                           const LanHostConfig59& config,
                           const std::vector<uint8_t>& game_key,
                           LanBrowseResult59& output,
                           std::string* error = nullptr);
} // namespace pia
