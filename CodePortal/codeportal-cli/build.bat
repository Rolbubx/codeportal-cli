@echo off
rem Compilation directe avec le MinGW fourni par CLion (sans passer par CMake).
set GXX="E:\CLion 2026.2.2\bin\mingw\bin\g++.exe"
if not exist "%~dp0build" mkdir "%~dp0build"
%GXX% -std=gnu++17 -O2 -Wall -Wextra -pthread "%~dp0src\main.cpp" -o "%~dp0build\codeportal.exe" -static-libgcc -static-libstdc++ -lwinhttp -lshell32
if %ERRORLEVEL% neq 0 (
    echo Echec de la compilation.
    exit /b 1
)
copy /Y "E:\CLion 2026.2.2\bin\mingw\bin\libwinpthread-1.dll" "%~dp0build\" >nul
echo Compilation OK : %~dp0build\codeportal.exe
