#pragma once
#include <windows.h>
#include <string>
// Route HD packages to the separate system-extension engine. Loading HD and
// legacy folders together through the old embedded SDK can poison enumeration
// of otherwise working ordinary voices. Names/IDs of ordinary tokens stay intact.
inline bool IsHdVoicePath(const std::wstring& directory) {
    std::wstring file=directory+L"\\Tokens.xml";
    HANDLE handle=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(handle,&size)||size.QuadPart<=0||size.QuadPart>1024*1024){CloseHandle(handle);return false;}
    std::string text(static_cast<size_t>(size.QuadPart),'\0');DWORD count=0;
    BOOL ok=ReadFile(handle,text.data(),static_cast<DWORD>(text.size()),&count,nullptr);CloseHandle(handle);
    if(!ok)return false;text.resize(count);
    if(text.find("(Natural HD)")!=std::string::npos)return true;
    const wchar_t marker[]=L"(Natural HD)";
    return text.find(std::string(reinterpret_cast<const char*>(marker),sizeof(marker)-sizeof(wchar_t)))!=std::string::npos;
}
