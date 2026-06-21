@echo off
set EDITOR=Z:\UnrealEngineSource_5_7\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=Z:\UnrealEngineNetworkedProj\NetworkedPhysics_20_06_2026\Sandbox.uproject
"%EDITOR%" "%PROJECT%" /Game/Map1 -game -ExecCmds="car.SelfTest 2" -unattended -nopause -nosplash -noxgeshadercompile -log -nullrhi
