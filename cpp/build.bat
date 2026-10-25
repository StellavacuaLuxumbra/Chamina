@echo off
rem Build chamina-cpp — the standalone C++ engine.
rem
rem Prereqs: MinGW-w64 g++ on PATH (or set GXX to its full path).
rem   Everything else (json / stb_truetype / earcut / stb_image_write) is
rem   vendored in cpp\vendor — no package manager involved.
rem
rem Output: dist\chamina-cpp.exe (statically linked, no runtime DLLs).

setlocal
cd /d "%~dp0\.."

rem Prefer the known-good MinGW-w64 on this machine; fall back to PATH g++.
if "%GXX%"=="" (
  if exist D:\mingw64\bin\g++.exe (set GXX=D:\mingw64\bin\g++.exe) else (set GXX=g++)
)

if not exist dist mkdir dist

echo [build] %GXX% -O2 -std=c++17 -fopenmp -static -Icpp/vendor cpp/src/chamina.cpp
%GXX% -O2 -std=c++17 -fopenmp -static -static-libgcc -static-libstdc++ ^
  -Icpp/vendor cpp/src/chamina.cpp -o dist/chamina-cpp.exe || exit /b 1

echo [build] ok -^> dist\chamina-cpp.exe
dist\chamina-cpp.exe 2>&1 | findstr /C:"usage" >nul || (echo [build] smoke test failed & exit /b 1)
echo [build] smoke ok
