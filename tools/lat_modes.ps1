# Latencia por modo fijo en Cyberpunk: una corrida del benchmark por cada
# modo, misma escena, para comparar sin confundir escena con modo. La latencia
# sale del reporte de Reflex (slReflexGetState), que no depende del hook de
# Present, asi que anda lanzado por Steam.
#
#   tools/lat_modes.ps1                 # modos 2 3 4 6
#   tools/lat_modes.ps1 -Modos 2,4,6
param([int[]]$Modos = @(2, 3, 4, 6))
$dir = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
$set = Join-Path $dir "mfg-settings.txt"
$orig = Get-Content $set
foreach ($m in $Modos) {
  $et = "lat-m$m"
  Set-Content -Path $set -Value @("mode $m", "target 588", "dynfps 180", "hud 1")
  Write-Output "== $et"
  & "$PSScriptRoot\run_cp_focus.ps1" -Etiqueta $et -Steam | Select-Object -Last 2
  Copy-Item (Join-Path $dir "mfg-unlock.log") (Join-Path $dir "mfg-unlock.$et.log") -Force
}
Set-Content -Path $set -Value $orig
Write-Output "listo; logs mfg-unlock.lat-mN.log"
