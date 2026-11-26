#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>

const std::vector<std::wstring> TARGET_PROCESSES = {
    L"explorer.exe",
    L"chrome.exe",
    L"msedge.exe"
};

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\OrbisPipe";
const wchar_t* CONVERTER_MUTEX = L"Orbis_Converter_Mutex";
constexpr UINT WM_APP_TRIGGER = WM_APP + 1;
constexpr UINT WM_APP_FOCUS   = WM_APP + 2;

bool g_isTargetAppFocused = false;
HHOOK g_mouseHook = NULL;
HWINEVENTHOOK g_winEventHook = NULL;
DWORD g_mainThreadId = 0;

// Mengecek apakah OrbisConverter.exe sudah berjalan
bool IsConverterRunning() {
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, CONVERTER_MUTEX);
    if (hMutex) {
        CloseHandle(hMutex);
        return true;
    }
    return false;
}

// Meluncurkan OrbisConverter.exe dari folder yang sama dengan OrbisWatcher.exe
bool EnsureConverterRunning() {
    if (IsConverterRunning()) return true;

    wchar_t watcherPath[MAX_PATH];
    GetModuleFileNameW(NULL, watcherPath, MAX_PATH);
    std::wstring dir(watcherPath);
    size_t lastSlash = dir.find_last_of(L"\\/");
    std::wstring converterPath = dir.substr(0, lastSlash + 1) + L"OrbisConverter.exe";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (CreateProcessW(converterPath.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::wcout << L"[WATCHER] Meluncurkan OrbisConverter.exe...\n";
        WaitNamedPipeW(PIPE_NAME, 1500); // Tunggu maksimal 1.5 detik sampai pipe siap
        return true;
    }

    std::wcerr << L"[WATCHER] Gagal meluncurkan OrbisConverter.exe!\n";
    return false;
}

// Mengirim pesan teks singkat ke OrbisConverter.exe via Named Pipe
void SendPipeMessage(const std::string& message) {
    if (!EnsureConverterRunning()) return;

    HANDLE hPipe = CreateFileW(
        PIPE_NAME, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL
    );

    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD bytesWritten = 0;
        WriteFile(hPipe, message.c_str(), static_cast<DWORD>(message.size()), &bytesWritten, NULL);
        CloseHandle(hPipe);
    }
}

bool CheckForegroundProcess(HWND hwnd) {
    if (!hwnd) return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) return false;

    wchar_t processPath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool isMatch = false;

    if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
        std::wstring fullPath(processPath);
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        std::wstring exeName = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
        std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);

        for (const auto& target : TARGET_PROCESSES) {
            if (exeName == target) {
                isMatch = true;
                break;
            }
        }
    }

    CloseHandle(hProcess);
    return isMatch;
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        bool wasFocused = g_isTargetAppFocused;
        g_isTargetAppFocused = CheckForegroundProcess(hwnd);

        if (g_isTargetAppFocused && !wasFocused) {
            std::wcout << L"[WATCHER] Masuk mode fokus -> Membangunkan OrbisConverter.\n";
            PostThreadMessageW(g_mainThreadId, WM_APP_FOCUS, 0, 0);
        } else if (!g_isTargetAppFocused && wasFocused) {
            std::wcout << L"[WATCHER] Keluar mode fokus.\n";
        }
    }
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_isTargetAppFocused) {
        if (wParam == WM_MBUTTONDOWN) {
            bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (isShiftPressed) {
                MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
                HWND fgHwnd = GetForegroundWindow();

                // Kirim ke message loop utama agar hook mouse tidak pernah lag
                PostThreadMessageW(
                    g_mainThreadId, WM_APP_TRIGGER,
                    reinterpret_cast<WPARAM>(fgHwnd),
                    MAKELPARAM(pMouse->pt.x, pMouse->pt.y)
                );
                return 1;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

int main() {
    g_mainThreadId = GetCurrentThreadId();
    std::wcout << L"=== ORBIS WATCHER ===\n";

    g_isTargetAppFocused = CheckForegroundProcess(GetForegroundWindow());
    if (g_isTargetAppFocused) {
        SendPipeMessage("FOCUS");
    }

    g_winEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);

    if (!g_winEventHook || !g_mouseHook) {
        std::wcerr << L"[WATCHER] Gagal memasang Windows Hook!\n";
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.hwnd == NULL) {
            if (msg.message == WM_APP_FOCUS) {
                SendPipeMessage("FOCUS");
                continue;
            } else if (msg.message == WM_APP_TRIGGER) {
                int x = static_cast<short>(LOWORD(msg.lParam));
                int y = static_cast<short>(HIWORD(msg.lParam));
                uintptr_t hwndVal = reinterpret_cast<uintptr_t>(msg.wParam);

                std::ostringstream oss;
                oss << "SHOW " << x << " " << y << " " << hwndVal;
                SendPipeMessage(oss.str());
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_mouseHook);
    UnhookWinEvent(g_winEventHook);
    return 0;
}