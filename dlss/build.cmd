@echo off
rem Builds wtdlss.dll with MSVC: NVIDIA's NGX library needs its static runtime. Usage: build.cmd <out dir> <DLSS SDK>
setlocal
set HERE=%~dp0
set OUT=%~1
set SDK=%~2
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo wtdlss: no Visual Studio & exit /b 1)
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo wtdlss: no MSVC x64 tools & exit /b 1)
if not exist "%SDK%\include\nvsdk_ngx.h" (echo wtdlss: no DLSS SDK in %SDK% & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /MT /EHsc /W4 /WX /LD /I"%SDK%\include" "%HERE%wtdlss.cpp" /Fo"%OUT%\obj\wtdlss.obj" /Fe"%OUT%\wtdlss.dll" ^
  /link /NOLOGO /IMPLIB:"%OUT%\obj\wtdlss.lib" "%SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" d3d11.lib advapi32.lib user32.lib || exit /b 1
