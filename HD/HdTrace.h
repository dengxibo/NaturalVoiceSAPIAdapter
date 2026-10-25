#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <string>
namespace HdTrace {
inline std::atomic<unsigned long long> sequence{0};
inline long long Tick() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline std::wstring Directory() { wchar_t value[32768]{}; DWORD n=GetEnvironmentVariableW(L"HD_SAPI_TRACE_DIR",value,32768); return n && n<32768 ? value : L""; }
inline void Log(unsigned long long call,const char* phase,unsigned long long a=0,unsigned long long b=0) noexcept {
    try {
        static std::mutex lock; static std::wstring dir=Directory();
        if(dir.empty()) return;
        std::lock_guard guard(lock);
        std::wstring path=dir+L"\\trace-"+std::to_wstring(GetCurrentProcessId())+L".csv";
        FILE* file=nullptr; if(_wfopen_s(&file,path.c_str(),L"ab") || !file) return;
        fprintf(file,"%lld,%lu,%lu,%llu,%s,%llu,%llu\n",Tick(),GetCurrentProcessId(),GetCurrentThreadId(),call,phase,a,b); fclose(file);
    } catch(...) {}
}
inline void Pcm(unsigned long long call,const void* data,size_t size) noexcept {
    try {
        static std::wstring dir=Directory(); if(dir.empty()) return;
        auto path=dir+L"\\pcm-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(call)+L".raw";
        FILE* file=nullptr; if(_wfopen_s(&file,path.c_str(),L"ab") || !file) return; fwrite(data,1,size,file); fclose(file);
    }catch(...){}
}
}
