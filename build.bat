@echo off
set "GCC_BIN=C:\Users\prabh\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
"%GCC_BIN%\g++.exe" -std=c++20 -O2 -static -static-libgcc -static-libstdc++ -Iinclude %* -lws2_32
