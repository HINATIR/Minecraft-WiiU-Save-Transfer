#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class TransferPhase : int
{
    Idle,
    Preparing,
    Discovering,
    Connecting,
    WaitingForReceiver,
    Sending,
    Completed,
    Error,
    Cancelled
};

struct AppState
{
    char remote_host[64] = "192.168.0.10";
    bool auto_detect_switch = true;
    char send_path_a[260] = "";
    char send_path_b[260] = "";
    bool use_edited_world_name = false;
    std::string edited_world_name_utf8;
    std::string imported_icon_path;
    std::atomic<bool> sender_running{false};
    std::atomic<bool> sender_cancel{false};
    std::atomic<int> transfer_phase{static_cast<int>(TransferPhase::Idle)};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> total_bytes{0};
    std::string transfer_status_detail;
    std::thread sender_thread;
    std::mutex mutex;
    std::vector<std::string> log;
};
