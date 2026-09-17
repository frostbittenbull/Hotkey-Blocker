@echo off
windres src\app.rc -O coff -o src\app_res.o
if errorlevel 1 goto :error

g++ -std=c++17 -O2 -municode -mwindows src\main.cpp src\app_res.o -o HotkeyBlocker.exe ^
    -lcomctl32 -ldwmapi -lshell32 -lgdi32 -static -static-libgcc -static-libstdc++
if errorlevel 1 goto :error

echo.
echo Готово: HotkeyBlocker.exe
goto :eof

:error
echo.
echo Сборка не удалась. Проверьте, что g++ (MinGW-w64) установлен и добавлен в PATH.
exit /b 1