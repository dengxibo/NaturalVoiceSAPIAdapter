#pragma once
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <atomic>
#include <thread>
#include <algorithm>

// Observe this probe's own audio sessions only. No system audio is recorded.
class ProcessAudioMeter {
    std::atomic<bool> stop{false};
    std::thread worker;
    float peak = 0;
public:
    void Start() {
        worker = std::thread([this] {
            HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IMMDeviceEnumerator* devices = nullptr;
            IMMDevice* device = nullptr;
            IAudioSessionManager2* sessions = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&devices));
            if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(&sessions));
            while (SUCCEEDED(hr) && !stop.load()) {
                IAudioSessionEnumerator* list = nullptr;
                if (SUCCEEDED(sessions->GetSessionEnumerator(&list))) {
                    int count = 0;
                    list->GetCount(&count);
                    for (int i = 0; i < count; ++i) {
                        IAudioSessionControl* session = nullptr;
                        IAudioSessionControl2* identity = nullptr;
                        IAudioMeterInformation* meter = nullptr;
                        DWORD pid = 0;
                        if (SUCCEEDED(list->GetSession(i, &session)) &&
                            SUCCEEDED(session->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&identity))) &&
                            SUCCEEDED(identity->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                            SUCCEEDED(session->QueryInterface(__uuidof(IAudioMeterInformation), reinterpret_cast<void**>(&meter)))) {
                            float value = 0;
                            if (SUCCEEDED(meter->GetPeakValue(&value))) peak = (std::max)(peak, value);
                        }
                        if (meter) meter->Release();
                        if (identity) identity->Release();
                        if (session) session->Release();
                    }
                    list->Release();
                }
                Sleep(15);
            }
            if (sessions) sessions->Release();
            if (device) device->Release();
            if (devices) devices->Release();
            if (SUCCEEDED(init)) CoUninitialize();
        });
    }
    float Finish() {
        stop.store(true);
        if (worker.joinable()) worker.join();
        return peak;
    }
    ~ProcessAudioMeter() { Finish(); }
};
