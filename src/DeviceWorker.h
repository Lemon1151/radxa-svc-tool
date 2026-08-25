#pragma once

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

constexpr UINT WM_APP_DEVICE_UPDATE = WM_APP + 1;

enum class ConnectionState
{
    Connecting,
    Ready,
    DriverMissing,
    UpdateRequired,
    Unavailable,
};

enum class UserNotice
{
    None,
    Applied,
    ActionFailed,
};

struct TemperatureValue
{
    bool Valid = false;
    double Celsius = 0.0;
};

struct PowerValue
{
    bool VoltageValid = false;
    bool CurrentValid = false;
    bool PowerValid = false;
    double Volts = 0.0;
    double Amps = 0.0;
    double Watts = 0.0;
};

struct DeviceSnapshot
{
    std::uint64_t Revision = 0;
    ConnectionState Connection = ConnectionState::Connecting;
    UserNotice Notice = UserNotice::None;
    bool Busy = false;

    bool ProfileSupported = false;
    bool ProfileValid = false;
    std::uint32_t EffectiveProfile = 0;

    bool FanSupported = false;
    bool FanValid = false;
    std::uint32_t FanMode = 2;
    std::uint32_t ManualFanValue = 0;

    bool SensorsSupported = false;
    std::array<TemperatureValue, 2> Temperatures{};
    std::array<PowerValue, 2> UsbPower{};
    PowerValue SystemPower{};
};

class DeviceWorker final
{
public:
    explicit DeviceWorker(HWND notificationWindow);
    ~DeviceWorker();

    DeviceWorker(const DeviceWorker&) = delete;
    DeviceWorker& operator=(const DeviceWorker&) = delete;

    void Start();
    void Stop();
    void Retry();
    void SetProfile(std::uint32_t profile);
    void SetFan(std::uint32_t mode, std::uint32_t manualValue);
    DeviceSnapshot GetSnapshot() const;

private:
    enum class CommandKind
    {
        Retry,
        SetProfile,
        SetFan,
    };

    struct Command
    {
        CommandKind Kind = CommandKind::Retry;
        std::uint32_t Value0 = 0;
        std::uint32_t Value1 = 0;
    };

    void Queue(Command command);
    void ThreadMain();
    void Publish();
    void ResetLiveValues();
    bool OpenDevice(HANDLE& device);
    bool PollDevice(HANDLE device, bool& sensorsEnumerated);
    bool ExecuteCommand(HANDLE device, const Command& command, bool& sensorsEnumerated);

    HWND notificationWindow_ = nullptr;
    mutable std::mutex snapshotMutex_;
    DeviceSnapshot publishedSnapshot_{};
    DeviceSnapshot workingSnapshot_{};
    DeviceSnapshot& snapshot_ = workingSnapshot_;
    std::mutex commandMutex_;
    std::condition_variable commandCondition_;
    std::deque<Command> commands_;
    bool stopping_ = false;
    std::thread thread_;
};
