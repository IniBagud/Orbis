#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

// Daftar aplikasi yang diizinkan memicu Orbis (gunakan huruf kecil)
const std::vector<std::wstring> TARGET_PROCESSES = {
    L"explorer.exe",
};

bool g_isTargetAppFocused = false;
HHOOK g_mouseHook = NULL;
HWINEVENTHOOK g_winEventHook = NULL;

// Mengecek apakah jendela yang sedang aktif termasuk dalam daftar TARGET_PROCESSES
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

// Dipanggil otomatis oleh Windows setiap kali user berpindah jendela aktif
void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (event == EVENT_SYSTEM_FOREGROUND) {
        bool wasFocused = g_isTargetAppFocused;
        g_isTargetAppFocused = CheckForegroundProcess(hwnd);

        if (g_isTargetAppFocused && !wasFocused) {
            std::wcout << L"[FOCUS] Aplikasi target AKTIF -> Menyiagakan OrbisConverter...\n";
            // Fase berikutnya: Jalankan/bangunkan OrbisConverter.exe di latar belakang
        } else if (!g_isTargetAppFocused && wasFocused) {
            std::wcout << L"[FOCUS] Keluar dari aplikasi target -> Timer idle 2 menit dimulai.\n";
        }
    }
}

// Menangkap kombinasi Shift + Middle Click saat aplikasi target aktif
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_isTargetAppFocused) {
        if (wParam == WM_MBUTTONDOWN) {
            bool isShiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if (isShiftPressed) {
                MSLLHOOKSTRUCT* pMouse = (MSLLHOOKSTRUCT*)lParam;
                std::wcout << L"[TRIGGER] Shift + Middle Click pada koordinat: ("
                           << pMouse->pt.x << L", " << pMouse->pt.y << L")\n";

                // Fase berikutnya: Kirim perintah "SHOW_MENU" beserta koordinat via Named Pipe

                // Return 1 menelan event klik agar tidak memicu aksi bawaan Windows
                return 1;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

int main() {
    std::wcout << L"========================================\n";
    std::wcout << L"   ORBIS WATCHER - FASE 1 (DEBUG MODE)  \n";
    std::wcout << L"========================================\n";
    std::wcout << L"Instruksi uji coba:\n";
    std::wcout << L"1. Buka File Explorer.\n";
    std::wcout << L"2. Tekan Shift + Middle Click di area Explorer.\n";
    std::wcout << L"3. Tekan Ctrl + C di terminal ini untuk keluar.\n\n";

    // Cek status jendela aktif saat program baru saja dimulai
    g_isTargetAppFocused = CheckForegroundProcess(GetForegroundWindow());
    if (g_isTargetAppFocused) {
        std::wcout << L"[FOCUS] Aplikasi target sudah aktif sejak awal.\n";
    }

    // Pasang Event Hook perpindahan fokus jendela
    g_winEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    // Pasang Low-Level Mouse Hook
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);

    if (!g_winEventHook || !g_mouseHook) {
        std::wcerr << L"[FATAL] Gagal memasang Windows Hook!\n";
        return 1;
    }

    // Message loop standar Windows
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_mouseHook);
    UnhookWinEvent(g_winEventHook);
    return 0;
}