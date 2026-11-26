@echo off
if not exist build mkdir build

echo [1/2] Compiling OrbisWatcher.exe...
g++ -O2 -std=c++17 src/watcher/main.cpp -o build/OrbisWatcher.exe -luser32 -lpsapi
if %errorlevel% neq 0 goto :error

echo [2/2] Compiling OrbisConverter.exe...
g++ -O2 -std=c++17 src/converter/main.cpp -o build/OrbisConverter.exe -luser32 -lgdi32 -lgdiplus -lole32 -loleaut32 -luuid -lshell32
if %errorlevel% neq 0 goto :error

echo [SUCCESS] OrbisWatcher.exe dan OrbisConverter.exe berhasil dibuat di folder build/
pause
exit /b 0

:error
echo [ERROR] Kompilasi gagal! Periksa pesan error di atas.
pause
exit /b %errorlevel%