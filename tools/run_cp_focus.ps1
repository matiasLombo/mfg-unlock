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

$sl = Join-Path $dir "sl.log"
if (Test-Path $sl) {
  $sinfoco = (Select-String -Path $sl -Pattern "window not focused" -SimpleMatch).Count
  Write-Output "sl.log: 'window not focused' x$sinfoco"
} else { Write-Output "sl.log: no existe" }
