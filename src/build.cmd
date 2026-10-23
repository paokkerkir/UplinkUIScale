@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist ..\build mkdir ..\build
cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /LD UplinkUIScale.cpp /Fo..\build\ /Fe..\build\version.dll /link /DLL /DEF:version.def user32.lib opengl32.lib || exit /b 1
del /q ..\build\version.exp ..\build\version.lib ..\build\*.obj 2>nul
copy /y UplinkUIScale.ini ..\build\ >nul
echo Built ..\build\version.dll
