#include "audio_device.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cstdio>

static const wchar_t* kRegKeyPath = L"SOFTWARE\\Koe";
static const wchar_t* kRegValueName = L"SelectedAudioDeviceId";

// ── IMMNotificationClient implementation ────────────────

struct DeviceNotificationClient : public IMMNotificationClient {
    HWND m_hwnd = nullptr;
    LONG m_refCount = 1;

    explicit DeviceNotificationClient(HWND hwnd) : m_hwnd(hwnd) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient — only react to device removal/disabling
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD dwNewState) override {
        // Only notify when a device becomes unavailable (unplugged, disabled, not present)
        if (dwNewState != DEVICE_STATE_ACTIVE) {
            PostMessageW(m_hwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        PostMessageW(m_hwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }
};

// ── AudioDeviceManager ──────────────────────────────────

AudioDeviceManager::AudioDeviceManager() {}

AudioDeviceManager::~AudioDeviceManager() {
    stopMonitoring();
}

void AudioDeviceManager::startMonitoring(HWND messageWindow) {
    if (m_enumerator) return;  // already monitoring

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&m_enumerator));
    if (FAILED(hr)) {
        m_enumerator = nullptr;
        return;
    }

    m_notifyClient = new DeviceNotificationClient(messageWindow);
    m_enumerator->RegisterEndpointNotificationCallback(m_notifyClient);
    OutputDebugStringA("[Koe] Audio device monitoring started\n");
}

void AudioDeviceManager::stopMonitoring() {
    if (m_enumerator && m_notifyClient) {
        m_enumerator->UnregisterEndpointNotificationCallback(m_notifyClient);
    }
    if (m_notifyClient) {
        m_notifyClient->Release();
        m_notifyClient = nullptr;
    }
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

std::vector<AudioInputDevice> AudioDeviceManager::availableInputDevices() {
    std::vector<AudioInputDevice> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) continue;

        // Get endpoint ID
        LPWSTR deviceId = nullptr;
        if (FAILED(device->GetId(&deviceId))) {
            device->Release();
            continue;
        }

        // Get friendly name
        IPropertyStore* props = nullptr;
        std::wstring friendlyName;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    friendlyName = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            props->Release();
        }

        if (!friendlyName.empty()) {
            devices.push_back({ deviceId, friendlyName });
        }

        CoTaskMemFree(deviceId);
        device->Release();
    }

    collection->Release();
    enumerator->Release();

    // Sort by name
    std::sort(devices.begin(), devices.end(),
              [](const AudioInputDevice& a, const AudioInputDevice& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });

    return devices;
}

std::wstring AudioDeviceManager::selectedDeviceId() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return L"";
    }

    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(hKey, kRegValueName, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(hKey);

    if (status == ERROR_SUCCESS && type == REG_SZ) {
        return buf;
    }
    return L"";
}

void AudioDeviceManager::setSelectedDeviceId(const wchar_t* id) {
    HKEY hKey;
    if (id && wcslen(id) > 0) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr,
                            0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, kRegValueName, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(id),
                           static_cast<DWORD>((wcslen(id) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    } else {
        // Delete = revert to system default
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, kRegValueName);
            RegCloseKey(hKey);
        }
    }
}

std::wstring AudioDeviceManager::resolvedDeviceId() {
    std::wstring saved = selectedDeviceId();
    if (saved.empty()) return L"";

    // Check if saved device still exists
    auto devices = availableInputDevices();
    for (const auto& dev : devices) {
        if (dev.id == saved) return saved;
    }

    OutputDebugStringA("[Koe] Saved audio device not found, falling back to system default\n");
    return L"";
}
