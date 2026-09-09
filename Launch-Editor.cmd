@echo off
setlocal
if exist "%LOCALAPPDATA%\Programs\w64devkit\bin\g++.exe" set "PATH=%LOCALAPPDATA%\Programs\w64devkit\bin;%PATH%"
if exist "%~dp0build\m8-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m8-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m8-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m8-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m7-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m7-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m7-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m7-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m6-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m6-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m6-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m6-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m5-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m5-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m5-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m5-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m4-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m4-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m4-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m4-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m3-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m3-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m3-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m3-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m2-release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m2-release\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\m2-debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\m2-debug\bin\ProtoEditor.exe" %*
) else if exist "%~dp0build\release\bin\ProtoEditor.exe" (
    start "" "%~dp0build\release\bin\ProtoEditor.exe"
) else if exist "%~dp0build\debug\bin\ProtoEditor.exe" (
    start "" "%~dp0build\debug\bin\ProtoEditor.exe"
) else (
    echo Build first: powershell -ExecutionPolicy Bypass -File scripts\Build.ps1 -Configuration m8-release
    pause
)
