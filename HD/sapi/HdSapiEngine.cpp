#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "../HdPipeProtocol.h"
#include "../HdTrace.h"

// Independent COM engine; it adds voices without replacing any NVSA class/token.
static const CLSID EngineClass = {0xd8a46b78,0x87df,0x4dde,{0x9e,0x2a,0x46,0x8c,0xe2,0xf3,0x1d,0x8c}};
static const CLSID TestEngineClass = {0xd8a46b78,0x87df,0x4dde,{0x9e,0x2a,0x46,0x8c,0xe2,0xf3,0x1d,0x8d}};
static std::atomic<ULONG> Objects{0};
static std::atomic<ULONG> Locks{0};

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    void Reset() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
};
static void Check(BOOL ok) { if (!ok) throw HRESULT_FROM_WIN32(GetLastError()); }
static void CheckHr(HRESULT hr) { if (FAILED(hr)) throw hr; }
static std::wstring TokenString(ISpObjectToken* token, const wchar_t* name) {
    wchar_t* value = nullptr;
    CheckHr(token->GetStringValue(name, &value));
    std::wstring result(value ? value : L"");
    CoTaskMemFree(value);
    if (result.empty()) throw E_INVALIDARG;
    return result;
}
static std::wstring Quote(const std::wstring& text) {
    if (text.find(L'"') != text.npos) throw E_INVALIDARG;
    std::wstring result = L"\"" + text;
    size_t slashes = 0;
    for (size_t index = text.size(); index && text[index-1] == L'\\'; --index) ++slashes;
    return result + std::wstring(slashes, L'\\') + L"\"";
}
static std::wstring Xml(const std::wstring& text) {
    std::wstring result;
    for (auto character : text) {
        switch (character) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'"': result += L"&quot;"; break;
        default: result += character;
        }
    }
    return result;
}
static std::string Utf8(const std::wstring& text) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw E_INVALIDARG;
    std::string result(length, '\0');
    Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr));
    return result;
}

#include "HdSapiEvents.h"
#include "HdPrepareService.h"

class Engine final : public ISpTTSEngine, public ISpObjectWithToken, public ISpPhoneticAlphabetSelection {
    std::atomic<ULONG> references{1};
    ISpObjectToken* token = nullptr;
    std::wstring runtime, model, name, helper, locale;
    bool forceUps = false;
    std::unique_ptr<HdSession> session;
public:
    Engine() { ++Objects; }
    ~Engine() { session.reset(); if (token) token->Release(); --Objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id == IID_IUnknown || id == __uuidof(ISpTTSEngine)) *value = static_cast<ISpTTSEngine*>(this);
        else if (id == __uuidof(ISpObjectWithToken)) *value = static_cast<ISpObjectWithToken*>(this);
        else if (id == __uuidof(ISpPhoneticAlphabetSelection)) *value = static_cast<ISpPhoneticAlphabetSelection*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE IsAlphabetUPS(BOOL* value) override { if (!value) return E_POINTER; *value = forceUps; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetAlphabetToUPS(BOOL value) override { forceUps = value != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetObjectToken(ISpObjectToken* value) override {
        if (!value) return E_POINTER;
        if (token) return SPERR_ALREADY_INITIALIZED;
        try {
            runtime = TokenString(value, L"RuntimeRoot"); model = TokenString(value, L"ModelPath");
            name = TokenString(value, L"VoiceName"); helper = TokenString(value, L"HelperPath"); locale = TokenString(value, L"Locale");
            if (GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES)
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            value->AddRef(); token = value; return S_OK;
        } catch (HRESULT hr) { return hr; } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE GetObjectToken(ISpObjectToken** value) override {
        if (!value) return E_POINTER; *value = token;
        if (!token) return SPERR_UNINITIALIZED;
        token->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOutputFormat(const GUID*, const WAVEFORMATEX*, GUID* id, WAVEFORMATEX** format) override {
        if (!id || !format) return E_POINTER;
        *format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!*format) return E_OUTOFMEMORY;
        **format = {};
        (*format)->wFormatTag = WAVE_FORMAT_PCM; (*format)->nChannels = 1; (*format)->nSamplesPerSec = 24000;
        (*format)->wBitsPerSample = 16; (*format)->nBlockAlign = 2; (*format)->nAvgBytesPerSec = 48000;
        *id = SPDFID_WaveFormatEx; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Speak(DWORD, REFGUID formatId, const WAVEFORMATEX* format, const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site) override {
        if (!token) return SPERR_UNINITIALIZED;
        if (!site || !format) return E_POINTER;
        if (formatId != SPDFID_WaveFormatEx || format->wFormatTag != WAVE_FORMAT_PCM || format->nSamplesPerSec != 24000 || format->nChannels != 1 || format->wBitsPerSample != 16)
            return SPERR_UNSUPPORTED_FORMAT;
        try {
            if (site->GetActions() & SPVES_ABORT) return S_OK;
            auto call=++HdTrace::sequence;
            HdTrace::Log(call,"engine_begin");
            HRESULT result=HdSpeak(fragments, site, runtime, model, name, helper, locale, forceUps, session,call);
            HdTrace::Log(call,"engine_return",result); return result;
        } catch (HRESULT hr) { session.reset(); return hr; } catch (std::bad_alloc const&) { session.reset(); return E_OUTOFMEMORY; } catch (...) { session.reset(); return E_FAIL; }
    }
};

class Factory final : public IClassFactory {
    std::atomic<ULONG> references{1};
    bool preparer=false;
public:
    explicit Factory(bool prepare=false):preparer(prepare) { ++Objects; }
    ~Factory() { --Objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id != IID_IUnknown && id != IID_IClassFactory) return E_NOINTERFACE;
        *value = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID id, void** value) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            if(preparer){auto object=new HdPreparation::Service;HRESULT hr=object->QueryInterface(id,value);object->Release();return hr;}
            auto object = new(std::nothrow) Engine;
            if (!object) return E_OUTOFMEMORY;
            HRESULT hr = object->QueryInterface(id, value); object->Release(); return hr;
        }catch(HRESULT e){return e;}catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if (lock) ++Locks; else if (Locks) --Locks; return S_OK; }
};
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID id, REFIID iid, void** value) {
    if (id != EngineClass && id != TestEngineClass && id!=CLSID_HdSpeechPreparer && id!=CLSID_HdSpeechPreparerTest) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new(std::nothrow) Factory(id==CLSID_HdSpeechPreparer || id==CLSID_HdSpeechPreparerTest);
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, value); factory->Release(); return hr;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return Objects == 0 && Locks == 0 ? S_OK : S_FALSE; }
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) { if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance); return TRUE; }
