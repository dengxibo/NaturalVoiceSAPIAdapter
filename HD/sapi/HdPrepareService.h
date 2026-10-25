#pragma once
#include "HdPrepare.h"
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <cwctype>
namespace HdPreparation {
struct Ledger { std::mutex lock; uint64_t bytes=0,text=0,resident=0,residentLimit=64ull*1024*1024; };
struct Resident {
    std::shared_ptr<Ledger> ledger;uint64_t charge;
    Resident(std::shared_ptr<Ledger> l,uint64_t n):ledger(std::move(l)),charge(n){std::lock_guard guard(ledger->lock);if(n>ledger->residentLimit-ledger->resident)throw HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);ledger->resident+=n;}
    ~Resident(){std::lock_guard guard(ledger->lock);ledger->resident-=charge;}
};
struct Job {
    uint64_t id=0,generation=0,reservation=0,hits=0;
    long rate=0; std::wstring text,voice,path;
    ULONG state=HD_QUEUED; HRESULT error=E_PENDING;
    std::atomic<bool> cancelled{false}; std::shared_ptr<Ledger> ledger;
    HD_PREPARE_STATUS readyInfo;
    uint64_t residentCharge=0;
    ~Job(){ if(!path.empty())DeleteFileW(path.c_str()); if(ledger){std::lock_guard guard(ledger->lock);ledger->bytes-=reservation;ledger->text-=text.size();ledger->resident-=residentCharge;} }
    void Status(HD_PREPARE_STATUS* value) const { value->state=state;value->error=error;value->rate=rate;value->generation=generation;value->bytes=state==HD_READY?reservation:0;value->cacheHits=hits; }
};
// Keeps the cache lease alive even when callers obtain the base IStream.
class BaseStream : public IStream {
    std::atomic<ULONG> refs{1}; CComPtr<IStream> inner; std::shared_ptr<Job> job;
    Resident resident;
public:
    BaseStream(IStream* stream,std::shared_ptr<Job> owner):inner(stream),job(owner),resident(owner->ledger,owner->reservation*2+65536){++Objects;}
    ~BaseStream(){inner.Release();job.reset();--Objects;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i,void**p) override{if(!p)return E_POINTER;*p=nullptr;if(i!=IID_IUnknown&&i!=IID_IStream&&i!=IID_ISequentialStream)return E_NOINTERFACE;*p=static_cast<IStream*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;} ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Read(void*p,ULONG n,ULONG*r) override{if(job->cancelled)return E_ABORT;return inner->Read(p,n,r);}
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER a,DWORD b,ULARGE_INTEGER*c) override{return inner->Seek(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*p,ULARGE_INTEGER n,ULARGE_INTEGER*a,ULARGE_INTEGER*b) override{return inner->CopyTo(p,n,a,b);}
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;} HRESULT STDMETHODCALLTYPE Revert() override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return inner->LockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return inner->UnlockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG*p,DWORD f) override{return inner->Stat(p,f);}
    HRESULT STDMETHODCALLTYPE Clone(IStream**p) override{if(!p)return E_POINTER;*p=nullptr;try{CComPtr<IStream> copy;HRESULT hr=inner->Clone(&copy);if(FAILED(hr))return hr;*p=new BaseStream(copy,job);return S_OK;}catch(HRESULT e){return e;}catch(...){return E_OUTOFMEMORY;}}
};
class Audio : public IHdPreparedAudio,public ISpStream,public ISpEventSource {
    std::atomic<ULONG> refs{1}; std::shared_ptr<Job> job; CComPtr<ISpStream> stream;CComPtr<ISpEventSource> events;CComPtr<IUnknown> marshaler;HD_PREPARE_STATUS snapshot;
    Resident resident;
    std::atomic<USHORT> volume{100};
public:
    explicit Audio(std::shared_ptr<Job> value):job(value),resident(value->ledger,value->reservation*2+65536) {
        CheckHr(stream.CoCreateInstance(CLSID_SpStream));CheckHr(stream->BindToFile(job->path.c_str(),SPFM_OPEN_READONLY,nullptr,nullptr,SPFEI_ALL_TTS_EVENTS));
        stream.QueryInterface(&events);snapshot=job->readyInfo;CheckHr(CoCreateFreeThreadedMarshaler(static_cast<IHdPreparedAudio*>(this),&marshaler));++Objects;
    }
    ~Audio(){marshaler.Release();events.Release();stream.Release();job.reset();--Objects;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i,void**p) override{
        if(!p)return E_POINTER;*p=nullptr;
        if(i==IID_IMarshal)return marshaler->QueryInterface(i,p);
        if(i==IID_IUnknown||i==__uuidof(IHdPreparedAudio))*p=static_cast<IHdPreparedAudio*>(this);
        else if(i==IID_IStream||i==IID_ISequentialStream||i==__uuidof(ISpStream)||i==__uuidof(ISpStreamFormat))*p=static_cast<ISpStream*>(this);
        else if((i==__uuidof(ISpEventSource)||i==__uuidof(ISpNotifySource))&&events)*p=static_cast<ISpEventSource*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;} ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetStream(ISpStream**p) override{if(!p)return E_POINTER;*p=nullptr;if(job->cancelled)return HRESULT_FROM_WIN32(ERROR_CANCELLED);*p=static_cast<ISpStream*>(this);AddRef();return S_OK;}
    HRESULT STDMETHODCALLTYPE GetStatus(HD_PREPARE_STATUS*p) override{if(!p||p->cbSize!=sizeof(*p))return E_INVALIDARG;*p=snapshot;if(job->cancelled){p->state=HD_CANCELLED;p->error=HRESULT_FROM_WIN32(ERROR_CANCELLED);}return S_OK;}
    HRESULT STDMETHODCALLTYPE SetPlaybackVolume(USHORT value) override{if(value>100)return E_INVALIDARG;volume=value;return S_OK;}
    HRESULT STDMETHODCALLTYPE Validate(LPCWSTR text,LPCWSTR token,LONG rate,ULONGLONG generation) override{if(!text||!token)return E_POINTER;if(job->cancelled)return HRESULT_FROM_WIN32(ERROR_CANCELLED);return job->text==text&&_wcsicmp(job->voice.c_str(),token)==0&&job->rate==rate&&job->generation==generation?S_OK:S_FALSE;}
    HRESULT STDMETHODCALLTYPE Read(void*p,ULONG n,ULONG*r) override{if(job->cancelled)return E_ABORT;ULONG got=0;HRESULT hr=stream->Read(p,n,&got);USHORT level=volume.load();if(SUCCEEDED(hr)&&level<100){auto samples=static_cast<short*>(p);for(ULONG i=0;i<got/2;++i)samples[i]=static_cast<short>(int(samples[i])*level/100);}if(r)*r=got;return hr;}
    HRESULT STDMETHODCALLTYPE Write(const void*,ULONG,ULONG*) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER a,DWORD b,ULARGE_INTEGER*c) override{return stream->Seek(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*p,ULARGE_INTEGER n,ULARGE_INTEGER*a,ULARGE_INTEGER*b) override{return stream->CopyTo(p,n,a,b);}
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override{return S_OK;} HRESULT STDMETHODCALLTYPE Revert() override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return stream->LockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER a,ULARGE_INTEGER b,DWORD c) override{return stream->UnlockRegion(a,b,c);}
    HRESULT STDMETHODCALLTYPE Stat(STATSTG*p,DWORD f) override{return stream->Stat(p,f);}
    HRESULT STDMETHODCALLTYPE Clone(IStream**p) override{
        if(!p)return E_POINTER;*p=nullptr;try{LARGE_INTEGER zero{};ULARGE_INTEGER position{};CheckHr(stream->Seek(zero,STREAM_SEEK_CUR,&position));auto copy=new Audio(job);copy->SetPlaybackVolume(volume.load());LARGE_INTEGER offset;offset.QuadPart=position.QuadPart;HRESULT hr=copy->Seek(offset,STREAM_SEEK_SET,nullptr);if(FAILED(hr)){copy->Release();return hr;}*p=static_cast<ISpStream*>(copy);return S_OK;}catch(HRESULT e){return e;}catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE GetFormat(GUID*a,WAVEFORMATEX**b) override{return stream->GetFormat(a,b);}
    HRESULT STDMETHODCALLTYPE SetBaseStream(IStream*,REFGUID,const WAVEFORMATEX*) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE GetBaseStream(IStream**p) override{if(!p)return E_POINTER;*p=nullptr;try{CComPtr<IStream> base;HRESULT hr=stream->GetBaseStream(&base);if(FAILED(hr))return hr;*p=new BaseStream(base,job);return S_OK;}catch(HRESULT e){return e;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE BindToFile(LPCWSTR,SPFILEMODE,const GUID*,const WAVEFORMATEX*,ULONGLONG) override{return STG_E_ACCESSDENIED;}
    HRESULT STDMETHODCALLTYPE Close() override{return stream->Close();}
    HRESULT STDMETHODCALLTYPE SetNotifySink(ISpNotifySink*p) override{return events->SetNotifySink(p);}
    HRESULT STDMETHODCALLTYPE SetNotifyWindowMessage(HWND a,UINT b,WPARAM c,LPARAM d) override{return events->SetNotifyWindowMessage(a,b,c,d);}
    HRESULT STDMETHODCALLTYPE SetNotifyCallbackFunction(SPNOTIFYCALLBACK*a,WPARAM b,LPARAM c) override{return events->SetNotifyCallbackFunction(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetNotifyCallbackInterface(ISpNotifyCallback*a,WPARAM b,LPARAM c) override{return events->SetNotifyCallbackInterface(a,b,c);}
    HRESULT STDMETHODCALLTYPE SetNotifyWin32Event() override{return events->SetNotifyWin32Event();}
    HRESULT STDMETHODCALLTYPE WaitForNotifyEvent(DWORD a) override{return events->WaitForNotifyEvent(a);}
    HANDLE STDMETHODCALLTYPE GetNotifyEventHandle() override{return events->GetNotifyEventHandle();}
    HRESULT STDMETHODCALLTYPE SetInterest(ULONGLONG a,ULONGLONG b) override{return events->SetInterest(a,b);}
    HRESULT STDMETHODCALLTYPE GetEvents(ULONG a,SPEVENT*b,ULONG*c) override{return events->GetEvents(a,b,c);}
    HRESULT STDMETHODCALLTYPE GetInfo(SPEVENTSOURCEINFO*p) override{return events->GetInfo(p);}
};
class Service : public IHdSpeechPreparer {
    std::atomic<ULONG> refs{1};std::mutex lock;std::condition_variable wake;std::thread worker;bool stopping=false;
    HD_PREPARE_LIMITS limits;std::shared_ptr<Ledger> ledger=std::make_shared<Ledger>();uint64_t generation=0,next=0;
    std::map<uint64_t,std::shared_ptr<Job>> jobs;std::deque<std::shared_ptr<Job>> queue;std::shared_ptr<Job> active;CComPtr<IUnknown> marshaler;
    static uint64_t FileSize(const std::wstring& path){WIN32_FILE_ATTRIBUTE_DATA data{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data))return 0;return (uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;}
    void Run(){
        HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);CComPtr<ISpVoice> voice;std::wstring selected;
        for(;;){
            std::shared_ptr<Job> job;
            {std::unique_lock guard(lock);wake.wait(guard,[&]{return stopping||!queue.empty();});if(stopping)break;job=queue.front();queue.pop_front();if(job->cancelled)continue;active=job;job->state=HD_RUNNING;}
            HRESULT result=S_OK; CComPtr<ISpStream> file;
            try{
                CheckHr(init);if(!voice||selected!=job->voice){voice.Release();CheckHr(voice.CoCreateInstance(CLSID_SpVoice));CComPtr<ISpObjectToken> token;CheckHr(token.CoCreateInstance(CLSID_SpObjectToken));CheckHr(token->SetId(nullptr,job->voice.c_str(),FALSE));auto cls=TokenString(token,L"CLSID");if(_wcsicmp(cls.c_str(),L"{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8C}")&&_wcsicmp(cls.c_str(),L"{D8A46B78-87DF-4DDE-9E2A-468CE2F31D8D}"))throw E_INVALIDARG;CheckHr(voice->SetVoice(token));selected=job->voice;}
                CheckHr(voice->SetRate(job->rate));CheckHr(voice->SetVolume(100));
                wchar_t dir[MAX_PATH]{},path[MAX_PATH]{};Check(GetTempPathW(MAX_PATH,dir));Check(GetTempFileNameW(dir,L"hdp",0,path));job->path=path;
                CheckHr(file.CoCreateInstance(CLSID_SpStream));CSpStreamFormat format;CheckHr(format.AssignFormat(SPSF_24kHz16BitMono));
                ULONGLONG interests=SPFEI(SPEI_WORD_BOUNDARY)|SPFEI(SPEI_SENTENCE_BOUNDARY)|SPFEI(SPEI_VISEME)|SPFEI(SPEI_TTS_BOOKMARK);
                CheckHr(file->BindToFile(path,SPFM_CREATE_ALWAYS,&format.FormatId(),format.WaveFormatExPtr(),interests));CheckHr(voice->SetOutput(file,TRUE));
                if(job->cancelled)throw HRESULT_FROM_WIN32(ERROR_CANCELLED);
                HdTrace::Log(job->id,"prepare_begin",job->generation,job->rate);
                CheckHr(voice->Speak(job->text.c_str(),SPF_ASYNC|SPF_IS_NOT_XML,nullptr));
                while(voice->WaitUntilDone(10)==S_FALSE){
                    if(job->cancelled||FileSize(job->path)>job->reservation){if(!job->cancelled)result=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);CheckHr(voice->Speak(nullptr,SPF_ASYNC|SPF_PURGEBEFORESPEAK,nullptr));CheckHr(voice->WaitUntilDone(INFINITE));break;}
                }
                SPVOICESTATUS status{};CheckHr(voice->GetStatus(&status,nullptr));CheckHr(status.hrLastResult);
                CheckHr(file->Close());
                auto traceDirectory=HdTrace::Directory();if(!traceDirectory.empty())CopyFileW(job->path.c_str(),(traceDirectory+L"\\prepared-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(job->id)+L".wav").c_str(),FALSE);
                uint64_t size=FileSize(job->path);if(size>job->reservation)result=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
                if(SUCCEEDED(result)){std::lock_guard budget(ledger->lock);ledger->bytes-=job->reservation-size;job->reservation=size;}
            }catch(HRESULT e){result=e;}catch(...){result=E_FAIL;}
            if(file)file->Close();

            {std::lock_guard guard(lock);if(job->cancelled||job->generation!=generation){job->state=HD_CANCELLED;job->error=HRESULT_FROM_WIN32(ERROR_CANCELLED);}else{job->state=FAILED(result)?HD_FAILED:HD_READY;job->error=result;}if(job->state==HD_READY)job->Status(&job->readyInfo);active.reset();HdTrace::Log(job->id,"prepare_done",job->state,job->reservation);}
        }
        voice.Release();if(SUCCEEDED(init))CoUninitialize();
    }
public:
    Service(){CheckHr(CoCreateFreeThreadedMarshaler(static_cast<IHdSpeechPreparer*>(this),&marshaler));++Objects;}
    ~Service(){{std::lock_guard guard(lock);stopping=true;if(active)active->cancelled=true;for(auto&p:jobs)p.second->cancelled=true;}wake.notify_all();if(worker.joinable())worker.join();jobs.clear();queue.clear();marshaler.Release();--Objects;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID i,void**p) override{if(!p)return E_POINTER;*p=nullptr;if(i==IID_IMarshal)return marshaler->QueryInterface(i,p);if(i!=IID_IUnknown&&i!=__uuidof(IHdSpeechPreparer))return E_NOINTERFACE;*p=static_cast<IHdSpeechPreparer*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}ULONG STDMETHODCALLTYPE Release() override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetCapabilities(HD_PREPARE_CAPS*p) override{if(!p||p->cbSize!=sizeof(*p))return E_INVALIDARG;std::lock_guard guard(lock);p->version=1;p->flags=31;p->limits=limits;return S_OK;}
    HRESULT STDMETHODCALLTYPE SetLimits(const HD_PREPARE_LIMITS*p) override{if(!p||p->cbSize!=sizeof(*p)||!p->maxJobs||p->maxJobs>32||!p->maxTextChars||p->maxTextChars>65536||p->maxTotalTextChars<p->maxTextChars||p->maxTotalTextChars>262144||p->maxEntryBytes<65536||p->maxCacheBytes<p->maxEntryBytes||p->maxCacheBytes>256ull*1024*1024||p->maxResidentBytes<65536||p->maxResidentBytes>256ull*1024*1024)return E_INVALIDARG;std::lock_guard guard(lock);std::lock_guard budget(ledger->lock);if(!jobs.empty()||ledger->bytes||ledger->resident)return HRESULT_FROM_WIN32(ERROR_BUSY);limits=*p;ledger->residentLimit=p->maxResidentBytes;return S_OK;}
    HRESULT STDMETHODCALLTYPE AdvanceGeneration(ULONGLONG value) override{std::lock_guard guard(lock);if(value<=generation)return E_INVALIDARG;generation=value;for(auto&p:jobs)p.second->cancelled=true;jobs.clear();queue.clear();wake.notify_all();return S_OK;}
    HRESULT STDMETHODCALLTYPE Prepare(LPCWSTR text,LPCWSTR token,LONG rate,ULONGLONG gen,ULONGLONG*ticket) override{
        if(!text||!token||!*token||!ticket||rate<-10||rate>10)return E_INVALIDARG;*ticket=0;
        try{std::lock_guard guard(lock);if(!gen||gen!=generation)return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            size_t length=wcsnlen_s(text,limits.maxTextChars+1);if(!length||length>limits.maxTextChars||wcsnlen_s(token,4097)>4096)return E_INVALIDARG;
            for(auto&p:jobs){auto&j=*p.second;if(!j.cancelled&&j.generation==gen&&j.rate==rate&&_wcsicmp(j.voice.c_str(),token)==0&&j.text==text&&(j.state==HD_READY||j.state==HD_RUNNING||j.state==HD_QUEUED)){++j.hits;*ticket=j.id;return j.state==HD_READY?S_FALSE:S_OK;}}
            uint64_t residentCharge=(length+wcslen(token)+2)*4+4096;
            auto fits=[&]{std::lock_guard budget(ledger->lock);return jobs.size()<limits.maxJobs&&ledger->bytes+limits.maxEntryBytes<=limits.maxCacheBytes&&ledger->text+length<=limits.maxTotalTextChars&&ledger->resident+residentCharge<=limits.maxResidentBytes;};
            for(auto i=jobs.begin();!fits()&&i!=jobs.end();){if(i->second->state==HD_READY||i->second->state==HD_FAILED||i->second->state==HD_CANCELLED)i=jobs.erase(i);else ++i;}
            if(!fits())return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
            auto job=std::make_shared<Job>();job->id=++next;job->generation=gen;job->rate=rate;job->text=text;job->voice=token;job->reservation=limits.maxEntryBytes;job->residentCharge=residentCharge;
            {std::lock_guard budget(ledger->lock);ledger->bytes+=job->reservation;ledger->text+=length;ledger->resident+=residentCharge;}job->ledger=ledger;
            if(!worker.joinable())worker=std::thread([this]{Run();});jobs.emplace(job->id,job);queue.push_back(job);*ticket=job->id;wake.notify_all();return S_OK;
        }catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE GetStatus(ULONGLONG id,HD_PREPARE_STATUS*p) override{if(!p||p->cbSize!=sizeof(*p))return E_INVALIDARG;std::lock_guard guard(lock);auto found=jobs.find(id);if(found==jobs.end())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);found->second->Status(p);return S_OK;}
    HRESULT STDMETHODCALLTYPE Acquire(ULONGLONG id,IHdPreparedAudio**p) override{if(!p)return E_POINTER;*p=nullptr;try{std::lock_guard guard(lock);auto found=jobs.find(id);if(found==jobs.end()||found->second->cancelled)return HRESULT_FROM_WIN32(ERROR_CANCELLED);if(found->second->state!=HD_READY)return E_PENDING;*p=new Audio(found->second);return S_OK;}catch(HRESULT e){return e;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE Cancel(ULONGLONG id) override{std::lock_guard guard(lock);auto found=jobs.find(id);if(found==jobs.end())return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);found->second->cancelled=true;found->second->state=HD_CANCELLED;found->second->error=HRESULT_FROM_WIN32(ERROR_CANCELLED);return S_OK;}
};
}
