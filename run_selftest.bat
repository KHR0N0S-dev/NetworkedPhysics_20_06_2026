@echo off
setlocal
set EDITOR=Z:\UnrealEngineSource_5_7\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=Z:\UnrealEngineNetworkedProj\NetworkedPhysics_20_06_2026\Sandbox.uproject
set MODE=%1
if "%MODE%"=="" set MODE=2

"%EDITOR%" "%PROJECT%" /Game/Lvl_ModularCarTest -game -ExecCmds="car.SelfTest %MODE%" -unattended -nopause -nosplash -noxgeshadercompile -log -nullrhi
exit /b %ERRORLEVEL%