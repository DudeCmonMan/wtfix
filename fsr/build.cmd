@echo off
rem Builds wtfsr.dll (AMD FSR 3.1 behind wtdlss.dll's interface) with MSVC, AMD's SDK's compiler. Usage: build.cmd <out dir> <FSR SDK>
setlocal
set HERE=%~dp0
set OUT=%~1
set SDK=%~2
set S=%SDK%\sdk
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (echo wtfsr: no Visual Studio & exit /b 1)
for /f "usebackq tokens=*" %%i in (`call "%%VSWHERE%%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo wtfsr: no MSVC x64 tools & exit /b 1)
if not exist "%S%\include\FidelityFX\host\ffx_fsr3upscaler.h" (echo wtfsr: no FSR SDK in %SDK% & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
python -I "%HERE%shaders.py" "%SDK%" "%OUT%\fsr_shaders" || exit /b 1
python -I "%HERE%patch_backend.py" "%SDK%" "%OUT%\fsr_backend" || exit /b 1
if not exist "%OUT%\obj\fsr" mkdir "%OUT%\obj\fsr"
del /q "%OUT%\obj\fsr\*.obj" 2>nul
set INC=/I"%S%\include" /I"%S%\src\backends\shared" /I"%S%\src\components" /I"%S%\src\shared" /I"%OUT%\fsr_shaders"
rem FFX_GCC only turns FidelityFX's dllexport off: its functions stay inside wtfsr.dll.
cl /nologo /c /O2 /MT /EHsc /std:c++17 /W3 /DFFX_GCC /D_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING %INC% ^
  "%S%\src\components\fsr3upscaler\ffx_fsr3upscaler.cpp" ^
  "%OUT%\fsr_backend\ffx_dx11.cpp" "%S%\src\backends\shared\blob_accessors\ffx_fsr3upscaler_shaderblobs.cpp" ^
  "%S%\src\shared\ffx_object_management.cpp" "%S%\src\shared\ffx_assert.cpp" /Fo"%OUT%\obj\fsr/" || exit /b 1
cl /nologo /c /O2 /MT /W3 "%S%\src\backends\dx11\DXBCChecksum.c" "%S%\src\backends\dx11\md5.c" /Fo"%OUT%\obj\fsr/" || exit /b 1
cl /nologo /c /O2 /MT /EHsc /std:c++17 /W4 /WX /DFFX_GCC /external:W0 /external:I"%S%\include" ^
  /external:I"%S%\src\backends\shared" /I"%HERE%..\dlss" "%HERE%wtfsr.cpp" "%HERE%shaderblobs.cpp" /Fo"%OUT%\obj\fsr/" || exit /b 1
link /nologo /DLL /OUT:"%OUT%\wtfsr.dll" /IMPLIB:"%OUT%\obj\fsr\wtfsr.lib" "%OUT%\obj\fsr\*.obj" d3d11.lib dxguid.lib || exit /b 1
