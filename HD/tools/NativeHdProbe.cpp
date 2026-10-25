#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <activation.h>
#include <cstdio>
#include <string>
#include "ProcessAudioMeter.h"
#include "../HdSpeechRuntime.h"
#include <vector>
#include "../HdPipeProtocol.h"
#include <mutex>
#include "../HdTrace.h"

static int ServeSapiEvents(wchar_t** argv) {
    bool reusable=wcscmp(argv[1],L"--sapi-events-v3")==0;
    HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    int code = 1;
    try {
        winrt::check_hresult(init);
        auto read = [](void* data, DWORD length) {
            auto bytes = static_cast<BYTE*>(data);
            while (length) {
                DWORD count = 0;
                if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, length, &count, nullptr) || !count) return false;
                bytes += count; length -= count;
            }
            return true;
        };
        auto send = [](const void* data, size_t length) {
            auto bytes = static_cast<const BYTE*>(data);
            while (length) {
                DWORD count = 0;
                winrt::check_bool(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, static_cast<DWORD>((std::min)(length, size_t(65536))), &count, nullptr));
                if (!count) winrt::throw_hresult(E_FAIL);
                bytes += count; length -= count;
            }
        };
        // Read the first request before model construction, so the parent never
        // blocks sending text while the model is loading.
        uint32_t length = 0;
        if (!read(&length, sizeof(length)) || !length || length > 8 * 1024 * 1024) winrt::throw_hresult(E_INVALIDARG);
        std::string input(length, '\0');
        if (!read(input.data(), length)) winrt::throw_hresult(E_FAIL);
        uint32_t magic = reusable?HdPipe::ReusableEventMagic:HdPipe::EventMagic; send(&magic, sizeof(magic));
        std::mutex outputMutex;
        bool active = false;
        uint64_t request=0; bool firstPcm=true; uint64_t pcmBytes=0;
        auto frame = [&](HdPipe::Type type, const void* data, size_t size, const std::string& text = {}) {
            std::lock_guard lock(outputMutex);
            if (!active) return;
            if(type==HdPipe::Type::Audio) { if(firstPcm) { HdTrace::Log(request,"native_first_pcm"); firstPcm=false; } pcmBytes+=size; }
            if (size + text.size() > HdPipe::MaximumFrame) winrt::throw_hresult(E_INVALIDARG);
            HdPipe::Header header{static_cast<uint32_t>(type), static_cast<uint32_t>(size + text.size())};
            send(&header, sizeof(header)); send(data, size); send(text.data(), text.size());
        };
        HdSpeechRuntime runtime(argv[2], argv[3], argv[4],
            [&](const BYTE* data, size_t size) { frame(HdPipe::Type::Audio, data, size); },
            [&](HdPipe::Type type, const HdPipe::Event& event, const std::string& text) { frame(type, &event, sizeof(event), text); });
        winrt::handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!done) winrt::throw_last_error();
        HANDLE stop = reinterpret_cast<HANDLE>(_wcstoui64(argv[5], nullptr, 10));
        std::atomic<bool> cancelled{false};
        std::mutex stateMutex;
        bool speaking=false;
        std::thread stopper([&] {
            HRESULT threadInit = RoInitialize(RO_INIT_MULTITHREADED);
            HANDLE events[] = {done.get(), stop};
            while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                std::lock_guard state(stateMutex);
                if(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0)continue;
                cancelled = true;
                if(speaking)try { runtime.Stop(); } catch (...) {}
                ResetEvent(stop);
            }
            if (SUCCEEDED(threadInit)) RoUninitialize();
        });
        try {
            do {
                { std::lock_guard lock(outputMutex); ++request; firstPcm=true; pcmBytes=0; active = true; }
                bool update = false;
                HdTrace::Log(request,"native_begin");
                {std::lock_guard state(stateMutex);if(WaitForSingleObject(stop,0)==WAIT_OBJECT_0)cancelled=true;speaking=!cancelled;}
                int status = cancelled ? 1 : runtime.Speak(std::wstring(winrt::to_hstring(input)), update, true);
                {std::lock_guard lock(outputMutex);HdTrace::Log(request,"native_complete",status,pcmBytes);}
                {
                std::lock_guard state(stateMutex);
                speaking=false;
                code = cancelled ? 3 : (status == 0 && !update ? 0 : 1);
                uint32_t result = static_cast<uint32_t>(code);
                frame(HdPipe::Type::Complete, &result, sizeof(result));
                { std::lock_guard lock(outputMutex); active = false; }
                cancelled=false;ResetEvent(stop);
                }
                if ((code && (code!=3||!reusable)) || !read(&length, sizeof(length)) || !length) break;
                if (length > 8 * 1024 * 1024) winrt::throw_hresult(E_INVALIDARG);
                input.resize(length);
                if (!read(input.data(), length)) winrt::throw_hresult(E_FAIL);
            } while (!cancelled);
        } catch (...) { SetEvent(done.get()); stopper.join(); throw; }
        SetEvent(done.get()); stopper.join();
    } catch (std::exception const& error) { fprintf(stderr, "HD events helper: %s\n", error.what()); }
      catch (...) { code = 1; }
    if (SUCCEEDED(init)) RoUninitialize();
    return code;
}

static void PrintHr(const wchar_t* operation, HRESULT hr)
{
    wprintf(L"%ls: 0x%08lX\n", operation, static_cast<unsigned long>(hr));
}

// Probe the installed Windows extension in a disposable process. No registration,
// injection, binary modification, or model-license substitution is performed.
int wmain(int argc, wchar_t** argv)
{
    if (argc == 6 && (wcscmp(argv[1], L"--sapi-events")==0 || wcscmp(argv[1], L"--sapi-events-v3")==0)) return ServeSapiEvents(argv);
    if ((argc == 5 || argc == 6) && wcscmp(argv[1], L"--event-test") == 0) {
        HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        int result = 1;
        try {
            winrt::check_hresult(init);
            std::atomic<size_t> audio{0}, words{0}, sentences{0}, visemes{0}, bookmarks{0};
            HdSpeechRuntime runtime(argv[2], argv[3], argv[4], [&](const BYTE*, size_t size) { audio += size; },
                [&](HdPipe::Type type, const HdPipe::Event& event, const std::string& text) {
                    if (type == HdPipe::Type::Word) ++words;
                    if (type == HdPipe::Type::Sentence) ++sentences;
                    if (type == HdPipe::Type::Viseme) ++visemes;
                    if (type == HdPipe::Type::Bookmark) ++bookmarks;
                    wprintf(L"event=%u tick=%llu dur=%llu pos=%u len=%u value=%u text=%ls\n", static_cast<unsigned>(type), event.ticks, event.duration, event.textOffset, event.textLength, event.value, winrt::to_hstring(text).c_str());
                });
            bool update = false;
            std::wstring ssml = L"<speak version=\"1.0\" xmlns=\"http://www.w3.org/2001/10/synthesis\" xml:lang=\"en-US\"><voice name=\"" + std::wstring(argv[4]) + L"\">Hello &amp; world.<bookmark mark=\"42\"/> This is sentence two. 你好世界。</voice></speak>";
            if (argc == 6) ssml = argv[5];
            int status = runtime.Speak(ssml, update, true);
            wprintf(L"SUMMARY status=%d bytes=%zu words=%zu sentences=%zu visemes=%zu bookmarks=%zu\n", status, audio.load(), words.load(), sentences.load(), visemes.load(), bookmarks.load());
            result = status == 0 && audio && words ? 0 : 1;
        } catch (std::exception const& error) { fprintf(stderr, "%s\n", error.what()); }
          catch (...) { fprintf(stderr, "Event probe failed\n"); }
        if (SUCCEEDED(init)) RoUninitialize();
        return result;
    }
    if (argc == 6 && wcscmp(argv[1], L"--sapi") == 0) {
        HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        int code = 1;
        try {
            winrt::check_hresult(init);
            std::string input;
            char buffer[4096]; DWORD count = 0;
            while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) && count) {
                input.append(buffer, count);
                if (input.size() > 8 * 1024 * 1024) winrt::throw_hresult(E_INVALIDARG);
            }
            if (input.empty()) winrt::throw_hresult(E_INVALIDARG);
            auto send = [](const void* data, size_t length) {
                auto bytes = static_cast<const BYTE*>(data);
                while (length) {
                    DWORD written = 0;
                    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, static_cast<DWORD>((std::min)(length, size_t(65536))), &written, nullptr) || !written)
                        winrt::throw_last_error();
                    bytes += written; length -= written;
                }
            };
            uint32_t magic = HdPipe::Magic;
            send(&magic, sizeof(magic));
            HdSpeechRuntime runtime(argv[2], argv[3], argv[4], [&](const BYTE* data, size_t length) {
                if (length > HdPipe::MaximumFrame) winrt::throw_hresult(E_INVALIDARG);
                uint32_t size = static_cast<uint32_t>(length);
                send(&size, sizeof(size)); send(data, length);
            });
            HANDLE stop = reinterpret_cast<HANDLE>(_wcstoui64(argv[5], nullptr, 10));
            winrt::handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!done) winrt::throw_last_error();
            std::atomic<bool> cancelled{false};
            std::thread stopper;
            if (stop) stopper = std::thread([&] {
                HRESULT threadInit = RoInitialize(RO_INIT_MULTITHREADED);
                HANDLE events[] = {done.get(), stop};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                    cancelled.store(true);
                    try { runtime.Stop(); } catch (...) {}
                }
                if (SUCCEEDED(threadInit)) RoUninitialize();
            });
            try {
                bool update = false;
                int status = runtime.Speak(std::wstring(winrt::to_hstring(input)), update, true);
                code = cancelled.load() ? 3 : (status == 0 && !update ? 0 : 1);
            } catch (...) {
                SetEvent(done.get()); if (stopper.joinable()) stopper.join(); throw;
            }
            SetEvent(done.get()); if (stopper.joinable()) stopper.join();
            uint32_t end = 0, status = static_cast<uint32_t>(code);
            send(&end, sizeof(end)); send(&status, sizeof(status));
        } catch (std::exception const& error) { fprintf(stderr, "HD SAPI helper: %s\n", error.what()); }
          catch (...) { code = 1; }
        if (SUCCEEDED(init)) RoUninitialize();
        return code;
    }
    if (argc == 6 && wcscmp(argv[1], L"--pcm-test") == 0) {
        HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        int exitCode = 1;
        try {
            winrt::check_hresult(init);
            std::vector<BYTE> audio;
            HdSpeechRuntime runtime(argv[2], argv[3], argv[4], [&](const BYTE* data, size_t length) { audio.insert(audio.end(), data, data + length); });
            ProcessAudioMeter meter;
            meter.Start();
            bool update = false;
            int status = runtime.Speak(L"这是通过 SAPI 返回音频的测试。Hello from the local HD model.", update);
            float speakerPeak = meter.Finish();
            winrt::handle output(CreateFileW(argv[5], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
            if (!output) winrt::throw_last_error();
            DWORD written = 0;
            winrt::check_bool(WriteFile(output.get(), audio.data(), static_cast<DWORD>(audio.size()), &written, nullptr));
            bool nonzero = false;
            for (BYTE sample : audio) if (sample) { nonzero = true; break; }
            wprintf(L"PCM status=%d bytes=%zu nonzero=%u speakerPeak=%.6f\n", status, audio.size(), nonzero, speakerPeak);
            exitCode = status == 0 && !update && nonzero && speakerPeak == 0 && written == audio.size() ? 0 : 1;
        } catch (winrt::hresult_error const& error) { PrintHr(L"PCM probe", error.code()); }
          catch (std::exception const& error) { fprintf(stderr, "PCM probe: %s\n", error.what()); }
        if (SUCCEEDED(init)) RoUninitialize();
        return exitCode;
    }
    if (argc == 6 && wcscmp(argv[1], L"--play") == 0) {
        HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        int exitCode = 1;
        try {
            winrt::check_hresult(init);
            std::string input;
            char block[4096]; DWORD count = 0;
            while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), block, sizeof(block), &count, nullptr) && count) input.append(block, count);
            if (input.empty()) winrt::throw_hresult(E_INVALIDARG);
            HdSpeechRuntime runtime(argv[2], argv[3], argv[4]);
            HANDLE stopEvent = reinterpret_cast<HANDLE>(_wcstoui64(argv[5], nullptr, 10));
            if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                return 3;
            }
            winrt::handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!done) winrt::throw_last_error();
            std::atomic<bool> cancelled{false};
            std::thread stopThread;
            if (stopEvent) stopThread = std::thread([&] {
                HRESULT threadInit = RoInitialize(RO_INIT_MULTITHREADED);
                HANDLE events[] = {done.get(), stopEvent};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                    cancelled.store(true);
                    try { runtime.Stop(); } catch (...) {}
                }
                if (SUCCEEDED(threadInit)) RoUninitialize();
            });
            try {
                bool update = false;
                INT32 status = runtime.Speak(std::wstring(winrt::to_hstring(input)), update);
                exitCode = status == 0 && !update ? 0 : 1;
            } catch (...) {
                SetEvent(done.get());
                if (stopThread.joinable()) stopThread.join();
                throw;
            }
            SetEvent(done.get());
            if (stopThread.joinable()) stopThread.join();
            if (cancelled.load()) exitCode = 3;
        } catch (winrt::hresult_error const& error) {
            PrintHr(L"HD helper failed", error.code());
        } catch (...) { exitCode = 1; }
        if (SUCCEEDED(init)) RoUninitialize();
        return exitCode;
    }
    if ((argc == 5 || argc == 6) && (wcscmp(argv[1], L"--bridge") == 0 || wcscmp(argv[1], L"--bridge-stop") == 0 || wcscmp(argv[1], L"--bridge-ssml") == 0)) {
        HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
        try {
            winrt::check_hresult(init);
            HdSpeechRuntime runtime(argv[2], argv[3], argv[4]);
            ProcessAudioMeter meter;
            meter.Start();
            bool update = false;
            std::thread stopper;
            const bool stopTest = wcscmp(argv[1], L"--bridge-stop") == 0;
            if (stopTest) stopper = std::thread([&runtime] {
                HRESULT threadInit = RoInitialize(RO_INIT_MULTITHREADED);
                Sleep(1000);
                try { runtime.Stop(); } catch (...) {}
                if (SUCCEEDED(threadInit)) RoUninitialize();
            });
            auto begin = GetTickCount64();
            INT32 status;
            try {
                status = runtime.Speak(stopTest ? std::wstring(1000, L'好') : (argc == 6 ? std::wstring(argv[5]) : L"这是高清本地语音测试。Hello from the local HD model."), update, wcscmp(argv[1], L"--bridge-ssml") == 0);
            } catch (...) { if (stopper.joinable()) stopper.join(); throw; }
            if (stopper.joinable()) stopper.join();
            Sleep(150);
            const float peak = meter.Finish();
            wprintf(L"Bridge status: %d; update: %u; peak: %.6f; elapsed: %llu ms\n", status, update, peak, GetTickCount64() - begin);
            RoUninitialize();
            return stopTest ? (GetTickCount64() - begin < 5000 ? 0 : 1) : (status == 0 && peak > 0 ? 0 : 1);
        } catch (winrt::hresult_error const& error) {
            PrintHr(L"Bridge failure", error.code());
            fwprintf(stderr, L"%ls\n", error.message().c_str());
            if (SUCCEEDED(init)) RoUninitialize();
            return 1;
        }
    }
    if (argc != 2 && argc != 4) {
        fwprintf(stderr, L"Usage: NativeHdProbe.exe <installed SpeechSynthesizerExtension.dll> [model-directory voice-name]\n");
        return 2;
    }
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) { PrintHr(L"RoInitialize", hr); return 1; }
    std::wstring extensionPath(argv[1]);
    std::wstring root = extensionPath.substr(0, extensionPath.find_last_of(L"\\/"));
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(root.c_str());
    AddDllDirectory((root + L"\\SpeechSynthesizer").c_str());
    HMODULE module = LoadLibraryExW(extensionPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) { PrintHr(L"LoadLibraryEx", HRESULT_FROM_WIN32(GetLastError())); RoUninitialize(); return 1; }
    auto getFactory = reinterpret_cast<HRESULT(WINAPI*)(HSTRING, IActivationFactory**)>(
        GetProcAddress(module, "DllGetActivationFactory"));
    if (!getFactory) { PrintHr(L"DllGetActivationFactory export", HRESULT_FROM_WIN32(GetLastError())); return 1; }
    const wchar_t* className = L"SpeechSynthesizerExtension.SpeechSynthesizerExtensionImpl";
    HSTRING name = nullptr;
    WindowsCreateString(className, static_cast<UINT32>(wcslen(className)), &name);
    IActivationFactory* factory = nullptr;
    hr = getFactory(name, &factory);
    WindowsDeleteString(name);
    PrintHr(L"DllGetActivationFactory", hr);
    int result = 1;
    if (SUCCEEDED(hr)) {
        IInspectable* instance = nullptr;
        if (argc == 4) {
            // ABI recovered from the matching Microsoft public PDB and x64
            // disassembly: factory slot 6 takes an HSTRING array and device ID.
            IID staticsId;
            IIDFromString(L"{B0EE6BF6-E804-5DD8-A5F1-8FA934DC7B06}", &staticsId);
            IInspectable* statics = nullptr;
            hr = factory->QueryInterface(staticsId, reinterpret_cast<void**>(&statics));
            PrintHr(L"Query factory statics", hr);
            if (SUCCEEDED(hr)) {
                HSTRING modelPath = nullptr, deviceId = nullptr;
                WindowsCreateString(argv[2], static_cast<UINT32>(wcslen(argv[2])), &modelPath);
                using Create = HRESULT(WINAPI*)(IInspectable*, UINT32, HSTRING*, HSTRING, IInspectable**);
                auto table = *reinterpret_cast<void***>(statics);
                hr = reinterpret_cast<Create>(table[6])(statics, 1, &modelPath, deviceId, &instance);
                WindowsDeleteString(modelPath);
                statics->Release();
                PrintHr(L"GetForVoicePathsAndDeviceId", hr);
            }
        } else {
            hr = factory->ActivateInstance(&instance);
        }
        PrintHr(L"ActivateInstance", hr);
        if (SUCCEEDED(hr)) {
            ULONG count = 0;
            IID* ids = nullptr;
            hr = instance->GetIids(&count, &ids);
            PrintHr(L"GetIids", hr);
            for (ULONG i = 0; SUCCEEDED(hr) && i < count; ++i) {
                wchar_t text[40];
                StringFromGUID2(ids[i], text, 40);
                wprintf(L"Interface: %ls\n", text);
            }
            CoTaskMemFree(ids);
            if (argc == 4) {
                IID synthId;
                IIDFromString(L"{9BCE424E-5D0E-5A50-B770-F246CF3F5640}", &synthId);
                IInspectable* synth = nullptr;
                hr = instance->QueryInterface(synthId, reinterpret_cast<void**>(&synth));
                PrintHr(L"Query synthesis interface", hr);
                if (SUCCEEDED(hr)) {
                    HSTRING voiceName = nullptr;
                    WindowsCreateString(argv[3], static_cast<UINT32>(wcslen(argv[3])), &voiceName);
                    using Select = HRESULT(WINAPI*)(IInspectable*, HSTRING);
                    auto table = *reinterpret_cast<void***>(synth);
                    hr = reinterpret_cast<Select>(table[6])(synth, voiceName);
                    WindowsDeleteString(voiceName);
                    PrintHr(L"SelectVoice", hr);
                    if (SUCCEEDED(hr)) {
                        IID resultInterface;
                        IIDFromString(L"{18B03E9F-B9E8-5E71-AE6F-5E652F6914D0}", &resultInterface);
                        IInspectable* synth2 = nullptr;
                        hr = synth->QueryInterface(resultInterface, reinterpret_cast<void**>(&synth2));
                        if (SUCCEEDED(hr)) {
                            HSTRING text = nullptr;
                            const wchar_t* sample = L"这是高清本地语音测试。This is a local high definition voice test.";
                            WindowsCreateString(sample, static_cast<UINT32>(wcslen(sample)), &text);
                            IInspectable* synthesisResult = nullptr;
                            ProcessAudioMeter meter;
                            meter.Start();
                            using Speak = HRESULT(WINAPI*)(IInspectable*, HSTRING, IInspectable**);
                            auto table2 = *reinterpret_cast<void***>(synth2);
                            hr = reinterpret_cast<Speak>(table2[8])(synth2, text, &synthesisResult);
                            WindowsDeleteString(text);
                            PrintHr(L"SpeakTextWithResult", hr);
                            Sleep(150);
                            wprintf(L"Own-process audio peak: %.6f\n", meter.Finish());
                            if (synthesisResult) {
                                ULONG resultCount = 0;
                                IID* resultIds = nullptr;
                                HRESULT inspectHr = synthesisResult->GetIids(&resultCount, &resultIds);
                                for (ULONG i = 0; SUCCEEDED(inspectHr) && i < resultCount; ++i) {
                                    wchar_t value[40];
                                    StringFromGUID2(resultIds[i], value, 40);
                                    wprintf(L"Result interface: %ls\n", value);
                                }
                                CoTaskMemFree(resultIds);
                                IID resultId;
                                IIDFromString(L"{76BD7B79-5AF5-57E0-9312-24ACA89C534C}", &resultId);
                                IInspectable* typedResult = nullptr;
                                if (SUCCEEDED(synthesisResult->QueryInterface(resultId, reinterpret_cast<void**>(&typedResult)))) {
                                    auto resultTable = *reinterpret_cast<void***>(typedResult);
                                    using GetStatus = HRESULT(WINAPI*)(IInspectable*, INT32*);
                                    using GetUpdate = HRESULT(WINAPI*)(IInspectable*, BYTE*);
                                    INT32 status = -1;
                                    BYTE update = 0;
                                    HRESULT statusHr = reinterpret_cast<GetStatus>(resultTable[6])(typedResult, &status);
                                    PrintHr(L"Result.get_Status", statusHr);
                                    reinterpret_cast<GetUpdate>(resultTable[7])(typedResult, &update);
                                    wprintf(L"Synthesis status (raw): %d; model update needed: %u\n", status, update);
                                    typedResult->Release();
                                }
                                synthesisResult->Release();
                            }
                            synth2->Release();
                        }
                    }
                    synth->Release();
                }
            }
            result = SUCCEEDED(hr) ? 0 : 1;
            instance->Release();
        }
        factory->Release();
    }
    // Keep the extension loaded until process exit: its async cleanup may outlive
    // the last exposed interface reference.
    RoUninitialize();
    return result;
}
