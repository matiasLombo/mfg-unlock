# Mantiene la ventana del sample adelante mientras dura la corrida.
#
# El plugin se niega a interpolar sin foco (dlssg-needs-window-focus), y en la
# corrida de las 00:34 el foco se perdio a los 12.8 s: seis ventanas validas de
# veintiuna. El lab levanta la ventana al arrancar y nada la sostiene despues.
#
# Reafirma el primer plano solo cuando NO lo tiene, una vez por segundo. Si ya
# esta adelante no hace nada, para no meter ruido en la propia medicion.
param([int]$Segundos = 240)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Fg {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@

$fin = (Get-Date).AddSeconds($Segundos)
while ((Get-Date) -lt $fin) {
    $p = Get-Process StreamlineSample -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p -and $p.MainWindowHandle -ne 0) {
        if ([Fg]::GetForegroundWindow() -ne $p.MainWindowHandle) {
            $h = $p.MainWindowHandle
            $mio = [Fg]::GetCurrentThreadId()
            $suyo = [Fg]::GetWindowThreadProcessId([Fg]::GetForegroundWindow(), [IntPtr]::Zero)
            if ($suyo -ne 0 -and $suyo -ne $mio) { [Fg]::AttachThreadInput($mio, $suyo, $true) | Out-Null }
            [Fg]::ShowWindow($h, 9) | Out-Null
            [Fg]::BringWindowToTop($h) | Out-Null
            [Fg]::SetForegroundWindow($h) | Out-Null
            if ($suyo -ne 0 -and $suyo -ne $mio) { [Fg]::AttachThreadInput($mio, $suyo, $false) | Out-Null }
        }
    }
    Start-Sleep -Milliseconds 1000
}
