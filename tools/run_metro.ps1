# Lanza Metro Exodus Enhanced Edition por Steam sin pasos a mano.
#  - BadQuit 0 en HKCU\Software\4A-Games\Metro Exodus: si la corrida anterior
#    se mato (tope de segundos), el juego pregunta "start in safe mode?" y se
#    queda ahi esperando a alguien.
#  - La cinematica de apertura dura ~90-120 s y NO se saltea: probado Escape,
#    Space y Enter, sueltos (SendKeys) y sostenidos 200 ms (keybd_event), y
#    apartar legal.webm/intro.webm (la cinematica viene de los .vfs). El tope
#    tiene que absorberla: con -Max 200 quedan ~80 s de menu, que renderiza
#    con DLSS y alcanza para el modo host.
#  - -benchmark NO corre ningun benchmark en la EE (el benchmark real es
#    Benchmark.exe, aparte); el juego queda en el menu.
#  - Al tope de segundos se mata el proceso (el juego no sale solo).
#
#   tools/run_metro.ps1 -Max 200
param([int]$Max = 200)

$k = "HKCU:\Software\4A-Games\Metro Exodus"
if (Test-Path $k) { Set-ItemProperty -Path $k -Name BadQuit -Value 0 -Type DWord }
& "$PSScriptRoot\run_steam.ps1" -AppId 1449560 -Proc MetroExodus -Args "-benchmark" -Max $Max
