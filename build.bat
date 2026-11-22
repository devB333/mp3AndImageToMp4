@echo off
rem Builds mp4Maker.exe with clang++ (LLVM). Run from any folder.
setlocal
cd /d "%~dp0"

pushd bin-int

llvm-rc /fo mp4Maker.res ..\mp4Maker.rc || goto :failed

clang++ -std=c++17 -O2 -Wall -Wextra ..\mp4Maker.cpp mp4Maker.res -o mp4Maker.exe ^
    -fuse-ld=lld -Wl,/SUBSYSTEM:WINDOWS -Wl,/MANIFEST:NO ^
    -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32 || goto :failed


popd 

move /Y .\bin-int\mp4Maker.exe .\bin\ || goto :failed

echo Built mp4Maker.exe
exit /b 0

:failed
echo Build failed.
exit /b 1
