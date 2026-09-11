# Toca "Continue" en el dialogo "Launch Game with custom arguments" de Steam.
#
# El dialogo es UI web (CEF): SendKeys no le llega y se clickea. Antes se
# clickeaba en (1704,928), medido con la ventana de Steam en un tamano fijo;
# el 11/09 Steam estaba maximizada y dos corridas abortaron ("Steam no lanzo
# Cyberpunk2077 en 90 s") porque el boton estaba en otro lado. El dialogo se
# centra sobre la ventana de Steam y Continue queda a (+104, +106) del
# centro: medido en dos capturas (maximizada: centro 1280,696 -> boton
# 1384,802; sin maximizar: 1600,822 -> 1704,928).
#
# Se llama desde run_steam.ps1 y run_cp_focus.ps1 cada 3 s mientras el
# proceso del juego no aparezca. Devuelve $true si clickeo.
if (-not ("SteamWin" -as [type])) {
  Add-Type @"
using System; using System.Runtime.InteropServices;
public struct RECT { public int L, T, R, B; }
public class SteamWin {
  [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
}
function Click-SteamContinue {
  # La ventana principal es del proceso steamwebhelper (titulo "Steam");
  # FindWindow por titulo daba otra "Steam" oculta, con rect 0,0-0,0.
  $h = [IntPtr]::Zero
  foreach ($pr in (Get-Process steamwebhelper -ErrorAction SilentlyContinue)) {
    if ($pr.MainWindowHandle -ne 0 -and $pr.MainWindowTitle -eq "Steam") { $h = $pr.MainWindowHandle; break }
  }
  if ($h -eq [IntPtr]::Zero) { return $false }
  $r = New-Object RECT
  if (-not [SteamWin]::GetWindowRect($h, [ref]$r)) { return $false }
  $cx = [int](($r.L + $r.R) / 2); $cy = [int](($r.T + $r.B) / 2)
  $ws = New-Object -ComObject WScript.Shell
  $ws.AppActivate("Steam") | Out-Null; Start-Sleep -Milliseconds 300
  [SteamWin]::SetCursorPos($cx + 104, $cy + 106); Start-Sleep -Milliseconds 150
  [SteamWin]::mouse_event(2,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 80
  [SteamWin]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
  return $true
}
