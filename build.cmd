@echo off
rem 一键编译 Xiaomi 14 Pro (shennong / SM8650) 的 UEFI —— Windows 入口，转交 WSL。
rem 用法: build.cmd [DEBUG|RELEASE]
setlocal
for /f "delims=" %%i in ('wsl.exe wslpath -a "%CD%"') do set "WDIR=%%i"
wsl.exe -e bash -lc "cd '%WDIR%' && ./build.sh %*"
