@echo off
setlocal
set EDITOR=Z:\UnrealEngineSource_5_7\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=Z:\UnrealEngineNetworkedProj\NetworkedPhysics_20_06_2026\Sandbox.uproject
set TS=%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%
set TS=%TS: =0%
set LOGS=Z:\UnrealEngineNetworkedProj\NetworkedPhysics_20_06_2026\Saved\Logs
set NETS=%LOGS%\NetS_%TS%.log
set NETC=%LOGS%\NetC_%TS%.log

taskkill /F /IM UnrealEditor.exe >nul 2>&1
ping 127.0.0.1 -n 3 >nul

echo Starting dedicated server (log: %NETS%)...
start "NetServer" /B "%EDITOR%" "%PROJECT%" /Game/Lvl_ModularCarTest -server -nullrhi -ExecCmds="Net PktLag=90, car.SelfTest 6" -unattended -nopause -nosplash -noxgeshadercompile -abslog="%NETS%" -port=7777

echo Waiting 18s for server...
ping 127.0.0.1 -n 19 >nul

echo Starting client (log: %NETC%)...
"%EDITOR%" "%PROJECT%" 127.0.0.1:7777 -game -nullrhi -ExecCmds="Net PktLag=90, car.SelfTest 6" -unattended -nopause -nosplash -noxgeshadercompile -abslog="%NETC%"
set CLIENT_EXIT=%ERRORLEVEL%

echo Client exit code: %CLIENT_EXIT%
if exist "%NETC%" (
  findstr /C:"SELFTEST NETCOLLISION" "%NETC%"
) else (
  echo ERROR: client log not found: %NETC%
  set CLIENT_EXIT=1
)
exit /b %CLIENT_EXIT%