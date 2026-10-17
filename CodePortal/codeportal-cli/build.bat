@echo off
rem Compilation directe avec le MinGW fourni par CLion (sans passer par CMake).
rem Résultat : build\codeportal.exe, autonome (lié en statique, aucune DLL à copier).
set MINGW=E:\CLion 2026.2.2\bin\mingw\bin
if not exist "%~dp0build" mkdir "%~dp0build"
"%MINGW%\windres.exe" "%~dp0res\codeportal.rc" -O coff -o "%~dp0build\codeportal.res.o"
if %ERRORLEVEL% neq 0 goto :error
"%MINGW%\g++.exe" -std=gnu++17 -O2 -Wall -Wextra -pthread "%~dp0src\main.cpp" "%~dp0build\codeportal.res.o" -o "%~dp0build\codeportal.exe" -static -lwinhttp -lshell32 -lws2_32 -lbcrypt
if %ERRORLEVEL% neq 0 goto :error
del "%~dp0build\codeportal.res.o"
echo Compilation OK : %~dp0build\codeportal.exe
exit /b 0

:error
echo Echec de la compilation.
exit /b 1
