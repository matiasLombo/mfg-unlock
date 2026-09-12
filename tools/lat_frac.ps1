# Latencia: fraccionario fijo contra los enteros vecinos, en Cyberpunk. Mode 8
# (DYNAMIC) con dynpin, que fija el ratio y apaga el controlador -- 5.00, 5.50
# y 6.00 pasan TODOS por el planificador fraccional, asi que la unica variable
# es la fraccion (5.50 alterna 5/6; 5.00 y 6.00 no alternan). Una corrida por
# ratio, misma escena. La latencia sale de Reflex, que no depende del hook de
# Present.
#
#   tools/lat_frac.ps1                  # 500 550 600
param([int[]]$Pins = @(500, 550, 600))
$dir = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
$set = Join-Path $dir "mfg-settings.txt"
$cfg = Join-Path $dir "mfg-config.txt"
$oset = Get-Content $set
$ocfg = Get-Content $cfg
$base = @($ocfg | Where-Object { $_ -notmatch '^dynpin' })   # @() para que no concatene como string
foreach ($p in $Pins) {
  $et = "lat-pin$p"
  Set-Content -Path $set -Value @("mode 8", "target 588", "dynfps 180", "hud 1")
  Set-Content -Path $cfg -Value ($base + @("dynpin $p"))
  Write-Output "== $et (dynpin $p)"
  & "$PSScriptRoot\run_cp_focus.ps1" -Etiqueta $et -Steam | Select-Object -Last 2
  Copy-Item (Join-Path $dir "mfg-unlock.log") (Join-Path $dir "mfg-unlock.$et.log") -Force
}
Set-Content -Path $set -Value $oset
Set-Content -Path $cfg -Value $ocfg
Write-Output "listo; logs mfg-unlock.lat-pinN.log"
