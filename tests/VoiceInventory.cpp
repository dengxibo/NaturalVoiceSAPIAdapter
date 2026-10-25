#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <atlbase.h>
#include <cstdio>
#include <string>
#include "../NaturalVoiceSAPIAdapter/LocalVoiceKind.h"
static void Check(HRESULT hr){if(FAILED(hr)){printf("FAIL %08lx\n",static_cast<unsigned long>(hr));ExitProcess(1);}}
int wmain(int argc,wchar_t** argv){
 if(argc<4)return 2;
 if(IsHdVoicePath(argv[2])||!IsHdVoicePath(argv[3])){puts("FAIL HD/ordinary routing");return 1;}
 puts("PASS real ordinary and HD model routing");
 Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
 auto module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);if(!module)return 3;
 auto getClass=reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));if(!getClass)return 4;
 CLSID cls{};Check(CLSIDFromString(L"{B8B9E38F-E5A2-4661-9FDE-4AC7377AA6F6}",&cls));
 CComPtr<IClassFactory> factory;Check(getClass(cls,IID_PPV_ARGS(&factory)));
 CComPtr<IEnumSpObjectTokens> voices;Check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&voices)));
 ULONG count=0,ordinary=0;Check(voices->GetCount(&count));
 for(ULONG i=0;i<count;++i){CComPtr<ISpObjectToken> token;Check(voices->Item(i,&token));CComPtr<ISpDataKey> attrs;Check(token->OpenKey(L"Attributes",&attrs));LPWSTR kind=nullptr,name=nullptr;
  if(SUCCEEDED(attrs->GetStringValue(L"NaturalVoiceType",&kind))&&kind&&wcsstr(kind,L"Narrator")){
   Check(token->GetStringValue(L"",&name));if(wcsstr(name,L"Natural HD")){puts("FAIL HD leaked into legacy SDK");return 5;}++ordinary;wprintf(L"ordinary: %ls\n",name);
  }CoTaskMemFree(kind);CoTaskMemFree(name);
 }
 printf("PASS adapter inventory total=%lu ordinary=%lu; HD separate\n",count,ordinary);
 return ordinary?0:6;
}
