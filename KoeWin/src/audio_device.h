#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

#define WM_AUDIO_DEVICE_CHANGED (WM_APP + 30)

struct AudioInputDevice {
    std::wstring id;    // MMDevice endpoint ID
    std::wstring name;  // Friendly display name
};

class AudioDeviceManager {
public:
    AudioDeviceManager();
    ~AudioDeviceManager();

    // Enumerate all active capture endpoints, sorted by name
    std::vector<AudioInputDevice> availableInputDevices();

    // Get/set selected device endpoint ID (empty = system default)
    std::wstring selectedDeviceId();
    void setSelectedDeviceId(const wchar_t* id);

    // Returns selected ID if device still available, else empty (system default)
    std::wstring resolvedDeviceId();

    // Start/stop monitoring for device changes
    void startMonitoring(HWND messageWindow);
    void stopMonitoring();

private:
    struct IMMDeviceEnumerator* m_enumerator = nullptr;
    struct DeviceNotificationClient* m_notifyClient = nullptr;
};
