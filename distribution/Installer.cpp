#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string>
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR command,int) {
    const bool checkOnly=wcsstr(command,L"/check")!=nullptr;
    std::wstring action=L"Install";
    if(wcsstr(command,L"/uninstall")) action=L"Uninstall";
    else if(wcsstr(command,L"/advanced")) action=L"Configure";
    else if(!checkOnly) {
        int choice=MessageBoxW(nullptr,
            L"安装或升级联网 secv2、普通本地语音、HD 语音及预合成服务。\n"
            L"普通版与 HD 分别保留；不包含微软模型或系统运行库。\n\n"
            L"无需预装其他语音适配器。\n是：安装 / 升级（一次管理员确认）\n否：语音设置\n取消：退出",
            L"NaturalVoiceSAPIAdapter 完整版",MB_YESNOCANCEL|MB_ICONINFORMATION);
        if(choice==IDCANCEL)return 0;
        if(choice==IDNO)action=L"Configure";
    }
    wchar_t path[32768]{};DWORD size=GetModuleFileNameW(nullptr,path,32768);
    if(!size||size>=32768)return 2;
    std::wstring root(path);root.resize(root.find_last_of(L"\\/"));
    auto isFile=[](const std::wstring& file){DWORD attrs=GetFileAttributesW(file.c_str());return attrs!=INVALID_FILE_ATTRIBUTES&&!(attrs&FILE_ATTRIBUTE_DIRECTORY);};
    std::wstring script=root+L"\\data\\Install-Full.ps1";
    if(!isFile(script))script=root+L"\\Install-Full.ps1";
    if(checkOnly)return isFile(script)?0:2;
    if(!isFile(script)){MessageBoxW(nullptr,L"安装文件不完整，请保留 Installer.exe 旁的 data 文件夹。",L"安装失败",MB_ICONERROR);return 2;}
    wchar_t windows[32768]{};GetWindowsDirectoryW(windows,32768);
    std::wstring powershell=std::wstring(windows)+L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    // 32-bit bootstrap must launch the native host on x64 Windows.
    BOOL wow64=FALSE;IsWow64Process(GetCurrentProcess(),&wow64);
    if(wow64)powershell=std::wstring(windows)+L"\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring arguments=L"-NoProfile -ExecutionPolicy Bypass -File \""+script+L"\" -Action "+action;
    SHELLEXECUTEINFOW info{sizeof(info)};info.fMask=SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb=action==L"Configure"?L"open":L"runas";info.lpFile=powershell.c_str();
    info.lpParameters=arguments.c_str();info.nShow=SW_HIDE;
    if(!ShellExecuteExW(&info))return GetLastError()==ERROR_CANCELLED?0:2;
    WaitForSingleObject(info.hProcess,INFINITE);DWORD result=1;GetExitCodeProcess(info.hProcess,&result);CloseHandle(info.hProcess);
    if(action!=L"Configure")MessageBoxW(nullptr,result?
        L"操作未完成。请在 PowerShell 运行 Install-Full.ps1 查看错误；原有注册会尝试恢复。":
        (action==L"Uninstall"?L"卸载完成。模型和用户设置保留；关闭读者后可删除安装目录。":L"安装完成，请完全退出并重新打开朗读软件。HD 仅在系统扩展及本地模型可用时注册。"),
        L"NaturalVoiceSAPIAdapter 完整版",result?MB_ICONERROR:MB_ICONINFORMATION);
    return static_cast<int>(result);
}
