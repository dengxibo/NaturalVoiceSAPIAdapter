#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>
#include <atlbase.h>
#include <sphelper.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include "../HdTrace.h"
#include "../sapi/HdPrepare.h"
static void Hr(HRESULT r) { if(FAILED(r)) throw r; }
class Activation : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    std::atomic<ULONG> refs{1};
public:
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HRESULT result=E_PENDING; CComPtr<IAudioClient> client;
    ~Activation(){CloseHandle(ready);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** p) override { if(!p)return E_POINTER;*p=nullptr;if(iid==IID_IUnknown||iid==__uuidof(IActivateAudioInterfaceCompletionHandler))*p=static_cast<IActivateAudioInterfaceCompletionHandler*>(this);else if(iid==__uuidof(IAgileObject))*p=static_cast<IAgileObject*>(this);else return E_NOINTERFACE;AddRef();return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override { CComPtr<IUnknown> object; HRESULT status=E_FAIL;result=operation->GetActivateResult(&status,&object);if(SUCCEEDED(result))result=status;if(SUCCEEDED(result))result=object.QueryInterface(&client);SetEvent(ready);return S_OK; }
};
class Capture {
    CComPtr<IAudioClient> client; CComPtr<IAudioCaptureClient> capture;
    std::atomic<bool> stop{false}; std::thread worker; std::vector<float> samples;
    uint64_t base=0,last=0; bool first=true; HRESULT failure=S_OK;
public:
    void Start() {
        CComPtr<Activation> completion; completion.Attach(new Activation);
        AUDIOCLIENT_ACTIVATION_PARAMS params{};params.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId=GetCurrentProcessId();params.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT arguments{};arguments.vt=VT_BLOB;arguments.blob.cbSize=sizeof(params);arguments.blob.pBlobData=reinterpret_cast<BYTE*>(&params);
        CComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        Hr(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,__uuidof(IAudioClient),&arguments,completion,&operation));
        if(WaitForSingleObject(completion->ready,10000)!=WAIT_OBJECT_0)throw E_FAIL;Hr(completion->result);client=completion->client;
        WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};
        Hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,20000000,0,&format,nullptr));
        Hr(client->GetService(IID_PPV_ARGS(&capture)));Hr(client->Start());
        samples.reserve(48000*120);
        worker=std::thread([this] {
            HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
            try { while(!stop) {
                UINT32 count=0;Hr(capture->GetNextPacketSize(&count));
                if(!count){Sleep(2);continue;}
                BYTE* data=nullptr;DWORD flags=0;UINT64 position=0,qpc=0;Hr(capture->GetBuffer(&data,&count,&flags,&position,&qpc));
                if(first){base=position;last=position;first=false;HdTrace::Log(0,"capture_first_packet",position,qpc);}
                if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)HdTrace::Log(0,"capture_discontinuity",position,qpc);
                if(position>last && position-last<48000*60) samples.insert(samples.end(),static_cast<size_t>(position-last),0.f);
                const float* values=reinterpret_cast<const float*>(data);
                for(UINT32 i=0;i<count;++i)samples.push_back((flags&AUDCLNT_BUFFERFLAGS_SILENT)?0.f:(values[i*2]+values[i*2+1])*0.5f);
                last=position+count;Hr(capture->ReleaseBuffer(count));
            }}catch(HRESULT e){failure=e;}
            if(SUCCEEDED(init))CoUninitialize();
        });
    }
    void Finish(const std::wstring& path) {
        stop=true;if(worker.joinable())worker.join();if(client)client->Stop();Hr(failure);
        FILE* file=nullptr;_wfopen_s(&file,path.c_str(),L"wb");if(!file)throw E_FAIL;
        for(size_t i=0;i+1<samples.size();i+=2){float value=(samples[i]+samples[i+1])*0.5f;fwrite(&value,sizeof(value),1,file);}fclose(file);
        printf("CAPTURE samples24k=%zu base48k=%llu\n",samples.size()/2,base);
    }
    ~Capture(){stop=true;if(worker.joinable())worker.join();if(client)client->Stop();}
};
int wmain(int argc,wchar_t**argv) {
    setvbuf(stdout,nullptr,_IONBF,0);if(argc<3)return 2;
    Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED));int result=1;
    try {
        SetEnvironmentVariableW(L"HD_SAPI_TRACE_DIR",argv[2]);
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);HdTrace::Log(0,"qpc_frequency",frequency.QuadPart);
        CComPtr<ISpObjectToken> token;Hr(token.CoCreateInstance(CLSID_SpObjectToken));Hr(token->SetId(nullptr,argv[1],FALSE));
        CComPtr<ISpVoice> voice;Hr(voice.CoCreateInstance(CLSID_SpVoice));Hr(voice->SetVoice(token));Hr(voice->SetVolume(30));
        Hr(voice->SetNotifyWin32Event());Hr(voice->SetInterest(SPFEI_ALL_TTS_EVENTS,SPFEI_ALL_TTS_EVENTS));
        CComPtr<ISpStreamFormat> output;Hr(voice->GetOutputStream(&output));CComPtr<ISpAudio> audio;Hr(output.QueryInterface(&audio));
        SPAUDIOBUFFERINFO info{};Hr(audio->GetBufferInfo(&info));printf("BUFFER min_notify=%lu size_ms=%lu bias_ms=%lu\n",info.ulMsMinNotification,info.ulMsBufferSize,info.ulMsEventBias);
        if(argc>=4){info.ulMsBufferSize=_wtol(argv[3]);Hr(audio->SetBufferInfo(&info));}
        Capture capture;capture.Start();
        std::wstring mode=argc>=5?argv[4]:L"queue";
        if(mode==L"stop"||mode==L"jump"||mode==L"early-jump"||mode==L"rate"||mode==L"voice"){
            const wchar_t* original=L"Original paragraph has many words which must be discarded after the controller changes the reading position. Obsolete words at the end must never reach the new stream.";
            const wchar_t* obsolete=L"Obsolete queued paragraph must never be spoken.";
            const wchar_t* replacement=L"Delta restart contains different words for the new generation.";
            ULONG old=0,queued=0,fresh=0;Hr(voice->Speak(original,SPF_ASYNC|SPF_PURGEBEFORESPEAK|SPF_IS_NOT_XML,&old));
            bool acted=false,startedNew=false,done=false;size_t newWords=0,ignoredOld=0;DWORD deadline=GetTickCount()+60000;
            auto act=[&]{
                            Hr(voice->Speak(obsolete,SPF_ASYNC|SPF_IS_NOT_XML,&queued));
                            HdTrace::Log(0,"control_begin");
                            Hr(HdPurgeAndDrain(voice));
                            if(mode!=L"stop"){
                                if(mode==L"rate")Hr(voice->SetRate(4));
                                if(mode==L"voice") {std::wstring id=argv[1];auto pos=id.find(L"Xiaoxiao");if(pos==id.npos)throw E_INVALIDARG;id.replace(pos,8,L"Yunyi");CComPtr<ISpObjectToken> next;Hr(next.CoCreateInstance(CLSID_SpObjectToken));Hr(next->SetId(nullptr,id.c_str(),FALSE));Hr(voice->SetVoice(next));}
                                Hr(voice->Speak(replacement,SPF_ASYNC|SPF_IS_NOT_XML,&fresh));
                            }
                            HdTrace::Log(0,"control_return");acted=true;
            };
            if(mode==L"early-jump")act();
            while(static_cast<LONG>(deadline-GetTickCount())>0&&!done){
                SPEVENT event{};ULONG got=0;while(voice->GetEvents(1,&event,&got)==S_OK&&got){
                    HdTrace::Log(event.ulStreamNum,"client_event",event.eEventId,event.ullAudioStreamOffset);
                    if(event.eEventId==SPEI_START_INPUT_STREAM&&fresh&&event.ulStreamNum==fresh)startedNew=true;
                    if(event.eEventId==SPEI_WORD_BOUNDARY){
                        if(event.ulStreamNum==queued||(startedNew&&event.ulStreamNum!=fresh))throw E_FAIL;
                        HdTrace::Log(event.ulStreamNum,"word_detail",event.lParam,event.wParam);
                        if(acted&&!startedNew)++ignoredOld;
                        if(startedNew&&event.ulStreamNum==fresh){if(static_cast<size_t>(event.lParam)+event.wParam>wcslen(replacement))throw E_FAIL;if(!newWords&&event.lParam!=0)throw E_FAIL;++newWords;}
                        if(!acted&&event.ulStreamNum==old){
                            act();
                        }
                    }
                    SpClearEvent(&event);
                }
                if(acted&&voice->WaitUntilDone(5)==S_OK)done=true;else Sleep(2);
            }
            if(!done||!acted||(mode!=L"stop"&&newWords!=9))throw E_FAIL;
            Sleep(300);capture.Finish(std::wstring(argv[2])+L"\\capture-24k-float.raw");
            printf("PASS control old=%lu discarded=%lu new=%lu words=%zu stale_before_start=%zu\n",old,queued,fresh,newWords,ignoredOld);return 0;
        }
        bool prepared=argc>=5 && wcsstr(argv[4],L"prepare");
        long rate=argc>=5 && wcscmp(argv[4],L"prepare-rate")==0?4:0;Hr(voice->SetRate(rate));
        CComPtr<IHdSpeechPreparer> service;
        if(prepared){Hr(CoCreateInstance(CLSID_HdSpeechPreparerTest,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service)));HD_PREPARE_CAPS caps;Hr(service->GetCapabilities(&caps));Hr(service->AdvanceGeneration(1));}
        const wchar_t* a=L"Alpha paragraph explains the experiment while the reader queues a second paragraph. We measure the real audio device and the synthesis calls.";
        const wchar_t* b=L"Bravo paragraph starts with different words. This tests whether the next segment was actually prepared in advance.";
        const wchar_t* c=L"Charlie paragraph concludes our continuous speech experiment.";
        ULONG streams[3]{};HdTrace::Log(0,"queue_a");Hr(voice->Speak(a,SPF_ASYNC|SPF_PURGEBEFORESPEAK|SPF_IS_NOT_XML,&streams[0]));
        bool queuedB=false,queuedC=false;size_t ended=0;DWORD deadline=GetTickCount()+120000;
        ULONGLONG ticketB=0,ticketC=0;
        size_t words[3]{};bool endSeen[3]{};ULONG firstPosition[3]={ULONG_MAX,ULONG_MAX,ULONG_MAX};
        while(ended<3 && static_cast<LONG>(deadline-GetTickCount())>0) {
            auto queueReady=[&](ULONGLONG ticket,ULONG* stream){HD_PREPARE_STATUS status;Hr(service->GetStatus(ticket,&status));if(status.state==HD_FAILED)Hr(status.error);if(status.state!=HD_READY)return false;CComPtr<IHdPreparedAudio> lease;Hr(service->Acquire(ticket,&lease));Hr(HdQueuePrepared(voice,lease,ticket==ticketB?b:c,1,SPF_ASYNC,stream));return true;};
            if(prepared&&ticketB&&!queuedB)queuedB=queueReady(ticketB,&streams[1]);
            if(prepared&&ticketC&&!queuedC)queuedC=queueReady(ticketC,&streams[2]);
            WaitForSingleObject(voice->SpeakCompleteEvent(),5);
            SPEVENT event{};ULONG got=0;
            while(voice->GetEvents(1,&event,&got)==S_OK&&got) {
                HdTrace::Log(event.ulStreamNum,"client_event",event.eEventId,event.ullAudioStreamOffset);
                if(event.eEventId==SPEI_WORD_BOUNDARY){size_t index=event.ulStreamNum==streams[0]?0:event.ulStreamNum==streams[1]?1:2;const wchar_t* text=index==0?a:index==1?b:c;if(static_cast<size_t>(event.lParam)+event.wParam>wcslen(text)||(index&&!endSeen[index-1]))throw E_FAIL;if(!words[index])firstPosition[index]=static_cast<ULONG>(event.lParam);++words[index];}
                if(!queuedB&&!ticketB && event.eEventId==SPEI_START_INPUT_STREAM && event.ulStreamNum==streams[0]){HdTrace::Log(0,"queue_b");if(prepared)Hr(service->Prepare(b,argv[1],rate,1,&ticketB));else{Hr(voice->Speak(b,SPF_ASYNC|SPF_IS_NOT_XML,&streams[1]));queuedB=true;}}
                if(event.eEventId==SPEI_END_INPUT_STREAM){size_t index=event.ulStreamNum==streams[0]?0:event.ulStreamNum==streams[1]?1:2;if(endSeen[index])throw E_FAIL;endSeen[index]=true;++ended;if(event.ulStreamNum==streams[0]&&!queuedC){HdTrace::Log(0,"queue_c");if(prepared)Hr(service->Prepare(c,argv[1],rate,1,&ticketC));else{Hr(voice->Speak(c,SPF_ASYNC|SPF_IS_NOT_XML,&streams[2]));queuedC=true;}}}
                SpClearEvent(&event);
            }
        }
        if(ended!=3)throw E_FAIL;for(size_t i=0;i<3;++i)if(words[i]<4||firstPosition[i]!=0)throw E_FAIL;Sleep(300);capture.Finish(std::wstring(argv[2])+L"\\capture-24k-float.raw");
        printf("PASS ABC streams=%lu,%lu,%lu\n",streams[0],streams[1],streams[2]);result=0;
    }catch(HRESULT e){printf("FAIL %08lx\n",static_cast<unsigned long>(e));}
    CoUninitialize();return result;
}
