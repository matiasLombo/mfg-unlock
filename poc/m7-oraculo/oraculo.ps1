# M7: valida el oraculo externo contra una verdad conocida.
#
# HAY QUE CORRERLO ELEVADO. PresentMon abre una sesion ETW en tiempo real, y eso
# solo lo pueden hacer procesos elevados, el grupo "Performance Log Users" o
# servicios del sistema (Microsoft Learn, "About Event Tracing"). Sin elevar sale
# con codigo 1, sin stdout ni stderr y sin crear el CSV -- medido asi el
# 2026-09-08.
#
# El script hace todo adentro de UNA sola elevacion:
#   1. lanza presentador.exe, que presenta una cantidad conocida y la imprime
#   2. corre PresentMon apuntandole solo a ese proceso
#   3. compara los dos numeros
#
# La primera vez fallo por no tener nada que capturar: en el escritorio quieto
# PresentMon no escribe CSV. Por eso el presentador va primero.
#
# Tambien usa --session_name propio: ya hay un PresentMon de NVIDIA corriendo en
# esta maquina (FrameView) y con el nombre por defecto la sesion colisiona y el
# proceso se cuelga sin escribir.
param([int]$Segundos = 20)

$aqui = $PSScriptRoot
$pm = "C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\PresentMon_x64.exe"
$pres = Join-Path $aqui "presentador.exe"
$csv = Join-Path $aqui "captura.csv"
$salida = Join-Path $aqui "presentador-salida.txt"

$elevado = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Output "elevado: $elevado"
if (-not $elevado) { Write-Output "ABORTA: hay que correrlo elevado."; exit 1 }
foreach ($f in @($pm, $pres)) { if (-not (Test-Path $f)) { Write-Output "falta $f"; exit 2 } }
foreach ($f in @($csv, $salida)) { if (Test-Path $f) { Remove-Item $f -Force } }

# El presentador arranca primero y vive un poco mas que la captura, para que
# PresentMon tenga frames desde el primer instante hasta el ultimo.
$p = Start-Process -FilePath $pres -ArgumentList "$($Segundos + 6)" -PassThru `
                   -RedirectStandardOutput $salida -WindowStyle Normal
Start-Sleep -Seconds 3

Write-Output "capturando $Segundos s con PresentMon $((Get-Item $pm).VersionInfo.FileVersion)"
$args = @("--timed", "$Segundos", "--process_name", "presentador.exe",
          "--output_file", $csv, "--no_console_stats",
          "--session_name", "mfgpoc", "--stop_existing_session",
          "--terminate_after_timed", "--qpc_time")
$q = Start-Process -FilePath $pm -ArgumentList $args -Wait -PassThru -NoNewWindow
Write-Output "PresentMon exit: $($q.ExitCode)"

$p.WaitForExit(60000) | Out-Null
$linea = Get-Content $salida -ErrorAction SilentlyContinue | Where-Object { $_ -match "PRESENTADOR" }
Write-Output "verdad del presentador: $linea"

if (-not (Test-Path $csv)) { Write-Output "SIN CSV: el oraculo no capturo nada"; exit 1 }
$filas = @(Import-Csv $csv)
Write-Output "filas en el CSV: $($filas.Count)"
if ($filas.Count -lt 5) { Write-Output "muy pocas filas para comparar"; exit 1 }

# fps del oraculo: filas sobre el lapso que cubren.
$t0 = [double]$filas[0].TimeInSeconds
$t1 = [double]$filas[-1].TimeInSeconds
$dur = $t1 - $t0
$fpsOraculo = if ($dur -gt 0) { ($filas.Count - 1) / $dur } else { 0 }
Write-Output ("oraculo: {0} presentaciones en {1:N3} s = {2:N2} fps" -f $filas.Count, $dur, $fpsOraculo)

if ($linea -match "fps=([0-9.]+)") {
    $fpsVerdad = [double]$matches[1]
    $dif = 100.0 * ($fpsOraculo - $fpsVerdad) / $fpsVerdad
    Write-Output ("VEREDICTO: oraculo {0:N2} fps contra verdad {1:N2} fps, diferencia {2:+0.0;-0.0}%" -f $fpsOraculo, $fpsVerdad, $dif)
}
Write-Output "csv: $csv"
