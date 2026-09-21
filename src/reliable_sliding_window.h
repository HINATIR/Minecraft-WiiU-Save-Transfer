#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace pia
{
constexpr size_t kReliableHeaderSize = 0x18;
constexpr size_t kReliableFragmentSize = 0x4E0;
constexpr uint16_t kReliableFlagData = 0x0001;
// The target binary sets this bit on the final fragment. This differs from the
// generic flag table for some newer PIA revisions.
constexpr uint16_t kReliableFlagEnd = 0x0002;
constexpr uint32_t kInitialSequence = static_cast<uint32_t>(-2001);

struct ReliableMessage
{
    std::vector<uint8_t> payload;
};

class ReliableSlidingWindow
{
public:
    explicit ReliableSlidingWindow(size_t fragment_size = kReliableFragmentSize);

    void QueueMessage(const uint8_t* data, size_t size);
    void QueueMessage(const std::vector<uint8_t>& data);

    std::vector<std::vector<uint8_t>> Poll(uint64_t now_ms, size_t byte_budget = 64 * 1024);
    std::vector<ReliableMessage> Receive(const uint8_t* data, size_t size);

    bool HasPendingData() const;
    bool HasFailed() const;
    size_t PendingFragmentCount() const;

private:
    struct OutgoingFragment
    {
        uint32_t sequence = 0;
        uint16_t flags = 0;
        std::vector<uint8_t> payload;
        uint64_t last_send_ms = 0;
        unsigned attempts = 0;
    };

    struct IncomingFragment
    {
        uint16_t flags = 0;
        std::vector<uint8_t> payload;
    };

    std::vector<uint8_t> Encode(uint16_t flags, uint32_t sequence,
                                const uint8_t* payload, size_t payload_size) const;
    void ApplyAcknowledgements(uint32_t ack_id, uint64_t extra_acks);
    uint64_t BuildExtraAcknowledgements() const;

    size_t fragment_size_;
    uint32_t next_send_sequence_ = kInitialSequence;
    uint32_t next_receive_sequence_ = kInitialSequence;
    std::deque<OutgoingFragment> outgoing_;
    std::map<uint32_t, IncomingFragment> incoming_;
    std::vector<uint8_t> assembling_;
    bool ack_dirty_ = false;
    bool failed_ = false;
};
} // namespace pia
