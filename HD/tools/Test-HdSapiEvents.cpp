#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <atlbase.h>
#include <sphelper.h>
#include <vector>
#include <string>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <tlhelp32.h>

static void Hr(HRESULT result) { if (FAILED(result)) throw result; }
static void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Event { SPEVENT value; std::wstring bookmark; };
struct Site final : ISpTTSEngineSite {
    ULONG refs = 1;
    uint64_t bytes = 0;
    bool nonzero = false, acted = false, pending = false;
    bool pace = false;
    ULONGLONG started = GetTickCount64(), firstAudio = 0, lastWrite = 0, maxGap = 0;
    long rate = 0, skip = 0, completed = LONG_MIN;
    size_t triggerWords = SIZE_MAX, triggerSentences = SIZE_MAX;
    enum Mode { Plain, Skip, Rate, Abort } mode = Plain;
    std::vector<Event> events;
    size_t Count(SPEVENTENUM kind) const { return std::count_if(events.begin(),events.end(),[&](const Event& event) { return event.value.eEventId == kind; }); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(ISpTTSEngineSite) || iid == __uuidof(ISpEventSink)) *value = static_cast<ISpTTSEngineSite*>(this); else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE AddEvents(const SPEVENT* input, ULONG count) override {
        for (ULONG i=0; i<count; ++i) {
            Require(input[i].ullAudioStreamOffset == bytes, "event was not delivered before its exact audio position");
            Event event{input[i],{}};
            if (input[i].elParamType == SPET_LPARAM_IS_STRING) event.bookmark = reinterpret_cast<const wchar_t*>(input[i].lParam);
            events.push_back(std::move(event));
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEventInterest(ULONGLONG* interest) override { *interest = SPFEI_ALL_TTS_EVENTS; return S_OK; }
    DWORD STDMETHODCALLTYPE GetActions() override {
        if (!acted && (Count(SPEI_WORD_BOUNDARY)>=triggerWords || Count(SPEI_SENTENCE_BOUNDARY)>=triggerSentences)) pending = true;
        if (!pending) return 0;
        return mode == Skip ? SPVES_SKIP : mode == Rate ? SPVES_RATE : mode == Abort ? SPVES_ABORT : 0;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG count, ULONG* written) override {
        ULONGLONG now = GetTickCount64();
        if (!firstAudio) firstAudio = now-started;
        if (lastWrite) maxGap = (std::max)(maxGap,now-lastWrite);
        auto samples = static_cast<const short*>(data);
        for (ULONG i=0; i<count/2; ++i) if (samples[i]) nonzero = true;
        bytes += count; *written = count;
        if (pace) Sleep(count/48);
        lastWrite = GetTickCount64(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRate(long* value) override { if (pending && mode == Rate) { rate = 5; acted = true; pending = false; } *value = rate; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVolume(USHORT* value) override { *value = 100; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSkipInfo(SPVSKIPTYPE* type, long* count) override { *type = SPVST_SENTENCE; *count = skip; return S_OK; }
    HRESULT STDMETHODCALLTYPE CompleteSkip(long count) override { completed = count; acted = true; pending = false; return S_OK; }
};
static SPVTEXTFRAG Fragment(const std::wstring& text, ULONG offset = 0) {
    SPVTEXTFRAG fragment{}; fragment.State.eAction = SPVA_Speak; fragment.State.Volume=100; fragment.State.LangID=0x409;
    fragment.pTextStart=text.c_str(); fragment.ulTextLen=static_cast<ULONG>(text.size()); fragment.ulTextSrcOffset=offset; return fragment;
}
static CComPtr<ISpTTSEngine> Engine(HMODULE module, ISpObjectToken* token) {
    auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
    Require(get!=nullptr,"missing COM export");
    const CLSID clsid={0xd8a46b78,0x87df,0x4dde,{0x9e,0x2a,0x46,0x8c,0xe2,0xf3,0x1d,0x8d}};
    CComPtr<IClassFactory> factory; Hr(get(clsid,IID_PPV_ARGS(&factory)));
    CComPtr<ISpTTSEngine> engine; Hr(factory->CreateInstance(nullptr,IID_PPV_ARGS(&engine)));
    CComPtr<ISpObjectWithToken> owner; Hr(engine.QueryInterface(&owner)); Hr(owner->SetObjectToken(token)); return engine;
}
static void Run(HMODULE module, ISpObjectToken* token, SPVTEXTFRAG& fragments, Site& site, bool ups=false) {
    auto engine = Engine(module,token); GUID formatId{}; WAVEFORMATEX* wave=nullptr;
    if (ups) { CComPtr<ISpPhoneticAlphabetSelection> selection; Hr(engine.QueryInterface(&selection)); Hr(selection->SetAlphabetToUPS(TRUE)); }
    Hr(engine->GetOutputFormat(nullptr,nullptr,&formatId,&wave));
    HRESULT result=engine->Speak(0,formatId,wave,&fragments,&site); CoTaskMemFree(wave); Hr(result);
}
static int FirstViseme(const Site& site) {
    for (auto& event : site.events) if (event.value.eEventId==SPEI_VISEME && LOWORD(event.value.lParam)) return LOWORD(event.value.lParam);
    return -1;
}
int wmain(int argc,wchar_t** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if (argc<4) return 2;
    Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
    int result=1;
    try {
        CComPtr<ISpObjectToken> token; Hr(token.CoCreateInstance(CLSID_SpObjectToken)); Hr(token->SetId(nullptr,argv[2],FALSE));
        HMODULE module=LoadLibraryW(argv[1]); Require(module!=nullptr,"engine DLL load failed");
        if (argc>=5 && wcscmp(argv[4],L"--short-repeat")==0) {
            auto engine=Engine(module,token); CComPtr<ISpPhoneticAlphabetSelection> selection; Hr(engine.QueryInterface(&selection)); Hr(selection->SetAlphabetToUPS(TRUE));
            GUID id{}; WAVEFORMATEX* wave=nullptr; Hr(engine->GetOutputFormat(nullptr,nullptr,&id,&wave));
            CComPtr<ISpPhoneConverter> converter; Hr(SpCreatePhoneConverter(0,nullptr,nullptr,&converter));
            SPPHONEID phones[SP_MAX_PRON_LENGTH]{}; Hr(converter->PhoneToId(L"H EH L O",phones));
            std::wstring empty; auto fragment=Fragment(empty); fragment.pTextStart=nullptr; fragment.State.eAction=SPVA_Pronounce; fragment.State.LangID=0x804; fragment.State.pPhoneIds=phones;
            for (int pass=0;pass<6;++pass) { Site site; Hr(engine->Speak(0,id,wave,&fragment,&site)); Require(site.nonzero && site.Count(SPEI_WORD_BOUNDARY)==0 && site.Count(SPEI_VISEME)>=4 && FirstViseme(site)==12,"short utterance callbacks were truncated"); printf("PASS short repeat %d: bytes=%llu visemes=%zu\n",pass,site.bytes,site.Count(SPEI_VISEME)); }
            CoTaskMemFree(wave); return 0;
        }
        if (argc>=5 && wcscmp(argv[4],L"--empty-native")==0) {
            auto engine=Engine(module,token); GUID id{}; WAVEFORMATEX* wave=nullptr; Hr(engine->GetOutputFormat(nullptr,nullptr,&id,&wave));
            std::wstring unsupported=L"你好世界。"; auto fragment=Fragment(unsupported); Site silent;
            HRESULT empty=engine->Speak(0,id,wave,&fragment,&silent);
            Require(FAILED(empty) && !silent.bytes,"empty native result was accepted as successful speech");
            std::wstring supported=L"This is English speech after a rejected empty result."; fragment=Fragment(supported); Site restored;
            Hr(engine->Speak(0,id,wave,&fragment,&restored)); CoTaskMemFree(wave);
            Require(restored.nonzero,"engine did not recover after an empty native result"); printf("PASS empty native result rejected; next request recovered\n"); return 0;
        }
        if (argc>=5 && wcscmp(argv[4],L"--latency")==0) {
            auto engine=Engine(module,token); GUID id{}; WAVEFORMATEX* wave=nullptr; Hr(engine->GetOutputFormat(nullptr,nullptr,&id,&wave));
            std::wstring text=L"This first sentence contains enough words to play for several seconds while the following sentence is prepared in the background. This is the next sentence. This is the last sentence.";
            for (int pass=0;pass<2;++pass) {
                auto fragment=Fragment(text); Site site; site.pace=true;
                Hr(engine->Speak(0,id,wave,&fragment,&site));
                DWORD host=0; HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0); PROCESSENTRY32W entry{sizeof(entry)};
                if(snapshot!=INVALID_HANDLE_VALUE) { if(Process32FirstW(snapshot,&entry)) do {if(entry.th32ParentProcessID==GetCurrentProcessId() && (_wcsicmp(entry.szExeFile,L"HdSpeechHost.exe")==0 || _wcsicmp(entry.szExeFile,L"NativeHdProbe.exe")==0)) host=entry.th32ProcessID;}while(Process32NextW(snapshot,&entry)); CloseHandle(snapshot); }
                printf("LATENCY pass=%d first_audio_ms=%llu max_between_writes_ms=%llu retained_host_pid=%lu bytes=%llu\n",pass,site.firstAudio,site.maxGap,host,site.bytes);
                text=L"Another first sentence contains enough words to play for several seconds while the following sentence is prepared in the background. This is a different next sentence. This is another last sentence.";
            }
            CoTaskMemFree(wave); return 0;
        }
        std::wstring tokenId(argv[2]);
        bool chinese = tokenId.find(L"Xiaoxiao") != tokenId.npos || tokenId.find(L"Yunyi") != tokenId.npos;
        bool liveOnly = argc >= 5 && wcscmp(argv[4],L"--live-only") == 0; bool smoke = liveOnly || (argc >= 5 && wcscmp(argv[4],L"--smoke") == 0);
        std::wstring text=chinese ? L"Hello & 世界。第二句话 test 123。Third sentence." : L"Hello & world. This is sentence two. Third sentence.";
        auto fragment=Fragment(text);
        Site plain; Run(module,token,fragment,plain);
        Require(plain.nonzero && plain.Count(SPEI_WORD_BOUNDARY)>=6 && plain.Count(SPEI_SENTENCE_BOUNDARY)==3 && plain.Count(SPEI_VISEME)>5,"missing audio, words, sentences or visemes");
        bool amp=false;
        for (const auto& event:plain.events) if (event.value.eEventId==SPEI_WORD_BOUNDARY) {
            size_t start=event.value.lParam, length=event.value.wParam;
            Require(length && start+length<=text.size(),"word source position is invalid");
            if (text.substr(start,length)==L"&") amp=true;
        }
        Require(amp,"XML entity did not map back to one original character");
        printf("PASS mixed text: words=%zu sentences=%zu visemes=%zu pcm=%llu\n",plain.Count(SPEI_WORD_BOUNDARY),plain.Count(SPEI_SENTENCE_BOUNDARY),plain.Count(SPEI_VISEME),plain.bytes);
        if (!smoke) {
        Site forward; forward.mode=Site::Skip; forward.skip=1; forward.triggerWords=2; Run(module,token,fragment,forward);
        Require(forward.completed==1 && forward.Count(SPEI_SENTENCE_BOUNDARY)==3,"forward sentence skip failed"); printf("PASS forward sentence skip\n");
        Site backward; backward.mode=Site::Skip; backward.skip=-1; backward.triggerSentences=2; Run(module,token,fragment,backward);
        Require(backward.completed==-1 && backward.Count(SPEI_SENTENCE_BOUNDARY)==5,"backward sentence skip failed"); printf("PASS backward sentence skip\n");
        Site restart; restart.mode=Site::Skip; restart.skip=0; restart.triggerWords=2; Run(module,token,fragment,restart);
        Require(restart.completed==0 && restart.Count(SPEI_SENTENCE_BOUNDARY)==4,"current sentence restart failed"); printf("PASS restart current sentence\n");
        Site end; end.mode=Site::Skip; end.skip=99; end.triggerWords=2; Run(module,token,fragment,end);
        Require(end.completed==3 && end.Count(SPEI_SENTENCE_BOUNDARY)==1,"out-of-range skip did not report actual count and stop"); printf("PASS out-of-range sentence skip\n");
        Site rate; rate.mode=Site::Rate; rate.triggerWords=2; Run(module,token,fragment,rate);
        Require(rate.acted && rate.nonzero && rate.Count(SPEI_SENTENCE_BOUNDARY)==3 && rate.bytes<plain.bytes,"live rate change failed"); printf("PASS live rate change\n");
        auto muted=fragment; muted.State.Volume=0; Site fragmentVolume; Run(module,token,muted,fragmentVolume);
        Require(fragmentVolume.bytes && !fragmentVolume.nonzero,"fragment volume zero did not mute audio"); printf("PASS fragment volume\n");
        Site abort; abort.mode=Site::Abort; abort.triggerWords=2; Run(module,token,fragment,abort);
        Require(abort.Count(SPEI_WORD_BOUNDARY)==2 && abort.bytes<plain.bytes,"abort failed"); printf("PASS abort\n");
        std::wstring first=L"Hello ", marker=L"42", last=L"world.";
        auto f1=Fragment(first), bookmark=Fragment(marker), f2=Fragment(last,6);
        bookmark.State.eAction=SPVA_Bookmark; f1.pNext=&bookmark; bookmark.pNext=&f2;
        Site marked; Run(module,token,f1,marked);
        Require(marked.Count(SPEI_TTS_BOOKMARK)==1,"bookmark missing or duplicated");
        for(auto& event:marked.events) if(event.value.eEventId==SPEI_TTS_BOOKMARK) Require(event.bookmark==marker && event.value.wParam==42,"bookmark payload mismatch");
        printf("PASS bookmark and deduplication\n");
        std::wstring pronunciation=L"test"; auto normal=Fragment(pronunciation), pronounce=Fragment(pronunciation);
        CComPtr<ISpPhoneConverter> converter; Hr(SpCreatePhoneConverter(0x409,nullptr,nullptr,&converter));
        SPPHONEID phones[SP_MAX_PRON_LENGTH]{}; Hr(converter->PhoneToId(L"h eh l ow",phones));
        pronounce.State.eAction=SPVA_Pronounce; pronounce.State.pPhoneIds=phones;
        Site normalSite, pronSite; Run(module,token,normal,normalSite); Run(module,token,pronounce,pronSite);
        Require(pronSite.nonzero && FirstViseme(pronSite)!=FirstViseme(normalSite),"phoneme override did not change the actual first articulated sound");
        printf("PASS phoneme override: normalViseme=%d overriddenViseme=%d\n",FirstViseme(normalSite),FirstViseme(pronSite));
        CComPtr<ISpPhoneConverter> universal; Hr(SpCreatePhoneConverter(0,nullptr,nullptr,&universal));
        SPPHONEID ups[SP_MAX_PRON_LENGTH]{}; Hr(universal->PhoneToId(L"H EH L O",ups));
        pronounce.State.LangID=0x804; pronounce.State.pPhoneIds=ups;
        Site upsSite; Run(module,token,pronounce,upsSite,true);
        Require(upsSite.nonzero && FirstViseme(upsSite)==12,"Universal Phone Set override failed"); printf("PASS universal phoneme alphabet\n");
        auto standalone=pronounce; standalone.pTextStart=nullptr; standalone.ulTextLen=0;
        Site insertion; Run(module,token,standalone,insertion,true);
        printf("CHECK standalone: nonzero=%d words=%zu viseme=%d bytes=%llu\n",insertion.nonzero,insertion.Count(SPEI_WORD_BOUNDARY),FirstViseme(insertion),insertion.bytes);
        Require(insertion.nonzero && insertion.Count(SPEI_WORD_BOUNDARY)==0 && FirstViseme(insertion)==12,"standalone pronunciation insertion failed"); printf("PASS standalone pronunciation insertion\n");
        auto silence=Fragment(L""); silence.State.eAction=SPVA_Silence; silence.State.SilenceMSecs=800;
        Site silent; Run(module,token,silence,silent);
        Require(!silent.nonzero && silent.bytes==38400,"explicit 800ms silence was not preserved"); printf("PASS explicit silence\n");
        auto bookmarkOnly=Fragment(marker); bookmarkOnly.State.eAction=SPVA_Bookmark;
        Site only; Run(module,token,bookmarkOnly,only); Require(only.Count(SPEI_TTS_BOOKMARK)==1,"bookmark-only request failed"); printf("PASS bookmark-only request\n");
        }
        // Real SAPI output/event routing: this exercises SAPI's own queue and
        // event copies rather than only the engine-site contract above.
        CComPtr<ISpVoice> voice; Hr(voice.CoCreateInstance(CLSID_SpVoice)); Hr(voice->SetVoice(token));
        CComPtr<ISpStream> stream; Hr(stream.CoCreateInstance(CLSID_SpStream));
        CSpStreamFormat format; Hr(format.AssignFormat(SPSF_24kHz16BitMono));
        Hr(stream->BindToFile(argv[3],SPFM_CREATE_ALWAYS,&format.FormatId(),format.WaveFormatExPtr(),SPFEI_ALL_TTS_EVENTS));
        Hr(voice->SetOutput(stream,TRUE)); Hr(voice->SetNotifyWin32Event()); Hr(voice->SetInterest(SPFEI_ALL_TTS_EVENTS,SPFEI_ALL_TTS_EVENTS));
        Hr(voice->Speak(text.c_str(),SPF_ASYNC,nullptr)); Require(voice->WaitUntilDone(60000)==S_OK,"WAV synthesis timed out");
        // SAPI embeds timing in a file; it dispatches real-time notifications
        // during device playback. Keep this integration test inaudible.
        Hr(voice->SetOutput(nullptr,TRUE)); Hr(stream->Close()); Hr(voice->SetVolume(0));
        SPEVENT stale{}; ULONG staleCount=0;
        while (voice->GetEvents(1,&stale,&staleCount)==S_OK && staleCount) SpClearEvent(&stale);
        Hr(voice->Speak(text.c_str(),SPF_ASYNC,nullptr)); Require(voice->WaitUntilDone(60000)==S_OK,"audio/event playback timed out");
        size_t words=0,sentences=0,visemes=0; SPEVENT event{}; ULONG count=0;
        while (voice->GetEvents(1,&event,&count)==S_OK && count) {
            if(event.eEventId==SPEI_WORD_BOUNDARY) { ++words; Require(size_t(event.lParam)+event.wParam<=text.size(),"SAPI word event offset mismatch"); }
            if(event.eEventId==SPEI_SENTENCE_BOUNDARY) ++sentences;
            if(event.eEventId==SPEI_VISEME) ++visemes;
            SpClearEvent(&event);
        }
        printf("CHECK real SAPI: words=%zu sentences=%zu visemes=%zu\n",words,sentences,visemes);
        Require(words>=6 && sentences==3 && visemes>5,"real SAPI event delivery failed");
        printf("PASS real SAPI event queue: words=%zu sentences=%zu visemes=%zu\n",words,sentences,visemes);
        std::wstring markup=L"<sapi>Hello &amp; world.<bookmark mark=\"42\"/><spell>ABC</spell><silence msec=\"300\"/><pron sym=\"";
        markup += chinese ? L"ni 3 hao 3" : L"h eh l ow"; markup += L"\">test</pron></sapi>";
        Hr(voice->Speak(markup.c_str(),SPF_ASYNC|SPF_IS_XML,nullptr)); Require(voice->WaitUntilDone(60000)==S_OK,"XML playback timed out");
        size_t bookmarks=0,xmlWords=0;
        while (voice->GetEvents(1,&event,&count)==S_OK && count) {
            if(event.eEventId==SPEI_WORD_BOUNDARY) { ++xmlWords; Require(size_t(event.lParam)+event.wParam<=markup.size(),"XML source offset mismatch"); }
            if(event.eEventId==SPEI_TTS_BOOKMARK) { ++bookmarks; Require(event.wParam==42 && std::wstring(reinterpret_cast<const wchar_t*>(event.lParam))==L"42","real SAPI bookmark copy failed"); }
            SpClearEvent(&event);
        }
        Require(bookmarks==1 && xmlWords>=4,"real SAPI XML fragments or bookmark failed");
        printf("PASS real SAPI XML: words=%zu bookmarks=%zu\n",xmlWords,bookmarks);
        if (!smoke || liveOnly) {       std::wstring longText=L"First sentence has many words to make a live skip test reliable while this sentence is still playing slowly enough for the client to react. Second sentence. Third sentence.";
            Hr(voice->Speak(longText.c_str(),SPF_ASYNC,nullptr));
            bool firstWord=false; DWORD deadline=GetTickCount()+20000;
            while(!firstWord && static_cast<LONG>(deadline-GetTickCount())>0) {
                while(voice->GetEvents(1,&event,&count)==S_OK && count) { if(event.eEventId==SPEI_WORD_BOUNDARY) firstWord=true; SpClearEvent(&event); }
                if(!firstWord) Sleep(10);
            }
            if (!firstWord) { SPVOICESTATUS status{}; Hr(voice->GetStatus(&status,nullptr)); printf("DIAGNOSTIC running=%lu last_hresult=%08lx position=%lu\n",status.dwRunningState,static_cast<unsigned long>(status.hrLastResult),status.ulInputWordPos); }
            Require(firstWord,"real-time word notification did not arrive during playback");
            ULONG actual=0; Hr(voice->Skip(L"SENTENCE",1,&actual)); Require(actual==1,"ISpVoice::Skip did not report one skipped sentence");
            Require(voice->WaitUntilDone(60000)==S_OK,"real sentence skip timed out");
            bool secondSentence=false;
            while(voice->GetEvents(1,&event,&count)==S_OK && count) { if(event.eEventId==SPEI_SENTENCE_BOUNDARY && event.lParam==static_cast<LPARAM>(longText.find(L"Second"))) secondSentence=true; SpClearEvent(&event); }
            Require(secondSentence,"real skip did not continue at the second sentence"); printf("PASS real SAPI Skip and live word notification\n");
            Hr(voice->Speak(longText.c_str(),SPF_ASYNC,nullptr)); Sleep(100);
            DWORD started=GetTickCount(); Hr(voice->Speak(nullptr,SPF_ASYNC|SPF_PURGEBEFORESPEAK,nullptr));
            Require(voice->WaitUntilDone(5000)==S_OK && GetTickCount()-started<3000,"purge during model startup did not stop promptly");
            printf("PASS startup purge: %lu ms\n",GetTickCount()-started);
        }
        printf("ALL EVENT/CONTROL TESTS PASSED (%zu-bit)\n",sizeof(void*)*8); result=0;
    } catch(HRESULT error) { printf("FAIL HRESULT=0x%08lx\n",static_cast<unsigned long>(error)); }
      catch(std::exception const& error) { printf("FAIL %s\n",error.what()); }
    CoUninitialize(); return result;
}
