#include "Installer.h"

void Register(bool is64Bit);
void Unregister(bool is64Bit);
INT_PTR CALLBACK MainDlg(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (_wcsicmp(lpCmdLine, L"-uninstall") == 0)
    {
        WCHAR file[MAX_PATH]{};
        GetModuleFileNameW(nullptr, file, MAX_PATH);
        const WCHAR* name = wcsrchr(file, L'\\');
        const WCHAR* filename = name ? name + 1 : file;
        if (_wcsicmp(filename, L"AdvancedInstaller.exe") == 0 || _wcsicmp(filename, L"VoiceSettings.exe") == 0)
        {
            MessageBoxW(nullptr, L"请使用完整版 Installer.exe /uninstall 统一卸载。", L"NaturalVoiceSAPIAdapter", MB_ICONINFORMATION);
            return ERROR_INVALID_FUNCTION;
        }
        try
        {
            Unregister(false);
            if (Is64BitSystem())
                Unregister(true);

            ReportError(ERROR_SUCCESS);
        }
        catch (const std::system_error& ex)
        {
            DWORD err = ex.code().value();
            ReportError(err);
            return err;
        }
    }
    else
    {
        DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, MainDlg, 0);
    }

    return 0;
}
