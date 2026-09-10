# Un ciclo de Cyberpunk sin que nadie juegue.
#
# Cyberpunk arranca directo en la escena del benchmark con -benchmark, la corre
# unos 65 s y cierra el proceso solo. Eso lo convierte en un banco determinista,
# que es lo que hacia falta: el banco de Streamline no puede reproducir lo que
# Cyberpunk hace -- cuatro copias de sl.dlss_g, un swapchain descartable antes
# del real, la estructura de opciones en v3.
#
#   .\tools\cp_ciclo.ps1 -Flags "mfg-sinseis.txt,mfg-sinbase.txt" -Modo 4
#
# Deja el log en runs\cp-<etiqueta>.log y dice si hubo EXCEPCION.
param(
    [string]$Flags = "",
    [int]$Modo = 4,
    [string]$Etiqueta = "",
    [int]$TimeoutSeg = 120
)

$dir = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
$exe = Join-Path $dir "Cyberpunk2077.exe"
$log = Join-Path $dir "mfg-unlock.log"
$salida = "C:\Users\matia\dev\mfg-unlock\runs"
if (-not (Test-Path $salida)) { New-Item -ItemType Directory -Path $salida | Out-Null }
if ($Etiqueta -eq "") { $Etiqueta = if ($Flags -eq "") { "sin-flags" } else { $Flags -replace "[,\.]", "-" } }

# Estado limpio: se van TODOS los mfg-*.txt salvo settings, y se ponen los pedidos.
Get-ChildItem -LiteralPath $dir -Filter "mfg-*.txt" |
    Where-Object { $_.Name -ne "mfg-settings.txt" } |
    Remove-Item -Force
foreach ($f in ($Flags -split "," | Where-Object { $_ -ne "" })) {
    New-Item -ItemType File -Path (Join-Path $dir $f.Trim()) -Force | Out-Null
}
Set-Content -LiteralPath (Join-Path $dir "mfg-settings.txt") `
    -Value "mode $Modo`ntarget 400`ndynfps 165`nhud 1" -NoNewline
Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue

try { Get-Process Cyberpunk2077 -ErrorAction Stop | Stop-Process -Force } catch {}
Start-Sleep -Seconds 2

$p = Start-Process -FilePath $exe -ArgumentList "-benchmark" -WorkingDirectory $dir -PassThru
$fin = (Get-Date).AddSeconds($TimeoutSeg)
while (-not $p.HasExited -and (Get-Date) -lt $fin) { Start-Sleep -Milliseconds 500 }
$vivo = -not $p.HasExited
if ($vivo) { try { $p | Stop-Process -Force } catch {} }
Start-Sleep -Seconds 2

$exc = 0; $ventanas = 0; $lineas = 0
if (Test-Path $log) {
    $t = Get-Content -LiteralPath $log -Raw
    $lineas = ([regex]::Matches($t, "`n")).Count
    $exc = ([regex]::Matches($t, "EXCEPCION")).Count
    $ventanas = ([regex]::Matches($t, "counted multiplier")).Count
    Copy-Item -LiteralPath $log -Destination (Join-Path $salida "cp-$Etiqueta.log") -Force
}
$veredicto = if ($exc -gt 0) { "CRASHEA" } elseif ($ventanas -gt 0) { "ANDA" } else { "sin ventanas" }
"{0,-34} {1,-12} exc={2} ventanas={3} lineas={4} vivo_al_timeout={5}" -f `
    $Etiqueta, $veredicto, $exc, $ventanas, $lineas, $vivo
