@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /LD /EHsc xinput_proxy.cpp /link /DEF:xinput_proxy.def /OUT:xinput1_4.dll user32.lib || exit /b 1
del xinput_proxy.obj xinput_proxy.exp xinput_proxy.lib 2>nul
