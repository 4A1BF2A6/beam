@echo off
setlocal
cd /d %~dp0

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo ERROR: VS BuildTools not found at %VCVARS%
    exit /b 1
)
call %VCVARS% > nul 2>&1

if not exist build mkdir build

echo Compiling ds_bf.dll (shared library for Python ctypes)...
rem 中间件 (.obj) 写到 build\，DLL 留在顶层；.lib/.exp 也走 build\
cl.exe /nologo /std:c17 /O2 /W3 /utf-8 /I. /MT /LD ^
    /Fobuild\ ^
    ds_beamforming.c speex\fft\fft_wrap.c ^
    /Fe:ds_bf.dll ^
    /link /DEF:ds_bf.def /IMPLIB:build\ds_bf.lib
if %ERRORLEVEL% neq 0 ( echo Build FAILED & exit /b 1 )

echo.
echo Build OK: ds_bf.dll  (intermediates -^> build\)
dir /b ds_bf.dll
dir /b build\ds_bf.lib build\ds_bf.exp 2>nul

endlocal
