@echo off
if not exist build mkdir build

echo [1/1] Compiling OrbisWatcher.exe...
g++ -O2 -std=c++17 src/watcher/main.cpp -o build/OrbisWatcher.exe -luser32 -lpsapi

if %errorlevel% neq 0 (
    echo [ERROR] Kompilasi gagal! Periksa pesan error di atas.
    pause
    exit /b %errorlevel%
)

echo [SUCCESS] OrbisWatcher.exe berhasil dibuat di folder build/
pause