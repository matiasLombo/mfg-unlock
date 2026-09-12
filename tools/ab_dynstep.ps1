# A/B de la grilla de DYNAMIC (dynstep) en Cyberpunk, lanzado DIRECTO (sin el
# overlay de Steam: con el overlay no hay hook de Present y no existen las
# metricas de cadencia). Tres corridas por lado, alternadas para que el orden
# no sea la variable. Cada log se guarda como mfg-unlock.dyn-<lado><n>.log.
#
#   tools/ab_dynstep.ps1            # A = dynstep 50 (defecto), B = dynstep 0
#   tools/ab_dynstep.ps1 -B 25      # A contra 25
param([int]$A = 50, [int]$B = 0, [int]$Rondas = 3)
$dir = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
$cfg = Join-Path $dir "mfg-config.txt"
$base = Get-Content $cfg | Where-Object { $_ -notmatch '^dynstep' }
for ($r = 1; $r -le $Rondas; $r++) {
  foreach ($lado in @(@("A", $A), @("B", $B))) {
    $et = "dyn-$($lado[0])$r"
    Set-Content -Path $cfg -Value ($base + @("dynstep $($lado[1])"))
    Write-Output "== $et (dynstep $($lado[1]))"
    & "$PSScriptRoot\run_cp_focus.ps1" -Etiqueta $et | Select-Object -Last 2
    Copy-Item (Join-Path $dir "mfg-unlock.log") (Join-Path $dir "mfg-unlock.$et.log") -Force
    & "C:\Python313\python.exe" "$PSScriptRoot\ventanas.py" (Join-Path $dir "mfg-unlock.$et.log") | Select-Object -Last 2
  }
}
Set-Content -Path $cfg -Value $base
