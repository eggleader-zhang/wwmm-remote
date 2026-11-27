// This per-user launcher embeds the same Inno installer that the offline updater verifies.
// Resource-only readers can extract it without starting any executable from a shared folder.
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <string>
#include <vector>

static bool appRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    PROCESSENTRY32W e{sizeof(e)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        DWORD other;
        if (!_wcsicmp(e.szExeFile, L"WWMM Remote.exe") && ProcessIdToSessionId(e.th32ProcessID,&other) && other == session) {
            found = true; break;
        }
    }
    CloseHandle(snap);
    return found;
}
static int install(HINSTANCE self, const std::wstring &folder, const std::wstring &exe, const wchar_t *args) {
    auto item = FindResourceW(self, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
    if (!item) return 1;
    DWORD size = SizeofResource(self,item), written = 0;
    auto data = LockResource(LoadResource(self,item));
    HANDLE f = CreateFileW(exe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (!data || f == INVALID_HANDLE_VALUE) return 1;
    bool copied = WriteFile(f,data,size,&written,nullptr) && written == size;
    CloseHandle(f);
    if (!copied) return 1;
    std::wstring command = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION p{};
    if (!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,folder.c_str(),&si,&p)) return 1;
    WaitForSingleObject(p.hProcess,INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(p.hProcess,&result);
    CloseHandle(p.hThread); CloseHandle(p.hProcess);
    return static_cast<int>(result);
}
int WINAPI wWinMain(HINSTANCE self,HINSTANCE,wchar_t *args,int) {
    if (appRunning()) {
        if (!wcsstr(args,L"/VERYSILENT"))
            MessageBoxW(nullptr,L"请先结束当前会话，从系统托盘退出 WWMM Remote，然后重新打开安装程序。",L"WWMM Remote",MB_OK|MB_ICONINFORMATION);
        return 32;
    }
    wchar_t temp[MAX_PATH+1]{}, name[40]{};
    GUID id;
    if (!GetTempPathW(MAX_PATH,temp) || FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id,name,40)) return 1;
    std::wstring folder = std::wstring(temp) + L"WWMM-" + name;
    if (!CreateDirectoryW(folder.c_str(),nullptr)) return 1;
    auto exe = folder + L"\\install.exe";
    int result = install(self,folder,exe,args);
    DeleteFileW(exe.c_str()); RemoveDirectoryW(folder.c_str());
    return result;
}
