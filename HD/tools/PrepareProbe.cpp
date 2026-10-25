#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>
#include <atlbase.h>
#include <sphelper.h>
#include <string>
#include <cstdio>
#include "../sapi/HdPrepare.h"
#include "ProcessAudioMeter.h"
static void Hr(HRESULT h){if(FAILED(h))throw h;}
static void Need(bool b){if(!b)throw E_FAIL;}
static bool installed=false;
static CComPtr<IHdSpeechPreparer> Connect(){CComPtr<IHdSpeechPreparer> s;Hr(CoCreateInstance(installed?CLSID_HdSpeechPreparer:CLSID_HdSpeechPreparerTest,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&s)));return s;}
static HD_PREPARE_STATUS Ready(IHdSpeechPreparer*s,uint64_t id){DWORD deadline=GetTickCount()+45000;HD_PREPARE_STATUS v;do{Hr(s->GetStatus(id,&v));if(v.state==HD_READY)return v;if(v.state==HD_FAILED||v.state==HD_CANCELLED)throw v.error;Sleep(10);}while(static_cast<LONG>(deadline-GetTickCount())>0);throw E_FAIL;}
int wmain(int argc,wchar_t**argv){
    if(argc<2)return 2;setvbuf(stdout,nullptr,_IONBF,0);installed=argc>=3&&wcsstr(argv[2],L"installed");Hr(CoInitializeEx(nullptr,argc>=3&&wcscmp(argv[2],L"sta")==0?COINIT_APARTMENTTHREADED:COINIT_MULTITHREADED));int result=1;
    try{
        ProcessAudioMeter meter;meter.Start();
        auto service=Connect();HD_PREPARE_CAPS caps;Hr(service->GetCapabilities(&caps));Need(caps.version==1&&(caps.flags&HD_CAP_SAPI_AUDIO_EVENTS));
        HD_PREPARE_LIMITS limits;limits.maxCacheBytes=1024*1024;limits.maxEntryBytes=1024*1024;Hr(service->SetLimits(&limits));Hr(service->AdvanceGeneration(1));
        const wchar_t* text=L"Prepared speech has word events relative to this exact text.";
        ULONGLONG first=0,hit=0;Hr(service->Prepare(text,argv[1],0,1,&first));Ready(service,first);
        Need(service->Prepare(text,argv[1],0,1,&hit)==S_FALSE&&hit==first);printf("PASS async prepare and ready cache hit\n");
        CComPtr<IHdPreparedAudio> lease;Hr(service->Acquire(first,&lease));CComPtr<ISpStream> stream;Hr(lease->GetStream(&stream));
        CComPtr<IUnknown> ownerA,ownerB;Hr(lease.QueryInterface(&ownerA));Hr(stream.QueryInterface(&ownerB));Need(ownerA==ownerB);Need(lease->Validate(text,argv[1],0,1)==S_OK);Need(lease->Validate(text,argv[1],4,1)==S_FALSE);Need(lease->Validate(L"dictionary changed",argv[1],0,1)==S_FALSE);
        ULONGLONG rejected=0;Need(service->Prepare(L"Another cache entry.",argv[1],0,1,&rejected)==HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA));printf("PASS live stream lease charged against cache limit\n");
        Need(meter.Finish()==0.f);printf("PASS preparation produced no speaker audio\n");
        CComPtr<ISpVoice> playback;Hr(playback.CoCreateInstance(CLSID_SpVoice));Hr(playback->SetVolume(0));Hr(playback->SetNotifyWin32Event());Hr(playback->SetInterest(SPFEI_ALL_TTS_EVENTS,SPFEI_ALL_TTS_EVENTS));
        ULONG a=0,b=0;Hr(lease->SetPlaybackVolume(0));Hr(playback->SpeakStream(stream,SPF_ASYNC,&a));
        CComPtr<IHdPreparedAudio> again;
        // The entry may have been evicted above, but its stream remains valid.
        CComPtr<IStream> clone;Hr(stream->Clone(&clone));LARGE_INTEGER zero{};Hr(clone->Seek(zero,STREAM_SEEK_SET,nullptr));Hr(playback->SpeakStream(clone,SPF_ASYNC,&b));
        Need(playback->WaitUntilDone(45000)==S_OK);size_t words[2]{},ends[2]{};
        SPEVENT event{};ULONG count=0;
        while(playback->GetEvents(1,&event,&count)==S_OK&&count){size_t index=event.ulStreamNum==a?0:1;if(event.eEventId==SPEI_WORD_BOUNDARY){Need(size_t(event.lParam)+event.wParam<=wcslen(text));if(!words[index])Need(event.lParam==0);++words[index];}if(event.eEventId==SPEI_END_INPUT_STREAM)++ends[index];SpClearEvent(&event);}
        Need(words[0]>=4&&words[1]>=4&&ends[0]==1&&ends[1]==1);printf("PASS cached SpeakStream events: words=%zu/%zu END=%zu/%zu\n",words[0],words[1],ends[0],ends[1]);
        playback.Release();clone.Release();stream.Release();lease.Release();ownerA.Release();ownerB.Release();
        Hr(service->Prepare(L"A new entry after releasing the lease.",argv[1],0,1,&first));Ready(service,first);printf("PASS cache capacity reclaimed\n");
        {
            auto cancelled=Connect();Hr(cancelled->AdvanceGeneration(1));ULONGLONG id=0;
            Hr(cancelled->Prepare(L"Keep the ticket while this stream is queued.",argv[1],0,1,&id));Ready(cancelled,id);
            CComPtr<IHdPreparedAudio> held;Hr(cancelled->Acquire(id,&held));CComPtr<ISpStream> input;Hr(held->GetStream(&input));
            BYTE bytes[64]{};ULONG read=0;Hr(input->Read(bytes,sizeof(bytes),&read));Need(read>0);
            Hr(cancelled->Cancel(id));Need(FAILED(input->Read(bytes,sizeof(bytes),&read)));
            printf("PASS Cancel invalidates acquired stream; retain ticket until END/purge\n");
        }
        if(argc>=3&&wcscmp(argv[2],L"installed-smoke")==0){printf("PASS installed public COM service\n");return 0;}
        service.Release();service=Connect();Hr(service->AdvanceGeneration(1));
        {
            auto bounded=Connect();HD_PREPARE_LIMITS memory;memory.maxResidentBytes=65536;Hr(bounded->SetLimits(&memory));Hr(bounded->AdvanceGeneration(1));ULONGLONG id=0;Hr(bounded->Prepare(text,argv[1],0,1,&id));Ready(bounded,id);CComPtr<IHdPreparedAudio> tooLarge;Need(bounded->Acquire(id,&tooLarge)==HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA));printf("PASS resident-memory lease limit\n");
        }
        std::wstring longText;for(int i=0;i<100;++i)longText+=L"An old preparation which will be cancelled. ";
        Hr(service->Prepare(longText.c_str(),argv[1],0,1,&first));HD_PREPARE_STATUS state;
        for(int i=0;i<100;++i){Hr(service->GetStatus(first,&state));if(state.state==HD_RUNNING)break;Sleep(5);}
        DWORD started=GetTickCount();Hr(service->AdvanceGeneration(2));Need(GetTickCount()-started<100);Need(FAILED(service->Acquire(first,&again)));Need(FAILED(service->Prepare(text,argv[1],0,1,&hit)));printf("PASS generation cancellation: %lu ms\n",GetTickCount()-started);
        ULONGLONG normal=0,fast=0,other=0;Hr(service->Prepare(text,argv[1],0,2,&normal));Hr(service->Prepare(text,argv[1],4,2,&fast));Need(normal!=fast);
        std::wstring next=argv[1];auto pos=next.find(L"Xiaoxiao");Need(pos!=next.npos);next.replace(pos,8,L"Yunyi");Hr(service->Prepare(text,next.c_str(),0,2,&other));Need(other!=normal&&other!=fast);
        Ready(service,normal);Ready(service,fast);Ready(service,other);printf("PASS voice/rate/generation isolation and cancellation recovery\n");result=0;
    }catch(HRESULT h){printf("FAIL %08lx\n",static_cast<unsigned long>(h));}catch(...){printf("FAIL unexpected exception\n");}
    CoUninitialize();return result;
}
