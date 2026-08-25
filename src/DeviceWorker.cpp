#include "DeviceWorker.h"

#include "RadxaSvcPublic.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
constexpr auto kPollInterval = std::chrono::seconds(1);
constexpr auto kReconnectInterval = std::chrono::seconds(2);

template <typename T>
bool ReadIoctl(HANDLE device, int code, T& output)
{
    RADXA_SVC_STRUCT_HEADER request{
        sizeof(RADXA_SVC_STRUCT_HEADER), RADXA_SVC_USER_API_VERSION};
    ZeroMemory(&output, sizeof(output));
    DWORD bytesReturned = 0;
    const BOOL result = DeviceIoControl(
        device,
        static_cast<DWORD>(code),
        &request,
        sizeof(request),
        &output,
        sizeof(output),
        &bytesReturned,
        nullptr);
    return result && bytesReturned == sizeof(output);
}

template <typename TRequest, typename TOutput>
bool ReadIoctlWithRequest(
    HANDLE device,
    int code,
    const TRequest& request,
    TOutput& output)
{
    ZeroMemory(&output, sizeof(output));
    DWORD bytesReturned = 0;
    const BOOL result = DeviceIoControl(
        device,
        static_cast<DWORD>(code),
        const_cast<TRequest*>(&request),
        sizeof(request),
        &output,
        sizeof(output),
        &bytesReturned,
        nullptr);
    return result && bytesReturned == sizeof(output);
}

template <typename TRequest>
bool WriteIoctl(HANDLE device, int code, const TRequest& request)
{
    DWORD bytesReturned = 0;
    const BOOL result = DeviceIoControl(
        device,
        static_cast<DWORD>(code),
        const_cast<TRequest*>(&request),
        sizeof(request),
        nullptr,
        0,
        &bytesReturned,
        nullptr);
    return result && bytesReturned == 0;
}

std::string SensorName(const RADXA_SVC_SENSOR_DESCRIPTOR& descriptor)
{
    std::size_t length = 0;
    while (length < RADXA_SVC_SENSOR_NAME_LENGTH && descriptor.Name[length] != '\0')
    {
        ++length;
    }
    return std::string(descriptor.Name, descriptor.Name + length);
}

void SetPower(PowerValue& target, const RADXA_SVC_SENSOR_READING& reading)
{
    target.VoltageValid =
        (reading.ValidMask & RADXA_SVC_SENSOR_VALID_VOLTAGE) != 0;
    target.CurrentValid =
        (reading.ValidMask & RADXA_SVC_SENSOR_VALID_CURRENT) != 0;
    target.PowerValid = target.VoltageValid && target.CurrentValid;
    target.Volts = static_cast<double>(reading.VoltageMillivolts) / 1000.0;
    target.Amps = static_cast<double>(reading.CurrentMilliamps) / 1000.0;
    target.Watts = static_cast<double>(reading.PowerMicrowatts) / 1000000.0;
}
}

DeviceWorker::DeviceWorker(HWND notificationWindow) :
    notificationWindow_(notificationWindow)
{
}

DeviceWorker::~DeviceWorker()
{
    Stop();
}

void DeviceWorker::Start()
{
    if (!thread_.joinable())
    {
        thread_ = std::thread(&DeviceWorker::ThreadMain, this);
    }
}

void DeviceWorker::Stop()
{
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        stopping_ = true;
    }
    commandCondition_.notify_all();
    if (thread_.joinable())
    {
        CancelSynchronousIo(thread_.native_handle());
        thread_.join();
    }
}

void DeviceWorker::Retry()
{
    Queue({CommandKind::Retry, 0, 0});
}

void DeviceWorker::SetProfile(std::uint32_t profile)
{
    Queue({CommandKind::SetProfile, profile, 0});
}

void DeviceWorker::SetFan(std::uint32_t mode, std::uint32_t manualValue)
{
    Queue({CommandKind::SetFan, mode, manualValue});
}

DeviceSnapshot DeviceWorker::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return publishedSnapshot_;
}

void DeviceWorker::Queue(Command command)
{
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (stopping_)
        {
            return;
        }
        commands_.push_back(command);
    }
    commandCondition_.notify_one();
}

void DeviceWorker::Publish()
{
    ++snapshot_.Revision;
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        publishedSnapshot_ = snapshot_;
    }
    if (notificationWindow_ != nullptr)
    {
        PostMessageW(notificationWindow_, WM_APP_DEVICE_UPDATE, 0, 0);
    }
}

void DeviceWorker::ResetLiveValues()
{
    snapshot_.ProfileSupported = false;
    snapshot_.ProfileValid = false;
    snapshot_.FanSupported = false;
    snapshot_.FanValid = false;
    snapshot_.SensorsSupported = false;
    snapshot_.Temperatures = {};
    snapshot_.UsbPower = {};
    snapshot_.SystemPower = {};
}

bool DeviceWorker::OpenDevice(HANDLE& device)
{
    device = CreateFileW(
        RADXA_SVC_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (device != INVALID_HANDLE_VALUE)
    {
        snapshot_.Connection = ConnectionState::Connecting;
        snapshot_.Notice = UserNotice::None;
        Publish();
        return true;
    }

    const DWORD error = GetLastError();
    ResetLiveValues();
    snapshot_.Connection =
        (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ?
            ConnectionState::DriverMissing : ConnectionState::Unavailable;
    Publish();
    return false;
}

bool DeviceWorker::PollDevice(HANDLE device, bool& sensorsEnumerated)
{
    RADXA_SVC_INTERFACE_INFO interfaceInfo{};
    if (!ReadIoctl(
            device,
            IOCTL_RADXA_PLATFORM_SVC_GET_INTERFACE_INFO,
            interfaceInfo))
    {
        ResetLiveValues();
        snapshot_.Connection = ConnectionState::Unavailable;
        Publish();
        return false;
    }

    if (interfaceInfo.State == RADXA_SVC_STATE_INCOMPATIBLE_VERSION ||
        interfaceInfo.State == RADXA_SVC_STATE_MISSING_CAPABILITIES)
    {
        ResetLiveValues();
        snapshot_.Connection = ConnectionState::UpdateRequired;
        Publish();
        return true;
    }

    if (interfaceInfo.State != RADXA_SVC_STATE_READY)
    {
        ResetLiveValues();
        snapshot_.Connection =
            interfaceInfo.State == RADXA_SVC_STATE_HANDSHAKE_FAILED ?
                ConnectionState::Unavailable : ConnectionState::Connecting;
        Publish();
        return true;
    }

    snapshot_.Connection = ConnectionState::Ready;
    snapshot_.ProfileSupported =
        (interfaceInfo.Capabilities & RADXA_SVC_CAP_PROFILE) != 0;
    snapshot_.FanSupported =
        (interfaceInfo.Capabilities & RADXA_SVC_CAP_FANCTL_CTRL) != 0;
    snapshot_.SensorsSupported =
        (interfaceInfo.Capabilities & RADXA_SVC_CAP_SENSORS) != 0;
    snapshot_.Temperatures = {};

    snapshot_.ProfileValid = false;
    if (snapshot_.ProfileSupported)
    {
        RADXA_SVC_PROFILE_INFO profile{};
        if (ReadIoctl(
                device,
                IOCTL_RADXA_PLATFORM_SVC_GET_PROFILE,
                profile))
        {
            snapshot_.ProfileValid = true;
            snapshot_.EffectiveProfile = profile.EffectiveProfile;
        }
    }

    snapshot_.FanValid = false;
    if ((interfaceInfo.Capabilities & RADXA_SVC_CAP_FANCTL) != 0)
    {
        RADXA_SVC_FAN_STATE fanState{};
        if (ReadIoctl(
                device,
                IOCTL_RADXA_PLATFORM_SVC_FAN_GET_STATE,
                fanState))
        {
            snapshot_.Temperatures[0].Valid = fanState.CpuValid != 0;
            snapshot_.Temperatures[0].Celsius =
                static_cast<double>(fanState.CpuTemperatureDeciCelsius) / 10.0;
            snapshot_.Temperatures[1].Valid = fanState.GpuValid != 0;
            snapshot_.Temperatures[1].Celsius =
                static_cast<double>(fanState.GpuTemperatureDeciCelsius) / 10.0;
        }
    }
    if (snapshot_.FanSupported)
    {
        RADXA_SVC_FAN_CONTROL fan{};
        if (ReadIoctl(
                device,
                IOCTL_RADXA_PLATFORM_SVC_FAN_GET_CONTROL,
                fan))
        {
            snapshot_.FanValid = true;
            snapshot_.FanMode = fan.ControlMode;
            snapshot_.ManualFanValue = fan.ManualPwm;
        }
    }

    snapshot_.UsbPower = {};
    snapshot_.SystemPower = {};

    if (snapshot_.SensorsSupported)
    {
        RADXA_SVC_SENSOR_LIST sensorList{};
        if (ReadIoctl(
                device,
                IOCTL_RADXA_PLATFORM_SVC_ENUM_SENSORS,
                sensorList))
        {
            sensorsEnumerated = true;
            const std::uint32_t count =
                std::min<std::uint32_t>(sensorList.SensorCount, RADXA_SVC_MAX_SENSORS);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                const RADXA_SVC_SENSOR_DESCRIPTOR& descriptor = sensorList.Sensors[index];
                const std::string name = SensorName(descriptor);
                if (name != "typec_port0" && name != "typec_port1" &&
                    name != "system_power")
                {
                    continue;
                }

                RADXA_SVC_SENSOR_READ_REQUEST request{};
                request.Header.Size = sizeof(request);
                request.Header.Version = RADXA_SVC_USER_API_VERSION;
                request.SensorId = descriptor.SensorId;
                RADXA_SVC_SENSOR_READING reading{};
                if (!ReadIoctlWithRequest(
                        device,
                        IOCTL_RADXA_PLATFORM_SVC_READ_SENSOR,
                        request,
                        reading))
                {
                    continue;
                }

                if (name == "typec_port0")
                {
                    SetPower(snapshot_.UsbPower[0], reading);
                }
                else if (name == "typec_port1")
                {
                    SetPower(snapshot_.UsbPower[1], reading);
                }
                else
                {
                    SetPower(snapshot_.SystemPower, reading);
                }
            }
        }
    }

    Publish();
    return true;
}

bool DeviceWorker::ExecuteCommand(
    HANDLE device,
    const Command& command,
    bool& sensorsEnumerated)
{
    snapshot_.Busy = true;
    snapshot_.Notice = UserNotice::None;
    Publish();

    bool succeeded = false;
    if (command.Kind == CommandKind::SetProfile)
    {
        RADXA_SVC_SET_PROFILE_REQUEST request{};
        request.Header.Size = sizeof(request);
        request.Header.Version = RADXA_SVC_USER_API_VERSION;
        request.Profile = command.Value0;
        succeeded = WriteIoctl(
            device,
            IOCTL_RADXA_PLATFORM_SVC_SET_PROFILE,
            request);
    }
    else if (command.Kind == CommandKind::SetFan)
    {
        RADXA_SVC_FAN_CONTROL request{};
        request.Header.Size = sizeof(request);
        request.Header.Version = RADXA_SVC_USER_API_VERSION;
        request.ControlMode = command.Value0;
        request.ManualPwm = std::min<std::uint32_t>(
            command.Value1, RADXA_SVC_FAN_PWM_MAX);
        succeeded = WriteIoctl(
            device,
            IOCTL_RADXA_PLATFORM_SVC_FAN_SET_CONTROL,
            request);
    }

    snapshot_.Busy = false;
    snapshot_.Notice = succeeded ? UserNotice::Applied : UserNotice::ActionFailed;
    return PollDevice(device, sensorsEnumerated);
}

void DeviceWorker::ThreadMain()
{
    HANDLE device = INVALID_HANDLE_VALUE;
    bool sensorsEnumerated = false;
    auto nextPoll = std::chrono::steady_clock::now();

    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(commandMutex_);
            if (stopping_)
            {
                break;
            }
        }

        if (device == INVALID_HANDLE_VALUE)
        {
            OpenDevice(device);
            nextPoll = std::chrono::steady_clock::now() +
                (device == INVALID_HANDLE_VALUE ? kReconnectInterval : std::chrono::seconds(0));
        }

        std::deque<Command> pending;
        {
            std::unique_lock<std::mutex> lock(commandMutex_);
            commandCondition_.wait_until(lock, nextPoll, [this] {
                return stopping_ || !commands_.empty();
            });
            if (stopping_)
            {
                break;
            }
            pending.swap(commands_);
        }

        bool retry = false;
        for (const Command& command : pending)
        {
            if (command.Kind == CommandKind::Retry)
            {
                retry = true;
            }
            else if (device != INVALID_HANDLE_VALUE)
            {
                if (!ExecuteCommand(device, command, sensorsEnumerated))
                {
                    CloseHandle(device);
                    device = INVALID_HANDLE_VALUE;
                    sensorsEnumerated = false;
                    nextPoll = std::chrono::steady_clock::now() + kReconnectInterval;
                }
            }
            else
            {
                snapshot_.Notice = UserNotice::ActionFailed;
                Publish();
            }
        }

        if (retry)
        {
            if (device != INVALID_HANDLE_VALUE)
            {
                CloseHandle(device);
                device = INVALID_HANDLE_VALUE;
            }
            sensorsEnumerated = false;
            ResetLiveValues();
            snapshot_.Connection = ConnectionState::Connecting;
            snapshot_.Notice = UserNotice::None;
            Publish();
            nextPoll = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll)
        {
            if (device != INVALID_HANDLE_VALUE)
            {
                if (!PollDevice(device, sensorsEnumerated))
                {
                    CloseHandle(device);
                    device = INVALID_HANDLE_VALUE;
                    sensorsEnumerated = false;
                    nextPoll = now + kReconnectInterval;
                }
                else
                {
                    nextPoll = std::chrono::steady_clock::now() + kPollInterval;
                }
            }
            else
            {
                nextPoll = now;
            }
        }
    }

    if (device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(device);
    }
}
