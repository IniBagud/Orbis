#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <exdisp.h>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\OrbisPipe";
const wchar_t* CONVERTER_MUTEX = L"Orbis_Converter_Mutex";

constexpr ULONGLONG IDLE_TIMEOUT_MS = 120000; // 2 menit
constexpr UINT WM_APP_SHOW_MENU = WM_APP + 10;
constexpr UINT_PTR TIMER_FADE_IN = 1;

constexpr int WIN_SIZE = 420;
constexpr float CENTER_XY = WIN_SIZE / 2.0f;
constexpr float CENTER_R = 40.0f;
constexpr float INNER_R1 = 46.0f;
constexpr float INNER_R2 = 118.0f;
constexpr float OUTER_R1 = 124.0f;
constexpr float OUTER_R2 = 194.0f;
constexpr double PI = 3.14159265358979323846;

struct MenuSlice {
    std::wstring label;
    std::wstring action;
};

struct ShowRequest {
    int x = 0;
    int y = 0;
    std::vector<std::wstring> files;
    std::wstring primaryExt;
};

std::atomic<ULONGLONG> g_lastActivityTick{0};
std::atomic<int> g_activeConversions{0};
std::mutex g_reqMutex;
ShowRequest g_pendingReq;

HWND g_hwnd = NULL;
BYTE g_currentAlpha = 0;
bool g_isMenuVisible = false;
bool g_isCategoryMode = false;
bool g_canGoBack = false;

std::vector<std::wstring> g_currentFiles;
std::wstring g_currentPrimaryExt;
std::wstring g_centerLabel = L"BATAL";
std::vector<MenuSlice> g_innerSlices;
std::vector<MenuSlice> g_outerSlices;

bool g_hoveredCenter = false;
int g_hoveredInner = -1;
int g_hoveredOuter = -1;
int g_activeCategoryIdx = -1;

// --- Pemantau Idle ---
void IdleMonitorThread() {
    while (true) {
        Sleep(1000);
        ULONGLONG elapsed = GetTickCount64() - g_lastActivityTick.load();
        // Jangan matikan proses jika menu sedang tampil ATAU konversi/pop-up error sedang aktif
        if (elapsed >= IDLE_TIMEOUT_MS && !g_isMenuVisible && g_activeConversions.load() == 0) {
            std::wcout << L"[CONVERTER] Idle 2 menit tercapai. Menutup proses.\n";
            ExitProcess(0);
        }
    }
}

// --- Utilitas File & Engine ---
std::wstring GetExeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? full.substr(0, pos + 1) : L"";
}

// Cari engine di folder build/bin/ dulu; jika tidak ada, gunakan PATH sistem
std::wstring ResolveEngineExecutable(const std::wstring& exeName) {
    fs::path localBin = fs::path(GetExeDirectory()) / L"bin" / exeName;
    if (fs::exists(localBin)) {
        return L"\"" + localBin.wstring() + L"\"";
    }
    return exeName;
}

std::wstring GetFileExtension(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return L"";
    }
    size_t dotPos = path.find_last_of(L'.');
    size_t slashPos = path.find_last_of(L"\\/");
    if (dotPos == std::wstring::npos || (slashPos != std::wstring::npos && dotPos < slashPos)) {
        return L"";
    }
    std::wstring ext = path.substr(dotPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

bool IsImageExtension(const std::wstring& ext) {
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp" || ext == L".ico";
}

// Buat nama output unik agar tidak menimpa file yang sudah ada
std::wstring BuildUniqueOutputPath(const std::wstring& inputFile, const std::wstring& targetFormat) {
    fs::path inPath(inputFile);
    fs::path dir = inPath.parent_path();
    std::wstring stem = inPath.stem().wstring();
    std::wstring outExt = L"." + targetFormat;

    fs::path candidate = dir / (stem + outExt);
    int counter = 1;
    while (fs::exists(candidate)) {
        candidate = dir / (stem + L"_" + std::to_wstring(counter) + outExt);
        counter++;
    }
    return candidate.wstring();
}

// Menjalankan perintah CLI secara tersembunyi (tanpa jendela konsol)
bool RunHiddenProcess(std::wstring commandLine, DWORD& outExitCode, std::wstring& outError) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    // CreateProcessW membutuhkan buffer command line yang bisa dimodifikasi
    std::vector<wchar_t> cmdBuf(commandLine.begin(), commandLine.end());
    cmdBuf.push_back(L'\0');

    BOOL created = CreateProcessW(
        NULL, cmdBuf.data(), NULL, NULL, FALSE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi
    );

    if (!created) {
        DWORD err = GetLastError();
        outError = L"Gagal menjalankan mesin konversi (Error code: " + std::to_wstring(err) +
                   L"). Pastikan ffmpeg.exe / magick.exe sudah ada di folder build/bin/ atau PATH Windows.";
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &outExitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// Membangun perintah CLI yang sesuai dengan format tujuan
bool BuildConversionCommand(const std::wstring& inputFile, const std::wstring& outputFile,
                            const std::wstring& targetFormat, std::wstring& outCmd) {
    std::wstring inExt = GetFileExtension(inputFile);
    std::wstring qIn = L"\"" + inputFile + L"\"";
    std::wstring qOut = L"\"" + outputFile + L"\"";

    // 1. Audio & Video -> FFmpeg
    if (targetFormat == L"mp3") {
        outCmd = ResolveEngineExecutable(L"ffmpeg.exe") + L" -y -i " + qIn + L" -vn -q:a 2 " + qOut;
        return true;
    }
    if (targetFormat == L"wav" || targetFormat == L"flac" || targetFormat == L"ogg" || targetFormat == L"m4a") {
        outCmd = ResolveEngineExecutable(L"ffmpeg.exe") + L" -y -i " + qIn + L" -vn " + qOut;
        return true;
    }
    if (targetFormat == L"gif") {
        outCmd = ResolveEngineExecutable(L"ffmpeg.exe") + L" -y -i " + qIn + L" -vf \"fps=15,scale=640:-1:flags=lanczos\" " + qOut;
        return true;
    }
    if (targetFormat == L"mp4" || targetFormat == L"mkv" || targetFormat == L"webm") {
        outCmd = ResolveEngineExecutable(L"ffmpeg.exe") + L" -y -i " + qIn + L" " + qOut;
        return true;
    }

    // 2. Gambar -> ImageMagick
    if (targetFormat == L"png" || targetFormat == L"jpg" || targetFormat == L"webp" || targetFormat == L"ico" ||
        (targetFormat == L"pdf" && IsImageExtension(inExt))) {
        outCmd = ResolveEngineExecutable(L"magick.exe") + L" " + qIn + L" " + qOut;
        return true;
    }

    // 3. Dokumen -> Pandoc
    if (targetFormat == L"pdf" || targetFormat == L"docx" || targetFormat == L"txt" || targetFormat == L"md" || targetFormat == L"html") {
        outCmd = ResolveEngineExecutable(L"pandoc.exe") + L" " + qIn + L" -o " + qOut;
        return true;
    }

    return false;
}

// Worker thread konversi
void ExecuteConversionAsync(std::vector<std::wstring> files, std::wstring targetFormat) {
    g_activeConversions.fetch_add(1);
    g_lastActivityTick.store(GetTickCount64());

    if (files.empty()) {
        MessageBoxW(
            NULL,
            L"Tidak ada file target yang diseleksi di File Explorer.",
            L"Orbis - Konversi Gagal",
            MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND
        );
        g_lastActivityTick.store(GetTickCount64());
        g_activeConversions.fetch_sub(1);
        return;
    }

    std::wstring errorSummary;
    int failCount = 0;

    for (const auto& inputFile : files) {
        DWORD attrs = GetFileAttributesW(inputFile.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            failCount++;
            errorSummary += L"- " + fs::path(inputFile).filename().wstring() + L": Target adalah folder atau tidak ditemukan.\n";
            continue;
        }

        std::wstring outputFile = BuildUniqueOutputPath(inputFile, targetFormat);
        std::wstring cmd;

        if (!BuildConversionCommand(inputFile, outputFile, targetFormat, cmd)) {
            failCount++;
            errorSummary += L"- " + fs::path(inputFile).filename().wstring() + L": Format tujuan ." + targetFormat + L" belum didukung.\n";
            continue;
        }

        std::wcout << L"[CONVERTER] Memproses: " << fs::path(inputFile).filename().wstring()
                   << L" -> ." << targetFormat << L"\n";

        DWORD exitCode = 0;
        std::wstring spawnErr;
        bool started = RunHiddenProcess(cmd, exitCode, spawnErr);

        if (!started) {
            failCount++;
            errorSummary += L"- " + fs::path(inputFile).filename().wstring() + L": " + spawnErr + L"\n";
        } else if (exitCode != 0) {
            failCount++;
            // Hapus file output kosong/rusak jika sempat terbentuk
            std::error_code ec;
            fs::remove(outputFile, ec);
            errorSummary += L"- " + fs::path(inputFile).filename().wstring() +
                            L": Mesin konversi gagal (Exit code: " + std::to_wstring(exitCode) + L").\n";
        } else {
            std::wcout << L"[CONVERTER] SUKSES -> " << outputFile << L"\n";
        }
    }

    // Jika ada yang gagal, tampilkan pop-up persisten hingga ditutup manual oleh user
    if (failCount > 0) {
        std::wstring msg = L"Konversi ke format ." + targetFormat + L" gagal pada " +
                           std::to_wstring(failCount) + L" file:\n\n" + errorSummary;
        MessageBoxW(
            NULL,
            msg.c_str(),
            L"Orbis - Konversi Gagal",
            MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND
        );
    }

    g_lastActivityTick.store(GetTickCount64());
    g_activeConversions.fetch_sub(1);
}

// --- Ekstraktor File Explorer (COM) ---
std::vector<std::wstring> GetSelectedExplorerFiles(HWND targetHwnd) {
    std::vector<std::wstring> selectedFiles;
    if (!targetHwnd) return selectedFiles;

    HWND activeTab = FindWindowExW(targetHwnd, NULL, L"ShellTabWindowClass", NULL);

    IShellWindows* pShellWindows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, reinterpret_cast<void**>(&pShellWindows)))) {
        return selectedFiles;
    }

    long count = 0;
    pShellWindows->get_Count(&count);

    for (long i = 0; i < count; ++i) {
        VARIANT vtIndex;
        VariantInit(&vtIndex);
        vtIndex.vt = VT_I4;
        vtIndex.lVal = i;

        IDispatch* pDisp = nullptr;
        if (SUCCEEDED(pShellWindows->Item(vtIndex, &pDisp)) && pDisp) {
            IWebBrowserApp* pWebBrowser = nullptr;
            if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowserApp, reinterpret_cast<void**>(&pWebBrowser)))) {
                SHANDLE_PTR browserHwnd = 0;
                pWebBrowser->get_HWND(&browserHwnd);

                if (reinterpret_cast<HWND>(browserHwnd) == targetHwnd) {
                    IServiceProvider* pServiceProvider = nullptr;
                    if (SUCCEEDED(pWebBrowser->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&pServiceProvider)))) {
                        IShellBrowser* pShellBrowser = nullptr;
                        if (SUCCEEDED(pServiceProvider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&pShellBrowser)))) {
                            bool isTargetTab = true;
                            if (activeTab != NULL) {
                                HWND tabHwnd = NULL;
                                pShellBrowser->GetWindow(&tabHwnd);
                                if (tabHwnd != activeTab) isTargetTab = false;
                            }

                            if (isTargetTab) {
                                IShellView* pShellView = nullptr;
                                if (SUCCEEDED(pShellBrowser->QueryActiveShellView(&pShellView))) {
                                    IFolderView* pFolderView = nullptr;
                                    if (SUCCEEDED(pShellView->QueryInterface(IID_IFolderView, reinterpret_cast<void**>(&pFolderView)))) {
                                        IShellItemArray* pItems = nullptr;
                                        if (SUCCEEDED(pFolderView->Items(SVGIO_SELECTION, IID_IShellItemArray, reinterpret_cast<void**>(&pItems)))) {
                                            DWORD itemCount = 0;
                                            pItems->GetCount(&itemCount);

                                            for (DWORD j = 0; j < itemCount; ++j) {
                                                IShellItem* pItem = nullptr;
                                                if (SUCCEEDED(pItems->GetItemAt(j, &pItem))) {
                                                    PWSTR pszPath = nullptr;
                                                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                                                        selectedFiles.emplace_back(pszPath);
                                                        CoTaskMemFree(pszPath);
                                                    }
                                                    pItem->Release();
                                                }
                                            }
                                            pItems->Release();
                                        }
                                        pFolderView->Release();
                                    }
                                    pShellView->Release();
                                }
                            }
                            pShellBrowser->Release();
                        }
                        pServiceProvider->Release();
                    }
                }
                pWebBrowser->Release();
            }
            pDisp->Release();
        }
        if (!selectedFiles.empty()) break;
    }

    pShellWindows->Release();
    return selectedFiles;
}

// --- Pengaturan Isi Menu (Context-Aware Cerdas & Posisi MORE di Bawah) ---
std::vector<MenuSlice> BuildContextSlices(const std::wstring& ext, const std::vector<MenuSlice>& pool) {
    std::wstring cleanExt = (!ext.empty() && ext[0] == L'.') ? ext.substr(1) : ext;
    if (cleanExt == L"jpeg") cleanExt = L"jpg";

    std::vector<MenuSlice> result;
    for (const auto& item : pool) {
        if (item.action == cleanExt) continue;
        result.push_back(item);
        if (result.size() == 4) break;
    }

    if (result.size() >= 2) {
        result.insert(result.begin() + 2, {L"MORE", L"__MORE__"});
    } else {
        result.push_back({L"MORE", L"__MORE__"});
    }
    return result;
}

void LoadCategoryMode(bool canGoBack) {
    g_isCategoryMode = true;
    g_canGoBack = canGoBack;
    g_centerLabel = canGoBack ? L"KEMBALI" : L"BATAL";
    g_activeCategoryIdx = -1;
    g_hoveredCenter = false;
    g_hoveredInner = -1;
    g_hoveredOuter = -1;
    g_outerSlices.clear();
    g_innerSlices = {
        {L"VIDEO", L"__CAT_VIDEO__"},
        {L"AUDIO", L"__CAT_AUDIO__"},
        {L"IMAGE", L"__CAT_IMAGE__"},
        {L"DOC",   L"__CAT_DOC__"}
    };
}

void LoadOuterForCategory(const std::wstring& catAction) {
    g_outerSlices.clear();
    if (catAction == L"__CAT_VIDEO__") {
        g_outerSlices = {{L"MP4", L"mp4"}, {L"MKV", L"mkv"}, {L"MP3", L"mp3"}, {L"GIF", L"gif"}, {L"WEBM", L"webm"}};
    } else if (catAction == L"__CAT_AUDIO__") {
        g_outerSlices = {{L"MP3", L"mp3"}, {L"WAV", L"wav"}, {L"FLAC", L"flac"}, {L"OGG", L"ogg"}, {L"M4A", L"m4a"}};
    } else if (catAction == L"__CAT_IMAGE__") {
        g_outerSlices = {{L"PNG", L"png"}, {L"JPG", L"jpg"}, {L"WEBP", L"webp"}, {L"ICO", L"ico"}, {L"PDF", L"pdf"}};
    } else if (catAction == L"__CAT_DOC__") {
        g_outerSlices = {{L"PDF", L"pdf"}, {L"DOCX", L"docx"}, {L"TXT", L"txt"}, {L"MD", L"md"}};
    }
}

void SetupMenuForExtension(const std::wstring& ext) {
    g_hoveredCenter = false;
    g_hoveredInner = -1;
    g_hoveredOuter = -1;
    g_activeCategoryIdx = -1;
    g_outerSlices.clear();
    g_canGoBack = false;
    g_centerLabel = L"BATAL";

    if (ext == L".mp4" || ext == L".mkv" || ext == L".mov" || ext == L".avi" || ext == L".webm") {
        g_isCategoryMode = false;
        g_innerSlices = BuildContextSlices(ext, {
            {L"MP3", L"mp3"}, {L"MKV", L"mkv"}, {L"GIF", L"gif"}, {L"MP4", L"mp4"}, {L"WEBM", L"webm"}
        });
    } else if (ext == L".mp3" || ext == L".wav" || ext == L".flac" || ext == L".ogg" || ext == L".m4a") {
        g_isCategoryMode = false;
        g_innerSlices = BuildContextSlices(ext, {
            {L"MP3", L"mp3"}, {L"WAV", L"wav"}, {L"OGG", L"ogg"}, {L"FLAC", L"flac"}, {L"M4A", L"m4a"}
        });
    } else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp") {
        g_isCategoryMode = false;
        g_innerSlices = BuildContextSlices(ext, {
            {L"JPG", L"jpg"}, {L"PNG", L"png"}, {L"WEBP", L"webp"}, {L"ICO", L"ico"}, {L"PDF", L"pdf"}
        });
    } else if (ext == L".docx" || ext == L".md" || ext == L".txt" || ext == L".pdf") {
        g_isCategoryMode = false;
        g_innerSlices = BuildContextSlices(ext, {
            {L"PDF", L"pdf"}, {L"DOCX", L"docx"}, {L"TXT", L"txt"}, {L"MD", L"md"}, {L"HTML", L"html"}
        });
    } else {
        LoadCategoryMode(false);
    }
}

// --- Rendering GDI+ ke Layered Window ---
void DrawRing(Gdiplus::Graphics& g, const std::vector<MenuSlice>& slices,
              float r1, float r2, int hoveredIdx, int activeIdx, bool isOuter) {
    if (slices.empty()) return;

    const int n = static_cast<int>(slices.size());
    const float sweep = 360.0f / n;
    const float gap = 2.0f;

    Gdiplus::FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font font(&fontFamily, isOuter ? 10.5f : 11.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    for (int i = 0; i < n; ++i) {
        float startAngle = -90.0f + i * sweep + (gap / 2.0f);
        float sliceSweep = sweep - gap;

        Gdiplus::GraphicsPath path;
        path.AddArc(CENTER_XY - r2, CENTER_XY - r2, r2 * 2, r2 * 2, startAngle, sliceSweep);
        path.AddArc(CENTER_XY - r1, CENTER_XY - r1, r1 * 2, r1 * 2, startAngle + sliceSweep, -sliceSweep);
        path.CloseFigure();

        Gdiplus::Color fillColor(235, 32, 34, 38);
        if (i == hoveredIdx) {
            fillColor = Gdiplus::Color(250, 56, 132, 244);
        } else if (i == activeIdx) {
            fillColor = Gdiplus::Color(245, 52, 58, 70);
        } else if (isOuter) {
            fillColor = Gdiplus::Color(235, 42, 45, 52);
        }

        Gdiplus::SolidBrush brush(fillColor);
        g.FillPath(&brush, &path);

        double midRad = (startAngle + sliceSweep / 2.0f) * PI / 180.0;
        float textR = (r1 + r2) / 2.0f;
        float tx = CENTER_XY + static_cast<float>(std::cos(midRad)) * textR;
        float ty = CENTER_XY + static_cast<float>(std::sin(midRad)) * textR;

        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 245, 245, 245));
        Gdiplus::RectF textRect(tx - 40.0f, ty - 16.0f, 80.0f, 32.0f);
        g.DrawString(slices[i].label.c_str(), -1, &font, textRect, &sf, &textBrush);
    }
}

void RenderRadialMenu() {
    if (!g_hwnd) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = WIN_SIZE;
    bmi.bmiHeader.biHeight = -WIN_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    HBITMAP hOldBitmap = static_cast<HBITMAP>(SelectObject(hdcMem, hBitmap));

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        DrawRing(g, g_innerSlices, INNER_R1, INNER_R2, g_hoveredInner, g_activeCategoryIdx, false);
        DrawRing(g, g_outerSlices, OUTER_R1, OUTER_R2, g_hoveredOuter, -1, true);

        Gdiplus::Color centerColor(240, 22, 24, 28);
        if (g_hoveredCenter) {
            centerColor = g_canGoBack
                ? Gdiplus::Color(250, 56, 132, 244)
                : Gdiplus::Color(250, 185, 55, 65);
        }
        Gdiplus::SolidBrush centerBrush(centerColor);
        g.FillEllipse(&centerBrush, CENTER_XY - CENTER_R, CENTER_XY - CENTER_R, CENTER_R * 2, CENTER_R * 2);

        Gdiplus::FontFamily fontFamily(L"Segoe UI");
        Gdiplus::Font centerFont(&fontFamily, 8.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush centerTextBrush(
            g_hoveredCenter ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(220, 195, 200, 210)
        );
        Gdiplus::RectF centerRect(CENTER_XY - CENTER_R, CENTER_XY - CENTER_R, CENTER_R * 2, CENTER_R * 2);
        g.DrawString(g_centerLabel.c_str(), -1, &centerFont, centerRect, &sf, &centerTextBrush);
    }

    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {WIN_SIZE, WIN_SIZE};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = g_currentAlpha;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(g_hwnd, hdcScreen, NULL, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void HideMenuSoft() {
    KillTimer(g_hwnd, TIMER_FADE_IN);
    g_isMenuVisible = false;
    g_hoveredCenter = false;
    g_hoveredInner = -1;
    g_hoveredOuter = -1;
    g_currentAlpha = 0;
    ShowWindow(g_hwnd, SW_HIDE);
}

// --- Hit-Testing Kursor ---
int GetSliceIndexAtAngle(double angleDeg, int sliceCount) {
    if (sliceCount <= 0) return -1;
    double shifted = std::fmod(angleDeg + 90.0 + 360.0, 360.0);
    double sweep = 360.0 / sliceCount;
    int idx = static_cast<int>(shifted / sweep);
    return (idx >= 0 && idx < sliceCount) ? idx : -1;
}

void UpdateHover(int mx, int my) {
    double dx = mx - CENTER_XY;
    double dy = my - CENTER_XY;
    double dist = std::hypot(dx, dy);
    double angleDeg = std::atan2(dy, dx) * 180.0 / PI;

    bool newCenter = false;
    int newInner = -1;
    int newOuter = -1;

    if (dist <= CENTER_R) {
        newCenter = true;
    } else if (dist >= INNER_R1 && dist <= INNER_R2) {
        newInner = GetSliceIndexAtAngle(angleDeg, static_cast<int>(g_innerSlices.size()));
    } else if (!g_outerSlices.empty() && dist >= OUTER_R1 && dist <= OUTER_R2) {
        newOuter = GetSliceIndexAtAngle(angleDeg, static_cast<int>(g_outerSlices.size()));
    }

    bool needsRedraw = (newCenter != g_hoveredCenter || newInner != g_hoveredInner || newOuter != g_hoveredOuter);
    g_hoveredCenter = newCenter;
    g_hoveredInner = newInner;
    g_hoveredOuter = newOuter;

    if (g_isCategoryMode && g_hoveredInner != -1 && g_hoveredInner != g_activeCategoryIdx) {
        g_activeCategoryIdx = g_hoveredInner;
        LoadOuterForCategory(g_innerSlices[g_activeCategoryIdx].action);
        needsRedraw = true;
    }

    if (needsRedraw) {
        RenderRadialMenu();
    }
}

void StartConversion(const std::wstring& targetFormat) {
    std::vector<std::wstring> filesCopy = g_currentFiles;
    std::thread(ExecuteConversionAsync, std::move(filesCopy), targetFormat).detach();
}

// --- Window Procedure ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_SHOW_MENU: {
            ShowRequest req;
            {
                std::lock_guard<std::mutex> lock(g_reqMutex);
                req = g_pendingReq;
            }

            g_currentFiles = req.files;
            g_currentPrimaryExt = req.primaryExt;
            SetupMenuForExtension(g_currentPrimaryExt);

            int winX = req.x - (WIN_SIZE / 2);
            int winY = req.y - (WIN_SIZE / 2);

            POINT pt = {req.x, req.y};
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                if (winX < mi.rcWork.left) winX = mi.rcWork.left;
                if (winY < mi.rcWork.top) winY = mi.rcWork.top;
                if (winX + WIN_SIZE > mi.rcWork.right) winX = mi.rcWork.right - WIN_SIZE;
                if (winY + WIN_SIZE > mi.rcWork.bottom) winY = mi.rcWork.bottom - WIN_SIZE;
            }

            g_currentAlpha = 25;
            g_isMenuVisible = true;
            SetWindowPos(hwnd, HWND_TOPMOST, winX, winY, WIN_SIZE, WIN_SIZE, SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd);
            RenderRadialMenu();

            SetTimer(hwnd, TIMER_FADE_IN, 16, NULL);
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_FADE_IN) {
                if (g_currentAlpha < 255) {
                    int nextAlpha = static_cast<int>(g_currentAlpha) + 46;
                    g_currentAlpha = (nextAlpha >= 255) ? 255 : static_cast<BYTE>(nextAlpha);
                    RenderRadialMenu();
                }
                if (g_currentAlpha >= 255) {
                    KillTimer(hwnd, TIMER_FADE_IN);
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            UpdateHover(mx, my);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            g_lastActivityTick.store(GetTickCount64());
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            UpdateHover(mx, my);

            if (g_hoveredCenter) {
                if (g_isCategoryMode && g_canGoBack) {
                    SetupMenuForExtension(g_currentPrimaryExt);
                    RenderRadialMenu();
                } else {
                    HideMenuSoft();
                }
            } else if (g_hoveredOuter != -1) {
                std::wstring format = g_outerSlices[g_hoveredOuter].action;
                HideMenuSoft();
                StartConversion(format);
            } else if (g_hoveredInner != -1) {
                std::wstring action = g_innerSlices[g_hoveredInner].action;
                if (action == L"__MORE__") {
                    LoadCategoryMode(true);
                    RenderRadialMenu();
                } else if (action.rfind(L"__CAT_", 0) == 0) {
                    g_activeCategoryIdx = g_hoveredInner;
                    LoadOuterForCategory(action);
                    RenderRadialMenu();
                } else {
                    HideMenuSoft();
                    StartConversion(action);
                }
            } else {
                HideMenuSoft();
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                HideMenuSoft();
            }
            return 0;
        }

        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE && g_isMenuVisible) {
                HideMenuSoft();
            }
            return 0;
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// --- Background Thread Named Pipe ---
void PipeServerThread() {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    std::wcout << L"[CONVERTER] Siap.\n";

    while (true) {
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0, 512, 0, NULL
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(200);
            continue;
        }

        if (ConnectNamedPipe(hPipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buffer[512] = {0};
            DWORD bytesRead = 0;

            if (ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
                g_lastActivityTick.store(GetTickCount64());
                std::string command(buffer, bytesRead);

                if (command == "FOCUS") {
                    std::wcout << L"[CONVERTER] Timer idle reset.\n";
                } else if (command.rfind("SHOW ", 0) == 0) {
                    std::istringstream iss(command);
                    std::string tag;
                    int x = 0, y = 0;
                    uintptr_t hwndVal = 0;
                    iss >> tag >> x >> y >> hwndVal;

                    HWND targetHwnd = reinterpret_cast<HWND>(hwndVal);
                    auto files = GetSelectedExplorerFiles(targetHwnd);
                    std::wstring ext = files.empty() ? L"" : GetFileExtension(files[0]);

                    {
                        std::lock_guard<std::mutex> lock(g_reqMutex);
                        g_pendingReq.x = x;
                        g_pendingReq.y = y;
                        g_pendingReq.files = std::move(files);
                        g_pendingReq.primaryExt = ext;
                    }

                    if (g_hwnd) {
                        PostMessageW(g_hwnd, WM_APP_SHOW_MENU, 0, 0);
                    }
                }
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    CoUninitialize();
}

int main() {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, CONVERTER_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    const wchar_t* CLASS_NAME = L"OrbisRadialMenuClass";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CLASS_NAME, L"Orbis", WS_POPUP,
        0, 0, WIN_SIZE, WIN_SIZE,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    g_lastActivityTick.store(GetTickCount64());
    std::thread(IdleMonitorThread).detach();
    std::thread(PipeServerThread).detach();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CloseHandle(hMutex);
    return 0;
}