# Corre el benchmark de Cyberpunk forzando la ventana al frente.
#
# El plugin OTA 134656 apaga DLSS-G cuando la ventana no tiene el foco
# (dlfgPresent.cpp:2544 shouldInterpolate -> "DLSS-G disabled: window not focused").
# Lanzado desde una tarea en segundo plano la ventana no siempre lo toma, y la
# corrida entrega 1.0x sin que nada este roto.
param([string]$Etiqueta = "focus", [string]$EsperadoHash = "")

$dir = "C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64"
$exe = Join-Path $dir "Cyberpunk2077.exe"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Foco {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  public static bool Traer(IntPtr h) {
    uint pid;
    uint destino = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    uint mio = GetCurrentThreadId();
    AttachThreadInput(mio, destino, true);
    ShowWindow(h, 9);          // SW_RESTORE
    BringWindowToTop(h);
    bool ok = SetForegroundWindow(h);
    AttachThreadInput(mio, destino, false);
    return ok;
  }
}
"@

# Los diálogos de CrashReporter que deja un cierre forzado se quedan abiertos y
# le roban el foco a la ventana del juego. Con eso DLSS-G no interpola y la
# corrida sale invalida: cinco seguidas se perdieron asi, y el sintoma parecia
# una regresion del codigo.
Get-Process CrashReporter -ErrorAction SilentlyContinue | Stop-Process -Force
# QmlRenderer es el OTRO proceso del reporte de fallos de CDPR y no se llama
# CrashReporter, asi que sobrevivia a la linea de arriba. Quedaron dos vivos
# durante una tanda de corridas y todas entregaron 1.00.
Get-Process QmlRenderer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$slp = Join-Path $dir "sl.log"
if (Test-Path $slp) { Move-Item $slp (Join-Path $dir "sl.$Etiqueta-prev.log") -Force }
$logp = Join-Path $dir "mfg-unlock.log"
if (Test-Path $logp) { Move-Item $logp (Join-Path $dir "mfg-unlock.$Etiqueta-prev.log") -Force }

# Dos veces hoy estuve por leer numeros de un binario que no era el
# desplegado: el dll congelado del banco, y una copia que fallo con
# "Device or resource busy". El runner ahora se niega a correr asi.
# Con -EsperadoHash se verifica contra ESE binario en vez de contra el compilado
# del repo. Sirve para medir a proposito una linea base vieja sin apagar el guard:
# la proteccion es "se mide lo que se cree que se mide", no "siempre el ultimo".
$dest = Join-Path $dir "version.dll"
if (-not (Test-Path $dest)) { Write-Output "ABORTA: no hay version.dll en el juego."; exit 1 }
$b = (Get-FileHash $dest -Algorithm SHA256).Hash
if ($EsperadoHash -ne "") {
  if (-not $b.StartsWith($EsperadoHash.ToUpper())) {
    Write-Output "ABORTA: el version.dll del juego no es el esperado ($EsperadoHash); es $($b.Substring(0,24))"
    exit 1
  }
  Write-Output "binario verificado contra el hash pedido: $($b.Substring(0,24))"
} else {
  $repo = "$env:USERPROFILE/dev/mfg-unlock/version.dll"
  if (Test-Path $repo) {
    $a = (Get-FileHash $repo -Algorithm SHA256).Hash
    if ($a -ne $b) { Write-Output "ABORTA: el version.dll del juego NO es el compilado."; exit 1 }
    Write-Output "binario verificado contra el compilado"
  }
}
$p = Start-Process -FilePath $exe -ArgumentList "-benchmark" -WorkingDirectory $dir -PassThru
Write-Output "lanzado pid $($p.Id)"

# La ventana tarda en aparecer; una vez que esta, se reafirma el foco varias
# veces porque el juego crea y destruye ventanas al cambiar de modo de video.
$traidas = 0
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep -Milliseconds 1000
  if ($p.HasExited) { break }
  $p.Refresh()
  $h = $p.MainWindowHandle
  if ($h -ne [IntPtr]::Zero) {
    if ([Foco]::GetForegroundWindow() -ne $h) {
      if ([Foco]::Traer($h)) { $traidas++ }
    }
  }
}
if (-not $p.HasExited) { $p.WaitForExit(180000) | Out-Null }
Write-Output "foco reafirmado $traidas veces, salida $($p.ExitCode)"

# Una corrida donde DLSS-G nunca habilito la interpolacion NO es una medicion:
# el juego corre sin generar y el multiplicador da 1.00, que se lee como
# regresion. Es el mismo criterio que aplica tools/bench.py al banco, y aca
# faltaba: dos corridas seguidas dieron p90 1.00 y estuve por reportarlas como
# rotura del codigo cuando lo que fallo fue el foco de la ventana.
$sl = Join-Path $dir "sl.log"
if (Test-Path $sl) {
  $sinfoco = (Select-String -Path $sl -Pattern "window not focused" -SimpleMatch).Count
  $habilito = (Select-String -Path $sl -Pattern "interpolation state changed from disabled to enabled" -SimpleMatch).Count
  Write-Output "sl.log: 'window not focused' x$sinfoco ; interpolacion habilitada x$habilito"
}
# OJO: esta linea NO es de DLSS-G y NO es independiente. "presented fps" en
# nuestro log se calcula como frames_renderizados * (cuenta que pedimos): no
# cuenta una sola presentacion. Dice 4.00x porque pedimos 4x, haya generado o
# no. Se dejo porque sirve para ver QUE se pidio, pero no confirma nada.
# El unico instrumento que observa presentaciones de verdad es el contador del
# swapchain ("counted multiplier"), y para saber si la corrida vale hay que
# mirar ESE mas "interpolation state changed" en sl.log.
$ml = Join-Path $dir "mfg-unlock.log"
if (Test-Path $ml) {
  $pp = Select-String -Path $ml -Pattern "presented fps x10 (\d+)" -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value } | Sort-Object
  $rr = Select-String -Path $ml -Pattern "rendered fps x10 (\d+)" -AllMatches | ForEach-Object { $_.Matches } | ForEach-Object { [int]$_.Groups[1].Value } | Sort-Object
  if ($pp.Count -gt 0 -and $rr.Count -gt 0) {
    $pm = $pp[[int]($pp.Count/2)]; $rm = $rr[[int]($rr.Count/2)]
    if ($rm -gt 0) { Write-Output ("CIRCULAR (no es DLSS-G, es lo que pedimos): {0:N1} / {1:N1} = {2:N2}x" -f ($pm/10), ($rm/10), ($pm/$rm)) }
  }
  # NO se rechaza por esto. La linea "interpolation state changed" viene de una
  # memoria sobre el BANCO y no aparece en la version de Streamline de Cyberpunk:
  # se rechazaron corridas donde presented/rendered daba 4.00x exacto. Falso
  # negativo, y mio.
} else { Write-Output "sl.log: no existe" }
