#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "transfer_worker.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "crypto.h"
#include "pia_lan.h"
#include "pia_packet.h"
#include "reliable_sliding_window.h"
#include "save_transfer_metadata.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <vector>

namespace
{
constexpr uint8_t kPacketJoin = 0xC8;
constexpr uint8_t kPacketData = 0xC9;
constexpr uint8_t kPacketAck = 0xCA;
constexpr size_t kChunkBytes = 0x4000;
constexpr size_t kSaveNameBytes = 0x90;
constexpr size_t kExtensionHeaderBytes = save_transfer::kExtensionHeaderSize;
constexpr int kAckTimeoutMs = 500;
constexpr int kMaxSendAttempts = 8;
constexpr uint64_t kMaxReceiveBytes = 1024ull * 1024ull * 1024ull;
constexpr uint8_t kProtocolStation = 0x14;
constexpr uint8_t kProtocolMesh = 0x18;
constexpr uint8_t kProtocolLan = 0x44;
constexpr int kBrowsePort = 30000;
constexpr int kPcSourcePort = 49154;
constexpr int kSwitchDestinationPort = 49152;

const std::vector<uint8_t> kMinecraftLanKey = {
    0x9A, 0x1C, 0xF3, 0x80, 0xF3, 0xFC, 0x32, 0x5C,
    0xA7, 0x0F, 0x4B, 0xC7, 0xD5, 0xE6, 0x36, 0x97};

void SetPhase(AppState& app, TransferPhase phase)
{
    app.transfer_phase.store(static_cast<int>(phase));
}

void StopWithFailure(AppState& app)
{
    SetPhase(app, app.sender_cancel ? TransferPhase::Cancelled : TransferPhase::Error);
    {
        std::lock_guard<std::mutex> lock(app.mutex);
        if (!app.log.empty())
            app.transfer_status_detail = app.log.back();
    }
    app.sender_running = false;
}

uint32_t ReadBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint16_t ReadBe16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint64_t ReadBe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value = (value << 8) | data[index];
    return value;
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

uint64_t MonotonicMilliseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool SecureRandom(void* output, size_t size)
{
    return crypto::RandomBytes(output, size);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& output)
{
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file)
        return false;
    output.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

bool DetectLocalAddressForTarget(const sockaddr_in& target, sockaddr_in& local)
{
    SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET)
        return false;

    if (connect(probe, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == SOCKET_ERROR)
    {
        closesocket(probe);
        return false;
    }

    int local_size = sizeof(local);
    const bool ok = getsockname(probe, reinterpret_cast<sockaddr*>(&local), &local_size) == 0;
    closesocket(probe);
    if (!ok)
        return false;

    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<u_short>(kPcSourcePort));
    return true;
}

std::string AddressToString(const sockaddr_in& address)
{
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
    return text;
}

struct LivePiaSession
{
    std::unique_ptr<pia::Pia59PacketCodec> outbound_codec;
    std::unique_ptr<pia::Pia59PacketCodec> inbound_codec;
    uint64_t local_constant_id = 0;
    uint64_t remote_constant_id = 0;
    uint32_t local_variable_id = 0;
    uint32_t remote_variable_id = 0;
    uint32_t network_id = 0;
    uint8_t local_connection_id = 0;
    uint8_t remote_connection_id = 0;
    uint16_t next_packet_id = 1;
    uint16_t peer_timer = 0;
    uint32_t next_ack_id = 0;
    uint32_t lan_reply_id = 0;
    uint16_t lan_sequence = 0;
    uint64_t started_ms = 0;
    std::array<uint8_t, 70> local_connection_info{};
    std::array<uint8_t, 70> remote_connection_info{};
    std::array<uint8_t, 50> remote_lan_station{};
    std::vector<uint8_t> session_info;
};

void BuildStationLocation(const std::array<uint8_t, 4>& address, uint16_t port,
                          uint64_t constant_id, uint32_t variable_id,
                          uint32_t service_variable_id, uint8_t* output)
{
    std::fill_n(output, 35, static_cast<uint8_t>(0));
    std::copy(address.begin(), address.end(), output);
    WriteBe16(output + 4, port);
    WriteBe64(output + 6, constant_id);
    WriteBe32(output + 14, variable_id);
    WriteBe32(output + 18, service_variable_id);
    output[25] = 1;
    output[26] = 2;
    output[27] = 3;
}

bool SendPiaMessages(SOCKET sock, const sockaddr_in& target, LivePiaSession& session,
                     std::vector<pia::Pia59Message> messages, uint8_t connection_id = 0)
{
    pia::Pia59Packet packet;
    packet.connection_id = connection_id;
    packet.packet_id = connection_id == 0 ? 0 : session.next_packet_id++;
    packet.source_timer = static_cast<uint16_t>(MonotonicMilliseconds() - session.started_ms);
    packet.destination_timer = session.peer_timer;
    if (!SecureRandom(packet.nonce.data(), packet.nonce.size()))
        return false;
    packet.messages = std::move(messages);

    std::vector<uint8_t> encoded;
    std::string error;
    if (!session.outbound_codec->Encode(packet, encoded, &error))
        return false;

    const int sent = sendto(sock, reinterpret_cast<const char*>(encoded.data()),
                            static_cast<int>(encoded.size()), 0,
                            reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    return sent == static_cast<int>(encoded.size());
}

bool SendPiaPayload(SOCKET sock, const sockaddr_in& target, LivePiaSession& session,
                    uint8_t protocol, const std::vector<uint8_t>& payload,
                    uint64_t destination = 0, uint8_t connection_id = 0)
{
    pia::Pia59Message message;
    message.flags = protocol == kProtocolLan ? 0x9 : 0x1;
    message.destination = destination;
    message.source = session.local_constant_id;
    message.protocol_type = protocol;
    message.protocol_port = 0;
    message.payload = payload;
    return SendPiaMessages(sock, target, session, {std::move(message)}, connection_id);
}

bool SendLanSessionDescription(SOCKET sock, const sockaddr_in& target,
                               LivePiaSession& session, bool include_remote)
{
    if (session.session_info.size() != pia::kLanSessionInfoSize59)
        return false;

    std::vector<uint8_t> info = session.session_info;
    if (include_remote)
    {
        WriteBe16(info.data() + 0x20, 2);
        std::copy(session.remote_lan_station.begin(), session.remote_lan_station.end(),
                  info.begin() + 0x1D2 + 50);
    }

    bool sent = true;
    for (size_t offset = 0, fragment = 0; offset < info.size(); offset += 800, ++fragment)
    {
        const size_t count = std::min<size_t>(800, info.size() - offset);
        std::vector<uint8_t> response(24 + count, 0);
        response[0] = 6;
        WriteBe32(response.data() + 12, session.lan_reply_id);
        WriteBe16(response.data() + 16, session.lan_sequence);
        response[18] = static_cast<uint8_t>(fragment);
        response[19] = 2;
        WriteBe32(response.data() + 20, static_cast<uint32_t>(count));
        std::copy_n(info.begin() + offset, count, response.begin() + 24);
        for (int copy = 0; copy < 3; ++copy)
            sent = SendPiaPayload(sock, target, session, kProtocolLan, response) && sent;
    }
    ++session.lan_sequence;
    return sent;
}

bool HandleClockMessage(SOCKET sock, const sockaddr_in& target, LivePiaSession& session,
                        const pia::Pia59Message& message, AppState* diagnostics)
{
    if (message.protocol_type == 0x58 && message.payload.size() == 16 &&
        ReadBe32(message.payload.data()) == 0)
    {
        std::vector<uint8_t> response = message.payload;
        WriteBe32(response.data(), 1);
        const bool sent = SendPiaPayload(sock, target, session, 0x58, response, 2,
                                         session.local_connection_id);
        if (diagnostics)
            Log(*diagnostics, sent ? "answered RTT request" : "failed to answer RTT request");
        return true;
    }

    if (message.protocol_type == 0x1C && message.payload.size() == 16)
    {
        std::vector<uint8_t> response = message.payload;
        WriteBe64(response.data() + 8, MonotonicMilliseconds() - session.started_ms);
        const bool sent = SendPiaPayload(sock, target, session, 0x1C, response, 2,
                                         session.local_connection_id);
        if (diagnostics)
            Log(*diagnostics, sent ? "answered Sync Clock request" :
                                     "failed to answer Sync Clock request");
        return true;
    }
    return false;
}

std::vector<pia::Pia59Message> ReceivePiaMessages(SOCKET sock, const sockaddr_in& target,
                                                  LivePiaSession& session,
                                                  AppState* diagnostics = nullptr,
                                                  sockaddr_in* discovered_target = nullptr)
{
    std::array<uint8_t, 2048> datagram{};
    sockaddr_in from = {};
    int from_size = sizeof(from);
    const int received = recvfrom(sock, reinterpret_cast<char*>(datagram.data()),
                                  static_cast<int>(datagram.size()), 0,
                                  reinterpret_cast<sockaddr*>(&from), &from_size);
    if (received <= 0)
        return {};
    if (from.sin_addr.s_addr != target.sin_addr.s_addr)
    {
        if (diagnostics)
            Log(*diagnostics, "ignored UDP packet on transport port from an unexpected endpoint");
        return {};
    }

    const bool port_changed = from.sin_port != target.sin_port;
    if (port_changed && !discovered_target)
    {
        if (diagnostics)
            Log(*diagnostics, "ignored UDP packet on transport port from an unexpected endpoint");
        return {};
    }

    pia::Pia59Packet packet;
    std::string error;
    if (!session.inbound_codec->Decode(datagram.data(), static_cast<size_t>(received),
                                       packet, &error))
    {
        if (diagnostics)
        {
            std::ostringstream line;
            line << "received " << received << " UDP bytes, but PIA decode failed: " << error;
            Log(*diagnostics, line.str());
        }
        return {};
    }

    if (port_changed)
    {
        discovered_target->sin_port = from.sin_port;
        if (diagnostics)
        {
            std::ostringstream line;
            line << "discovered Switch transport port UDP " << ntohs(from.sin_port);
            Log(*diagnostics, line.str());
        }
    }

    session.peer_timer = packet.source_timer;
    if (diagnostics)
    {
        std::ostringstream line;
        line << "PIA packet received: connection " << static_cast<unsigned>(packet.connection_id)
             << ", " << packet.messages.size() << " message(s)";
        Log(*diagnostics, line.str());
    }
    return packet.messages;
}

std::vector<uint8_t> BuildStationAck(uint32_t ack_id)
{
    std::vector<uint8_t> payload(8, 0);
    payload[0] = 5;
    WriteBe32(payload.data() + 4, ack_id);
    return payload;
}

std::vector<uint8_t> BuildStationRequest(LivePiaSession& session)
{
    std::vector<uint8_t> payload(91, 0);
    payload[0] = 1;
    payload[1] = session.local_connection_id;
    payload[2] = 8;
    payload[3] = 1;
    WriteBe64(payload.data() + 4, session.remote_constant_id);
    WriteBe32(payload.data() + 12, session.remote_variable_id);
    payload[16] = session.remote_connection_id;
    std::copy(session.local_connection_info.begin(), session.local_connection_info.end(),
              payload.begin() + 17);
    WriteBe32(payload.data() + 87, session.next_ack_id++);
    return payload;
}

std::vector<uint8_t> BuildStationResponse(LivePiaSession& session)
{
    std::vector<uint8_t> payload(255, 0);
    payload[0] = 2;
    payload[2] = 8;
    payload[3] = 3;
    WriteBe64(payload.data() + 5, session.remote_constant_id);
    WriteBe32(payload.data() + 13, session.remote_variable_id);
    const char token[] = "Friend";
    std::copy(std::begin(token), std::end(token) - 1, payload.begin() + 17);
    WriteBe32(payload.data() + 49, session.network_id);
    payload[53] = 1;
    payload[54] = 1;
    payload[55] = 1;
    const char player[] = "MCU Save Transfer";
    std::copy(std::begin(player), std::end(player) - 1, payload.begin() + 56);
    payload[56 + 0x50] = 1;
    std::copy(std::begin(token), std::end(token) - 1, payload.begin() + 56 + 0x51);
    payload[56 + 0x79] = 1;
    WriteBe32(payload.data() + 251, session.next_ack_id++);
    return payload;
}

void WriteMeshStationInfo(uint8_t* output, const std::array<uint8_t, 70>& connection,
                          uint8_t station_index)
{
    std::copy(connection.begin(), connection.end(), output);
    output[70] = station_index;
    output[71] = 0;
}

std::vector<uint8_t> BuildMeshJoinResponse(LivePiaSession& session)
{
    std::vector<uint8_t> payload(164, 0);
    payload[0] = 2;
    payload[1] = 2;
    payload[3] = 1;
    payload[4] = 1;
    payload[6] = 2;
    payload[8] = 8;
    payload[10] = 8;
    WriteMeshStationInfo(payload.data() + 16, session.local_connection_info, 0);
    WriteMeshStationInfo(payload.data() + 88, session.remote_connection_info, 1);
    WriteBe32(payload.data() + 160, session.next_ack_id++);
    return payload;
}

std::vector<uint8_t> BuildMeshUpdate(const LivePiaSession& session)
{
    std::vector<uint8_t> payload(588, 0);
    payload[0] = 0x20;
    payload[1] = 2;
    WriteBe32(payload.data() + 4, 1);
    payload[8] = 1;
    payload[10] = 2;
    WriteMeshStationInfo(payload.data() + 12, session.local_connection_info, 0);
    WriteMeshStationInfo(payload.data() + 84, session.remote_connection_info, 1);
    return payload;
}

bool EstablishPiaLanHost(AppState& app, SOCKET transport, sockaddr_in& local,
                         sockaddr_in& target,
                         const std::array<uint8_t, save_transfer::kApplicationDataSize>& application_data,
                         LivePiaSession& session, bool auto_detect_switch)
{
    SOCKET browse = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (browse == INVALID_SOCKET)
    {
        Log(app, "PIA discovery failed: socket creation failed");
        return false;
    }

    BOOL enabled = TRUE;
    setsockopt(browse, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    DWORD timeout_ms = 250;
    setsockopt(browse, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in browse_address = local;
    browse_address.sin_addr.s_addr = INADDR_ANY;
    browse_address.sin_port = htons(kBrowsePort);
    if (bind(browse, reinterpret_cast<const sockaddr*>(&browse_address), sizeof(browse_address)) == SOCKET_ERROR)
    {
        closesocket(browse);
        Log(app, "PIA discovery failed: could not bind UDP 30000");
        return false;
    }

    pia::LanHostConfig59 config;
    std::memcpy(config.address.data(), &local.sin_addr.s_addr, config.address.size());
    config.port = ntohs(local.sin_port);
    do
    {
        SecureRandom(&config.variable_id, sizeof(config.variable_id));
    } while (config.variable_id == 0);
    config.application_data = application_data;

    pia::LanBrowseResult59 browse_result;
    const uint64_t browse_deadline = MonotonicMilliseconds() + 45000;
    Log(app, auto_detect_switch ?
             "waiting for Switch LAN browse request on UDP 30000 (auto detect)" :
             "waiting for Switch LAN browse request on UDP 30000");
    bool replied = false;
    while (!app.sender_cancel && MonotonicMilliseconds() < browse_deadline)
    {
        std::array<uint8_t, 1400> request{};
        sockaddr_in from = {};
        int from_size = sizeof(from);
        const int received = recvfrom(browse, reinterpret_cast<char*>(request.data()),
                                      static_cast<int>(request.size()), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_size);
        if (received <= 0)
            continue;
        if (!auto_detect_switch && from.sin_addr.s_addr != target.sin_addr.s_addr)
            continue;

        sockaddr_in candidate_target = target;
        candidate_target.sin_family = AF_INET;
        candidate_target.sin_addr = from.sin_addr;
        candidate_target.sin_port = htons(static_cast<u_short>(kSwitchDestinationPort));

        std::string error;
        if (auto_detect_switch || local.sin_addr.s_addr == INADDR_ANY)
        {
            sockaddr_in detected_local = {};
            if (!DetectLocalAddressForTarget(candidate_target, detected_local))
            {
                Log(app, "ignored LAN browse packet: could not detect local IPv4 for sender");
                continue;
            }
            local = detected_local;
        }
        target = candidate_target;
        std::memcpy(config.address.data(), &local.sin_addr.s_addr, config.address.size());
        config.port = ntohs(local.sin_port);

        if (!pia::BuildLanBrowseReply59(request.data(), static_cast<size_t>(received), config,
                                        kMinecraftLanKey, browse_result, &error))
        {
            Log(app, "ignored LAN browse packet: " + error);
            continue;
        }

        for (int copy = 0; copy < 3; ++copy)
        {
            sendto(browse, reinterpret_cast<const char*>(browse_result.reply.data()),
                   static_cast<int>(browse_result.reply.size()), 0,
                   reinterpret_cast<const sockaddr*>(&from), sizeof(from));
        }
        replied = true;
        if (auto_detect_switch)
        {
            std::ostringstream line;
            line << "auto detected Switch IPv4 " << AddressToString(target);
            Log(app, line.str());
        }
        Log(app, "answered Switch LAN browse request");
        break;
    }
    closesocket(browse);
    if (!replied)
        return false;

    std::array<uint8_t, 16> session_key{};
    std::string key_error;
    if (!pia::DeriveLanSessionKey(kMinecraftLanKey, browse_result.session_key_parameter,
                                  session_key, &key_error))
    {
        Log(app, "PIA session key failed: " + key_error);
        return false;
    }

    std::array<uint8_t, 4> local_ip{};
    std::array<uint8_t, 4> remote_ip{};
    std::memcpy(local_ip.data(), &local.sin_addr.s_addr, local_ip.size());
    std::memcpy(remote_ip.data(), &target.sin_addr.s_addr, remote_ip.size());
    session.outbound_codec = std::make_unique<pia::Pia59PacketCodec>(session_key, local_ip);
    session.inbound_codec = std::make_unique<pia::Pia59PacketCodec>(session_key, remote_ip);
    session.local_constant_id = browse_result.constant_id;
    session.local_variable_id = config.variable_id;
    session.network_id = browse_result.network_id;
    session.session_info.assign(browse_result.reply.begin() + 5,
                                browse_result.reply.begin() + 5 + pia::kLanSessionInfoSize59);
    session.started_ms = MonotonicMilliseconds();

    do
    {
        SecureRandom(&session.local_connection_id, sizeof(session.local_connection_id));
    } while (session.local_connection_id < 2);
    SecureRandom(&session.next_ack_id, sizeof(session.next_ack_id));
    SecureRandom(&session.lan_reply_id, sizeof(session.lan_reply_id));

    const uint32_t service_id = browse_result.service_variable_id;
    BuildStationLocation(local_ip, config.port, session.local_constant_id,
                         session.local_variable_id, service_id,
                         session.local_connection_info.data());
    BuildStationLocation(local_ip, config.port, session.local_constant_id,
                         session.local_variable_id, service_id,
                         session.local_connection_info.data() + 35);

    bool inverse_request_sent = false;
    bool station_response_sent = false;
    bool mesh_response_sent = false;
    uint32_t mesh_response_ack = 0;
    const uint64_t handshake_deadline = MonotonicMilliseconds() + 15000;
    SetPhase(app, TransferPhase::Connecting);
    Log(app, "PIA LAN discovered; establishing Station and Mesh session");
    while (!app.sender_cancel && MonotonicMilliseconds() < handshake_deadline)
    {
        for (const pia::Pia59Message& message :
             ReceivePiaMessages(transport, target, session, &app, &target))
        {
            const std::vector<uint8_t>& payload = message.payload;
            if (payload.empty())
                continue;

            {
                std::ostringstream line;
                line << "PIA message: protocol 0x" << std::hex
                     << static_cast<unsigned>(message.protocol_type) << ", type 0x"
                     << static_cast<unsigned>(payload[0]) << std::dec
                     << ", " << payload.size() << " bytes";
                Log(app, line.str());
            }

            if (HandleClockMessage(transport, target, session, message, &app))
                continue;

            if (message.protocol_type == kProtocolLan && payload[0] == 5)
            {
                SendLanSessionDescription(transport, target, session, false);
                Log(app, "sent fragmented LAN session description");
            }
            else if (message.protocol_type == kProtocolLan && payload[0] == 3)
            {
                std::vector<uint8_t> response(86, 0);
                response[0] = 4;
                WriteBe32(response.data() + 12, session.network_id);
                std::copy(session.local_connection_info.begin(), session.local_connection_info.end(),
                          response.begin() + 16);
                for (int copy = 0; copy < 3; ++copy)
                    SendPiaPayload(transport, target, session, kProtocolLan, response);
                Log(app, "sent LAN host connection information");
            }
            else if (message.protocol_type == kProtocolStation && payload[0] == 1 &&
                     payload.size() >= 91)
            {
                session.remote_connection_id = payload[1];
                session.remote_constant_id = ReadBe64(payload.data() + 23);
                session.remote_variable_id = ReadBe32(payload.data() + 31);
                std::copy_n(payload.begin() + 17, session.remote_connection_info.size(),
                            session.remote_connection_info.begin());
                SendPiaPayload(transport, target, session, kProtocolStation,
                               BuildStationAck(ReadBe32(payload.data() + payload.size() - 4)));
                if (!inverse_request_sent)
                {
                    SendPiaPayload(transport, target, session, kProtocolStation,
                                   BuildStationRequest(session));
                    inverse_request_sent = true;
                    Log(app, "accepted Station request and sent inverse request");
                }
            }
            else if (message.protocol_type == kProtocolStation && payload[0] == 2 &&
                     payload.size() >= 8)
            {
                if (payload.size() >= 56 + 0xC3)
                {
                    session.remote_lan_station.fill(0);
                    session.remote_lan_station[0] = 2;
                    session.remote_lan_station[1] = payload[56 + 0x50];
                    std::copy_n(payload.begin() + 56, 40,
                                session.remote_lan_station.begin() + 2);
                    WriteBe64(session.remote_lan_station.data() + 0x2A,
                              session.remote_constant_id);
                }
                SendPiaPayload(transport, target, session, kProtocolStation,
                               BuildStationAck(ReadBe32(payload.data() + payload.size() - 4)));
                if (!station_response_sent)
                {
                    SendPiaPayload(transport, target, session, kProtocolStation,
                                   BuildStationResponse(session));
                    station_response_sent = true;
                    Log(app, "accepted Station response and sent host response");
                }
            }
            else if (message.protocol_type == kProtocolMesh && payload[0] == 1 &&
                     payload.size() >= 14)
            {
                SendPiaPayload(transport, target, session, kProtocolStation,
                               BuildStationAck(ReadBe32(payload.data() + payload.size() - 4)));
                std::vector<uint8_t> response = BuildMeshJoinResponse(session);
                mesh_response_ack = ReadBe32(response.data() + response.size() - 4);
                SendPiaPayload(transport, target, session, kProtocolMesh, response);
                mesh_response_sent = true;
                Log(app, "accepted Mesh join request and sent join response");
            }
            else if (message.protocol_type == kProtocolStation && payload[0] == 5 &&
                     payload.size() == 8 && mesh_response_sent &&
                     ReadBe32(payload.data() + 4) == mesh_response_ack)
            {
                SendPiaPayload(transport, target, session, kProtocolMesh,
                               BuildMeshUpdate(session), 2);
                if (SendLanSessionDescription(transport, target, session, true))
                    Log(app, "announced updated LAN session with 2 participants");
                else
                    Log(app, "failed to announce updated LAN session");
                SetPhase(app, TransferPhase::WaitingForReceiver);
                Log(app, "PIA Station/Mesh session established");
                return true;
            }
        }
    }

    Log(app, "PIA Station/Mesh handshake timed out");
    return false;
}

bool SendWindowPacketsPia(SOCKET sock, const sockaddr_in& target, LivePiaSession& session,
                          pia::ReliableSlidingWindow& window)
{
    for (const std::vector<uint8_t>& payload : window.Poll(MonotonicMilliseconds()))
    {
        if (!SendPiaPayload(sock, target, session, pia::kReliableProtocolType, payload, 2,
                            session.local_connection_id))
            return false;
    }
    return true;
}

std::vector<pia::ReliableMessage> ReceiveWindowPacketsPia(
    AppState& app, SOCKET sock, const sockaddr_in& target, LivePiaSession& session,
    pia::ReliableSlidingWindow& window)
{
    std::vector<pia::ReliableMessage> received_messages;
    for (const pia::Pia59Message& message : ReceivePiaMessages(sock, target, session, &app))
    {
        if (!message.payload.empty())
        {
            std::ostringstream line;
            line << "connected PIA message: protocol 0x" << std::hex
                 << static_cast<unsigned>(message.protocol_type) << ", type 0x"
                 << static_cast<unsigned>(message.payload[0]) << std::dec
                 << ", " << message.payload.size() << " bytes";
            Log(app, line.str());
        }

        if (HandleClockMessage(sock, target, session, message, &app))
            continue;

        if (message.protocol_type == kProtocolLan && !message.payload.empty() &&
            message.payload[0] == 5)
        {
            if (SendLanSessionDescription(sock, target, session, true))
                Log(app, "sent updated LAN session description for 2 participants");
            else
                Log(app, "failed to send updated LAN session description");
            continue;
        }

        if (message.protocol_type != pia::kReliableProtocolType)
            continue;

        if (message.payload.size() >= pia::kReliableHeaderSize)
        {
            std::ostringstream line;
            line << "RSW frame: flags 0x" << std::hex << ReadBe16(message.payload.data())
                 << std::dec << ", payload " << ReadBe16(message.payload.data() + 2)
                 << ", sequence " << ReadBe32(message.payload.data() + 8);
            Log(app, line.str());
        }

        std::vector<pia::ReliableMessage> current =
            window.Receive(message.payload.data(), message.payload.size());
        received_messages.insert(received_messages.end(),
                                 std::make_move_iterator(current.begin()),
                                 std::make_move_iterator(current.end()));
    }
    return received_messages;
}

bool WaitForJoinPacket(AppState& app, SOCKET sock, const sockaddr_in& target,
                       LivePiaSession& session, pia::ReliableSlidingWindow& window)
{
    const uint64_t deadline = MonotonicMilliseconds() + 30000;
    Log(app, "waiting for C8 join packet from the receiver");
    while (!app.sender_cancel && MonotonicMilliseconds() < deadline)
    {
        if (!SendWindowPacketsPia(sock, target, session, window))
            return false;

        for (const pia::ReliableMessage& message :
             ReceiveWindowPacketsPia(app, sock, target, session, window))
        {
            if (message.payload.size() == 1 + kSaveNameBytes && message.payload[0] == kPacketJoin)
            {
                Log(app, "received C8 join packet");
                SendWindowPacketsPia(sock, target, session, window);
                return true;
            }
        }
    }
    return false;
}

bool SendDataMessage(AppState& app, SOCKET sock, const sockaddr_in& target,
                     LivePiaSession& session, pia::ReliableSlidingWindow& window,
                     const std::vector<uint8_t>& message)
{
    window.QueueMessage(message);
    const uint64_t deadline = MonotonicMilliseconds() +
                              static_cast<uint64_t>(kAckTimeoutMs * (kMaxSendAttempts + 2));
    bool app_acknowledged = false;
    while (!app.sender_cancel && MonotonicMilliseconds() < deadline)
    {
        if (!SendWindowPacketsPia(sock, target, session, window))
            return false;

        for (const pia::ReliableMessage& incoming :
             ReceiveWindowPacketsPia(app, sock, target, session, window))
        {
            if (incoming.payload.size() == 1 && incoming.payload[0] == kPacketAck)
                app_acknowledged = true;
        }

        if (app_acknowledged && !window.HasPendingData())
            return true;
        if (window.HasFailed())
            return false;
    }
    return false;
}

void SendFile(AppState& app, std::string path_a, std::string path_b, std::string remote_host,
              bool use_edited_world_name, std::string edited_world_name_utf8,
              std::string imported_icon_path, bool auto_detect_switch)
{
    std::vector<uint8_t> bytes_a;
    std::vector<uint8_t> bytes_b;
    save_transfer::Metadata metadata;
    if (!ReadFile(path_a, bytes_a))
    {
        Log(app, "send failed: could not open file A");
        StopWithFailure(app);
        return;
    }
    const bool use_default_extension = path_b.empty();
    if (use_default_extension)
    {
        save_transfer::BuildDefaultExtensionFile(bytes_b);
        save_transfer::BuildDefaultMetadata(metadata);
        Log(app, "no .ext file selected; using default transparent icon");
    }
    else if (!ReadFile(path_b, bytes_b))
    {
        Log(app, "send failed: could not open file B");
        StopWithFailure(app);
        return;
    }
    if (!use_default_extension && bytes_b.size() < kExtensionHeaderBytes)
    {
        Log(app, "send failed: file B is smaller than its 0x100-byte header");
        StopWithFailure(app);
        return;
    }

    std::string metadata_error;
    if (!use_default_extension &&
        !save_transfer::ParseExtensionFile(bytes_b, metadata, &metadata_error))
    {
        Log(app, "send failed: " + metadata_error);
        StopWithFailure(app);
        return;
    }

    if (use_edited_world_name)
    {
        if (!save_transfer::SetExtensionWorldName(bytes_b, edited_world_name_utf8,
                                                  &metadata, &metadata_error))
        {
            Log(app, "send failed: " + metadata_error);
            StopWithFailure(app);
            return;
        }
        Log(app, "using edited world name: " + edited_world_name_utf8);
    }

    if (!imported_icon_path.empty())
    {
        std::vector<uint8_t> imported_icon;
        if (!ReadFile(imported_icon_path, imported_icon))
        {
            Log(app, "send failed: could not open imported icon PNG");
            StopWithFailure(app);
            return;
        }
        if (!save_transfer::ReplaceExtensionIconPng(bytes_b, imported_icon, &metadata_error))
        {
            Log(app, "send failed: " + metadata_error);
            StopWithFailure(app);
            return;
        }
        Log(app, "using imported icon PNG");
    }

    const size_t extension_payload_size = bytes_b.size() - kExtensionHeaderBytes;
    if (bytes_a.size() > kMaxReceiveBytes ||
        extension_payload_size > kMaxReceiveBytes - bytes_a.size())
    {
        Log(app, "send failed: combined data exceeds the 1 GiB safety limit");
        StopWithFailure(app);
        return;
    }

    std::array<uint8_t, save_transfer::kApplicationDataSize> application_data{};
    if (!save_transfer::BuildApplicationData(bytes_a.size(), extension_payload_size,
                                             metadata, application_data, &metadata_error))
    {
        Log(app, "send failed: " + metadata_error);
        StopWithFailure(app);
        return;
    }

    {
        std::ostringstream message;
        message << "LAN metadata ready: game mode 0x4e, seed " << metadata.seed
                << ", host options 0x" << std::hex << metadata.host_options;
        Log(app, message.str());
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(bytes_a.size() + extension_payload_size);
    bytes.insert(bytes.end(), bytes_a.begin(), bytes_a.end());
    bytes.insert(bytes.end(), bytes_b.begin() + kExtensionHeaderBytes, bytes_b.end());
    app.total_bytes = bytes.size();
    app.bytes_sent = 0;
    SetPhase(app, TransferPhase::Discovering);

    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(static_cast<u_short>(kSwitchDestinationPort));
    if (!auto_detect_switch && inet_pton(AF_INET, remote_host.c_str(), &target.sin_addr) != 1)
    {
        Log(app, "send failed: Switch IPv4 must be an IPv4 address");
        StopWithFailure(app);
        return;
    }

    sockaddr_in local = {};
    if (auto_detect_switch)
    {
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<u_short>(kPcSourcePort));
        local.sin_addr.s_addr = INADDR_ANY;
        Log(app, "Switch IPv4 auto detection enabled");
    }
    else if (!DetectLocalAddressForTarget(target, local))
    {
        Log(app, "send failed: could not detect the local IPv4 address for the Switch");
        StopWithFailure(app);
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
    {
        Log(app, "send failed: socket creation failed");
        StopWithFailure(app);
        return;
    }

    DWORD timeout_ms = kAckTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in bind_address = local;
    bind_address.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) == SOCKET_ERROR)
    {
        closesocket(sock);
        Log(app, "send failed: could not bind UDP 49154");
        StopWithFailure(app);
        return;
    }

    {
        std::ostringstream message;
        if (auto_detect_switch)
            message << "using UDP " << kPcSourcePort
                    << " and waiting for Switch discovery";
        else
            message << "using PC " << AddressToString(local) << ':' << kPcSourcePort
                    << " -> Switch " << remote_host << ':' << kSwitchDestinationPort;
        Log(app, message.str());
    }

    LivePiaSession pia_session;
    if (!EstablishPiaLanHost(app, sock, local, target, application_data, pia_session,
                             auto_detect_switch))
    {
        closesocket(sock);
        Log(app, app.sender_cancel ? "send cancelled" : "send failed: PIA LAN connection failed");
        StopWithFailure(app);
        return;
    }

    pia::ReliableSlidingWindow window;
    if (!WaitForJoinPacket(app, sock, target, pia_session, window))
    {
        closesocket(sock);
        Log(app, app.sender_cancel ? "send cancelled" : "send failed: no C8 join packet");
        StopWithFailure(app);
        return;
    }

    SetPhase(app, TransferPhase::Sending);
    uint32_t message_count = 0;
    for (size_t offset = 0; offset < bytes.size() && !app.sender_cancel; offset += kChunkBytes)
    {
        const size_t chunk_size = std::min(kChunkBytes, bytes.size() - offset);
        std::vector<uint8_t> packet(1 + chunk_size);
        packet[0] = kPacketData;
        std::copy(bytes.begin() + offset, bytes.begin() + offset + chunk_size, packet.begin() + 1);
        if (!SendDataMessage(app, sock, target, pia_session, window, packet))
        {
            closesocket(sock);
            Log(app, app.sender_cancel ? "send cancelled" : "send failed: C9/CA timeout");
            StopWithFailure(app);
            return;
        }
        ++message_count;
        app.bytes_sent = offset + chunk_size;
    }

    closesocket(sock);
    if (!app.sender_cancel)
    {
        std::ostringstream message;
        message << "sent A=" << bytes_a.size() << " + B payload=" << extension_payload_size
                << " bytes (total " << bytes.size() << ") in " << message_count
                << " C9 data packets";
        Log(app, message.str());
    }
    SetPhase(app, app.sender_cancel ? TransferPhase::Cancelled : TransferPhase::Completed);
    {
        std::lock_guard<std::mutex> lock(app.mutex);
        app.transfer_status_detail.clear();
    }
    app.sender_running = false;
}
} // namespace

void Log(AppState& app, const std::string& line)
{
    std::lock_guard<std::mutex> lock(app.mutex);
    app.log.push_back(line);
    if (app.log.size() > 300)
        app.log.erase(app.log.begin(), app.log.begin() + 50);
}

void StartSender(AppState& app)
{
    if (app.sender_running)
        return;

    if (app.sender_thread.joinable())
        app.sender_thread.join();

    app.sender_cancel = false;
    app.bytes_sent = 0;
    app.total_bytes = 0;
    {
        std::lock_guard<std::mutex> lock(app.mutex);
        app.transfer_status_detail.clear();
    }
    SetPhase(app, TransferPhase::Preparing);
    app.sender_running = true;
    app.sender_thread = std::thread(SendFile, std::ref(app),
                                    std::string(app.send_path_a),
                                    std::string(app.send_path_b),
                                    std::string(app.remote_host),
                                    app.use_edited_world_name,
                                    app.edited_world_name_utf8,
                                    app.imported_icon_path,
                                    app.auto_detect_switch);
}

void StopSender(AppState& app)
{
    app.sender_cancel = true;
    if (app.sender_thread.joinable())
        app.sender_thread.join();
    SetPhase(app, TransferPhase::Cancelled);
    app.sender_running = false;
}
