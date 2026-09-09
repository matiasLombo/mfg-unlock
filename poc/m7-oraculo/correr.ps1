# M7: oraculo externo. Da un numero de fps independiente de nuestro codigo.
#
# No hace falta descargar nada: PresentMon_x64.exe 1.9 viene incluido en el SDK
# de FrameView, que ya esta instalado en esta maquina.
#
# Necesita elevacion, porque abre una sesion ETW en tiempo real -- y eso, segun
# la documentacion de Microsoft, solo lo pueden hacer procesos elevados, el grupo
# "Performance Log Users" o servicios del sistema. Sin elevar sale con codigo 1
# y no imprime nada.
#
# Uso:
#   powershell -ExecutionPolicy Bypass -File correr.ps1 -Proceso StreamlineSample.exe -Segundos 30
param(
    [string]$Proceso = "",
    [int]$Segundos = 20,
    [string]$Salida = ""
)

$pm = "C:\Program Files\NVIDIA Corporation\FrameViewSDK\bin\PresentMon_x64.exe"
if (-not (Test-Path $pm)) { Write-Output "NO ESTA: $pm"; exit 2 }

if ($Salida -eq "") {
    $Salida = Join-Path $PSScriptRoot ("captura_" + (Get-Date -Format "HHmmss") + ".csv")
}
if (Test-Path $Salida) { Remove-Item $Salida -Force }

$args = @("--timed", "$Segundos", "--output_file", $Salida,
          "--no_console_stats", "--stop_existing_session",
          "--terminate_after_timed", "--qpc_time")
if ($Proceso -ne "") { $args += @("--process_name", $Proceso) }

$elevado = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Output "elevado: $elevado"
Write-Output "version de PresentMon: $((Get-Item $pm).VersionInfo.FileVersion)"

$p = Start-Process -FilePath $pm -ArgumentList $args -Wait -PassThru -NoNewWindow
Write-Output "exit code: $($p.ExitCode)"

if (-not (Test-Path $Salida)) {
    Write-Output "SIN CSV. Si el exit code es 1 y no imprimio nada, es elevacion."
    exit 1
}

# Resumen: fps por proceso a partir de MsBetweenPresents.
$filas = Import-Csv $Salida
Write-Output "filas: $($filas.Count)"
if ($filas.Count -eq 0) { Write-Output "CSV vacio: nadie presento durante la captura"; exit 1 }

$filas | Group-Object Application | ForEach-Object {
    $ms = $_.Group | ForEach-Object { [double]$_.MsBetweenPresents } | Where-Object { $_ -gt 0 }
    if ($ms.Count -gt 4) {
        $orden = $ms | Sort-Object
        $mediana = $orden[[int]($orden.Count / 2)]
        $caidos = ($_.Group | Where-Object { $_.Dropped -eq "1" }).Count
        Write-Output ("  {0,-28} frames {1,6}  fps mediana {2,7:N1}  caidos {3}" -f `
            $_.Name, $_.Group.Count, (1000.0 / $mediana), $caidos)
    }
}
Write-Output "csv: $Salida"
