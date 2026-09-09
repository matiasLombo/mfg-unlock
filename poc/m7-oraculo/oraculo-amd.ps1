# M7, prueba elevada con el PresentMon de AMD.
#
# Sin elevar, esta build corre bien (exit 0, "Started recording") pero NO emite
# ninguna fila: ni a CSV ni a stdout, con filtro de proceso o sin el. Ya se
# descartaron, midiendo: la sintaxis de los flags (valida los desconocidos), la
# ruta del CSV (absoluta y relativa), --output_stdout, --process_name (sin
# elevar los procesos son '<unknown>' y el filtro no matchea), --v1_metrics y
# las opciones de tracking (--no_track_display baja los eventos perdidos de 63k
# a 2.8k, o sea la sesion ETW existe y recibe, pero igual salen cero filas).
#
# Queda una sola variable sin probar en esta build: elevacion. La corrida
# elevada anterior uso el binario de NVIDIA, que falla distinto (exit 1).
param([int]$Segundos = 12)

$aqui = $PSScriptRoot
Start-Transcript -Path (Join-Path $aqui "oraculo-amd-salida.txt") -Force | Out-Null
$pm = "C:\Program Files\AMD\CNext\CNext\PresentMon-x64.exe"
$pres = Join-Path $aqui "presentador.exe"
$csv = Join-Path $aqui "amd-elevado.csv"
$salida = Join-Path $aqui "presentador-amd.txt"
$pmOut = Join-Path $aqui "amd-stdout.txt"
$pmErr = Join-Path $aqui "amd-stderr.txt"

$elevado = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Output "elevado: $elevado"
if (-not $elevado) { Write-Output "ABORTA: hay que correrlo elevado."; Stop-Transcript | Out-Null; exit 1 }
foreach ($f in @($pm, $pres)) { if (-not (Test-Path $f)) { Write-Output "falta $f"; Stop-Transcript | Out-Null; exit 2 } }
foreach ($f in @($csv, $salida, $pmOut, $pmErr)) { if (Test-Path $f) { Remove-Item $f -Force } }

$p = Start-Process -FilePath $pres -ArgumentList "$($Segundos + 8)" -PassThru -RedirectStandardOutput $salida -WindowStyle Normal
Start-Sleep -Seconds 3

Write-Output "capturando $Segundos s con PresentMon de AMD"
$a = @("--timed", "$Segundos", "--output_file", $csv, "--process_name", "presentador.exe",
       "--session_name", "mfgamdelev", "--stop_existing_session", "--terminate_after_timed", "--qpc_time")
$q = Start-Process -FilePath $pm -ArgumentList $a -Wait -PassThru -NoNewWindow -RedirectStandardOutput $pmOut -RedirectStandardError $pmErr
Write-Output "PresentMon exit: $($q.ExitCode)"
foreach ($f in @($pmOut, $pmErr)) {
    if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
        Write-Output "--- $(Split-Path $f -Leaf):"
        Get-Content $f | Select-Object -First 10 | ForEach-Object { Write-Output "    $_" }
    }
}

$p.WaitForExit(60000) | Out-Null
$linea = Get-Content $salida -ErrorAction SilentlyContinue | Where-Object { $_ -match "PRESENTADOR" }
Write-Output "verdad del presentador: $linea"

if (-not (Test-Path $csv)) { Write-Output "SIN CSV: elevado tampoco captura"; Stop-Transcript | Out-Null; exit 1 }
$filas = @(Import-Csv $csv)
Write-Output "filas en el CSV: $($filas.Count)"
if ($filas.Count -lt 5) { Write-Output "muy pocas filas para comparar"; Stop-Transcript | Out-Null; exit 1 }
$col = if ($filas[0].PSObject.Properties.Name -contains "TimeInSeconds") { "TimeInSeconds" } else { "CPUStartTime" }
$t0 = [double]$filas[0].$col
$t1 = [double]$filas[-1].$col
$dur = $t1 - $t0
$fpsOraculo = if ($dur -gt 0) { ($filas.Count - 1) / $dur } else { 0 }
Write-Output ("oraculo: {0} presentaciones en {1:N3} s = {2:N2} fps" -f $filas.Count, $dur, $fpsOraculo)
if ($linea -match "fps=([0-9.]+)") {
    $fpsVerdad = [double]$matches[1]
    $dif = 100.0 * ($fpsOraculo - $fpsVerdad) / $fpsVerdad
    Write-Output ("VEREDICTO: oraculo {0:N2} fps contra verdad {1:N2} fps, diferencia {2:+0.0;-0.0}%" -f $fpsOraculo, $fpsVerdad, $dif)
}
Stop-Transcript | Out-Null
