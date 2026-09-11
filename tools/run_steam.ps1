# Lanza un juego por Steam con argumentos, pasa el dialogo "Launch Game with
# custom arguments / Continue", sostiene el foco y espera a que termine (o a un
# tope de segundos). Generalizacion de run_cp_focus.ps1 para cualquier juego.
#
#   tools/run_steam.ps1 -AppId 1449560 -Proc MetroExodus -Args "-benchmark" -Max 300
param([int]$AppId, [string]$Proc, [string]$Args = "", [int]$Max = 300)

Add-Type @"
using System; using System.Runtime.InteropServices;
public class FocoS {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  public static bool Traer(IntPtr h) { uint pid; uint d = GetWindowThreadProcessId(GetForegroundWindow(), out pid); uint m = GetCurrentThreadId(); AttachThreadInput(m, d, true); ShowWindow(h, 9); BringWindowToTop(h); bool ok = SetForegroundWindow(h); AttachThreadInput(m, d, false); return ok; }
}
"@
. "$PSScriptRoot\steam_continue.ps1"
$url = if ($Args -ne "") { "steam://run/$AppId//$Args/" } else { "steam://rungameid/$AppId" }
Start-Process $url
$ws = New-Object -ComObject WScript.Shell
$p = $null
for ($i = 0; $i -lt 120 -and $p -eq $null; $i++) {
  Start-Sleep -Milliseconds 1000
  $p = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($p -eq $null -and $Args -ne "" -and $i -ge 3 -and ($i % 3) -eq 0 -and $i -le 27) {
    Click-SteamContinue | Out-Null
  }
}
if ($p -eq $null) { Write-Output "ABORTA: Steam no lanzo $Proc en 120 s"; exit 1 }
Write-Output "lanzado por Steam, pid $($p.Id)"
$traidas = 0
for ($i = 0; $i -lt $Max; $i++) {
  Start-Sleep -Milliseconds 1000
  $q = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($q -eq $null) { Write-Output "termino a los $i s"; break }
  $h = $q.MainWindowHandle
  if ($h -ne [IntPtr]::Zero -and [FocoS]::GetForegroundWindow() -ne $h) { if ([FocoS]::Traer($h)) { $traidas++ } }
}
$q = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
if ($q) { Write-Output "tope de $Max s: se cierra"; Stop-Process -Id $q.Id -Force }
Write-Output "foco reafirmado $traidas veces"
