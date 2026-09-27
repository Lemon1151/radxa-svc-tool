#pragma once

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "RadxaSvcPublic.h"

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

struct FanCurvePoint
{
    double Temperature;
    std::uint32_t Pwm;
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
    std::uint32_t FanMode = RADXA_SVC_FAN_CONTROL_AUTO;
    std::uint32_t ManualFanValue = 0;
    std::uint32_t CurrentFanPwm = 0;

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
    void SetFanCurve(const std::array<FanCurvePoint, 6>& curve);
    std::array<FanCurvePoint, 6> GetFanCurve() const;
    DeviceSnapshot GetSnapshot() const;

    // 默认曲线：50/60/70/80/85/90°C → 25/40/55/70/85/100%
    static constexpr std::array<FanCurvePoint, 6> kDefaultFanCurve = {{
        {50.0,  64},
        {60.0, 102},
        {70.0, 140},
        {80.0, 179},
        {85.0, 217},
        {90.0, 255},
    }};

    static std::uint32_t PercentToPwm(int percent)
    {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        return static_cast<std::uint32_t>(
            (percent * static_cast<int>(RADXA_SVC_FAN_PWM_MAX) + 50) / 100);
    }

    static int PwmToPercent(std::uint32_t pwm)
    {
        if (pwm > RADXA_SVC_FAN_PWM_MAX) pwm = RADXA_SVC_FAN_PWM_MAX;
        return static_cast<int>(
            (pwm * 100u + (RADXA_SVC_FAN_PWM_MAX / 2u)) / RADXA_SVC_FAN_PWM_MAX);
    }

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
    std::uint32_t InterpolateFanPwm(double temperature) const;

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

    mutable std::mutex curveMutex_;
    std::array<FanCurvePoint, 6> fanCurve_ = kDefaultFanCurve;
    std::uint32_t userSelectedMode_ = RADXA_SVC_FAN_CONTROL_AUTO;
    std::uint32_t lastCurvePwm_ = 0;
    bool curveControlActive_ = false;
};
