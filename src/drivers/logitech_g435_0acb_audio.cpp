#include "logitech_g435_0acb.h"

#include <windows.h>
#include <initguid.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

namespace {

using Microsoft::WRL::ComPtr;

// MinGW only forward-declares IAudioMeterInformation. GetPeakValue is its
// first method after IUnknown in the Windows SDK interface.
struct AudioMeter : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
};
constexpr GUID kAudioMeterId{0xc02216f6, 0x8c67, 0x4b5b, {0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64}};

bool IsHeadsetName(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value{};
    const bool match = SUCCEEDED(store->GetValue(key, &value)) &&
                       value.vt == VT_LPWSTR && value.pwszVal &&
                       (wcsstr(value.pwszVal, L"G435") || wcsstr(value.pwszVal, L"g435"));
    PropVariantClear(&value);
    return match;
}

bool NoCaptureSession(IMMDevice* device) {
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<IAudioSessionEnumerator> sessions;
    int count = 0;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(manager.GetAddressOf()))) ||
        FAILED(manager->GetSessionEnumerator(sessions.GetAddressOf())) ||
        FAILED(sessions->GetCount(&count))) return false;

    // An open microphone must not be interrupted, even if the room is quiet.
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> session;
        AudioSessionState state{};
        if (FAILED(sessions->GetSession(i, session.GetAddressOf())) ||
            FAILED(session->GetState(&state)) || state == AudioSessionStateActive) return false;
    }
    return true;
}

bool EndpointQuiet(IMMDevice* device) {
    ComPtr<AudioMeter> meter;
    if (FAILED(device->Activate(kAudioMeterId, CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(meter.GetAddressOf())))) return false;
    // Browsers and chat apps can keep silent render streams open indefinitely.
    // Check several meter periods before treating playback as idle.
    for (int i = 0; i < 5; ++i) {
        if (i) Sleep(50);
        float peak = 0;
        if (FAILED(meter->GetPeakValue(&peak)) || peak > 0.001f) return false;
    }
    return true;
}

bool CheckEndpoints() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> endpoints;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(enumerator.GetAddressOf()))) ||
        FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED,
                                             endpoints.GetAddressOf())) ||
        FAILED(endpoints->GetCount(&count))) return false;

    bool renderFound = false, captureFound = false;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        if (FAILED(endpoints->Item(i, device.GetAddressOf())) ||
            FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) return false;
        if (!IsHeadsetName(properties.Get(), PKEY_DeviceInterface_FriendlyName) &&
            !IsHeadsetName(properties.Get(), PKEY_Device_FriendlyName)) continue;

        ComPtr<IMMEndpoint> endpoint;
        EDataFlow flow = eAll;
        DWORD state = 0;
        if (FAILED(device.As(&endpoint)) || FAILED(endpoint->GetDataFlow(&flow)) ||
            FAILED(device->GetState(&state)) ||
            !(state & (DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED))) return false;
        renderFound |= flow == eRender;
        captureFound |= flow == eCapture;
        if (!(state & DEVICE_STATE_ACTIVE)) continue;
        if (flow == eCapture && !NoCaptureSession(device.Get())) return false;
        if (!EndpointQuiet(device.Get())) return false;
    }
    return renderFound && captureFound;
}

}  // namespace

bool G435Acb::AudioIdle() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    const bool idle = CheckEndpoints();
    if (SUCCEEDED(initialized)) CoUninitialize();
    return idle;
}
