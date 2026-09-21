#include "reliable_sliding_window.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace pia
{
namespace
{
constexpr uint64_t kResendDelayMs = 500;
constexpr unsigned kMaxAttempts = 8;

uint16_t ReadBe16(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t ReadBe32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t ReadBe64(const uint8_t* data)
{
    return (static_cast<uint64_t>(ReadBe32(data)) << 32) | ReadBe32(data + 4);
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
    WriteBe32(data, static_cast<uint32_t>(value >> 32));
    WriteBe32(data + 4, static_cast<uint32_t>(value));
}

bool SequenceBefore(uint32_t left, uint32_t right)
{
    return static_cast<int32_t>(left - right) < 0;
}
} // namespace

ReliableSlidingWindow::ReliableSlidingWindow(size_t fragment_size)
    : fragment_size_(fragment_size)
{
}

void ReliableSlidingWindow::QueueMessage(const uint8_t* data, size_t size)
{
    if ((size != 0 && data == nullptr) || fragment_size_ == 0)
    {
        failed_ = true;
        return;
    }

    size_t offset = 0;
    do
    {
        const size_t remaining = size - offset;
        const size_t part_size = std::min(fragment_size_, remaining);
        OutgoingFragment fragment;
        fragment.sequence = next_send_sequence_++;
        fragment.flags = kReliableFlagData;
        if (offset + part_size == size)
            fragment.flags |= kReliableFlagEnd;
        if (part_size != 0)
            fragment.payload.assign(data + offset, data + offset + part_size);
        outgoing_.push_back(std::move(fragment));
        offset += part_size;
    } while (offset < size || (size == 0 && offset == 0 && outgoing_.empty()));
}

void ReliableSlidingWindow::QueueMessage(const std::vector<uint8_t>& data)
{
    QueueMessage(data.data(), data.size());
}

std::vector<uint8_t> ReliableSlidingWindow::Encode(uint16_t flags, uint32_t sequence,
                                                   const uint8_t* payload, size_t payload_size) const
{
    std::vector<uint8_t> packet(kReliableHeaderSize + payload_size, 0);
    WriteBe16(packet.data(), flags);
    WriteBe16(packet.data() + 2, static_cast<uint16_t>(payload_size));
    WriteBe32(packet.data() + 4, 0); // Padding in PIA <= 5.12.
    WriteBe32(packet.data() + 8, sequence);
    // PIA 5.9 carries the next sequence id expected by the receiver.
    WriteBe32(packet.data() + 12, next_receive_sequence_);
    WriteBe64(packet.data() + 16, BuildExtraAcknowledgements());
    if (payload_size != 0)
        std::memcpy(packet.data() + kReliableHeaderSize, payload, payload_size);
    return packet;
}

std::vector<std::vector<uint8_t>> ReliableSlidingWindow::Poll(uint64_t now_ms, size_t byte_budget)
{
    std::vector<std::vector<uint8_t>> packets;
    size_t used = 0;

    for (OutgoingFragment& fragment : outgoing_)
    {
        if (fragment.attempts != 0 && now_ms - fragment.last_send_ms < kResendDelayMs)
            continue;
        if (fragment.attempts >= kMaxAttempts)
        {
            failed_ = true;
            continue;
        }

        const size_t packet_size = kReliableHeaderSize + fragment.payload.size();
        if (!packets.empty() && used + packet_size > byte_budget)
            break;
        packets.push_back(Encode(fragment.flags, fragment.sequence,
                                 fragment.payload.data(), fragment.payload.size()));
        used += packet_size;
        fragment.last_send_ms = now_ms;
        ++fragment.attempts;
        ack_dirty_ = false; // ACK state is piggybacked in every data packet.
    }

    if (packets.empty() && ack_dirty_ && kReliableHeaderSize <= byte_budget)
    {
        packets.push_back(Encode(0, 0, nullptr, 0));
        ack_dirty_ = false;
    }
    return packets;
}

void ReliableSlidingWindow::ApplyAcknowledgements(uint32_t ack_id, uint64_t extra_acks)
{
    outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
        [ack_id, extra_acks](const OutgoingFragment& fragment) {
            if (SequenceBefore(fragment.sequence, ack_id))
                return true;
            const uint32_t distance = fragment.sequence - ack_id;
            return distance < 64 && ((extra_acks >> distance) & 1u) != 0;
        }), outgoing_.end());
}

uint64_t ReliableSlidingWindow::BuildExtraAcknowledgements() const
{
    uint64_t bitmap = 0;
    for (const auto& item : incoming_)
    {
        const uint32_t distance = item.first - next_receive_sequence_;
        if (distance < 64)
            bitmap |= uint64_t{1} << distance;
    }
    return bitmap;
}

std::vector<ReliableMessage> ReliableSlidingWindow::Receive(const uint8_t* data, size_t size)
{
    std::vector<ReliableMessage> messages;
    if (data == nullptr || size < kReliableHeaderSize)
        return messages;

    const uint16_t flags = ReadBe16(data);
    const uint16_t payload_size = ReadBe16(data + 2);
    const uint32_t sequence = ReadBe32(data + 8);
    const uint32_t ack_id = ReadBe32(data + 12);
    const uint64_t extra_acks = ReadBe64(data + 16);
    if (size != kReliableHeaderSize + payload_size)
        return messages;

    ApplyAcknowledgements(ack_id, extra_acks);
    if ((flags & kReliableFlagData) == 0)
        return messages;

    const int32_t distance = static_cast<int32_t>(sequence - next_receive_sequence_);
    if (distance < 0)
    {
        ack_dirty_ = true;
        return messages;
    }
    if (distance > 1024)
        return messages;

    IncomingFragment fragment;
    fragment.flags = flags;
    fragment.payload.assign(data + kReliableHeaderSize, data + size);
    incoming_.emplace(sequence, std::move(fragment));
    ack_dirty_ = true;

    for (;;)
    {
        auto found = incoming_.find(next_receive_sequence_);
        if (found == incoming_.end())
            break;
        assembling_.insert(assembling_.end(), found->second.payload.begin(), found->second.payload.end());
        const bool is_end = (found->second.flags & kReliableFlagEnd) != 0;
        incoming_.erase(found);
        ++next_receive_sequence_;
        if (is_end)
        {
            messages.push_back({std::move(assembling_)});
            assembling_.clear();
        }
    }
    return messages;
}

bool ReliableSlidingWindow::HasPendingData() const
{
    return !outgoing_.empty();
}

bool ReliableSlidingWindow::HasFailed() const
{
    return failed_;
}

size_t ReliableSlidingWindow::PendingFragmentCount() const
{
    return outgoing_.size();
}
} // namespace pia
