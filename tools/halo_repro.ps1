# Reproduce el crash de Halo sin que haya alguien apretando teclas.
#
# El repro lo dio el usuario: abrir, esperar el menu principal, apretar Enter
# para continuar, y a partir de ahi crashea. Con mfg-settings.txt en mode 8
# (DYNAMIC) y objetivo 165.
#
# Existe porque el banco NO es el criterio: reproduce el crash una de cada
# varias corridas y el que decide es Halo. Ver docs/objetivo-halo-6x.md.
#
# Deja el juego cerrado siempre, crashee o no. Un proceso colgado retiene el
# swapchain y la corrida siguiente no vale -- eso ya paso.
param(
    [int]$EsperaMenu = 45,       # segundos hasta que aparece el menu principal
    [int]$SegundosJuego = 180,   # cuanto se deja correr despues del Enter
    [int]$Ciclos = 1
)

$ErrorActionPreference = "Stop"
$Win64 = "C:\Program Files (x86)\Steam\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64"
$Dumps = Join-Path $Win64 "ue4ss"
$Log   = Join-Path $Win64 "mfg-unlock.log"
$AppId = 2806050
$Proc  = "HaloCampaignEvolved"

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern void keybd_event(byte k, byte s, uint f, IntPtr e);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@

function Traer-Adelante([IntPtr]$h) {
    $mio  = [W]::GetCurrentThreadId()
    $suyo = [W]::GetWindowThreadProcessId([W]::GetForegroundWindow(), [IntPtr]::Zero)
    if ($suyo -ne 0 -and $suyo -ne $mio) { [W]::AttachThreadInput($mio, $suyo, $true) | Out-Null }
    [W]::ShowWindow($h, 9) | Out-Null
    [W]::BringWindowToTop($h) | Out-Null
    [W]::SetForegroundWindow($h) | Out-Null
    if ($suyo -ne 0 -and $suyo -ne $mio) { [W]::AttachThreadInput($mio, $suyo, $false) | Out-Null }
}

function Matar-Todo {
    foreach ($n in @($Proc, "CrashReportClient", "UnrealCEFSubProcess")) {
        Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Seconds 4
}

$resultados = @()
for ($c = 1; $c -le $Ciclos; $c++) {
    Write-Output "=== ciclo $c de $Ciclos ==="
    Matar-Todo

    # Marca de agua: cualquier dump posterior es de este ciclo.
    $marca = Get-Date
    Write-Output "  marca de agua: $($marca.ToString('HH:mm:ss'))"

    Start-Process "steam://rungameid/$AppId"

    # Esperar al proceso y a su ventana.
    $p = $null
    for ($i = 0; $i -lt 180; $i++) {
        Start-Sleep -Seconds 1
        $p = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne 0) { break }
    }
    if (-not $p -or $p.MainWindowHandle -eq 0) {
        Write-Output "  ARRANQUE FALLIDO: no aparecio la ventana"
        $resultados += "arranque fallido"
        Matar-Todo
        continue
    }
    Write-Output "  ventana arriba, esperando el menu ($EsperaMenu s)"
    Start-Sleep -Seconds $EsperaMenu

    if ($p.HasExited) {
        Write-Output "  murio ANTES del Enter"
        $resultados += "murio antes del Enter"
        Matar-Todo
        continue
    }

    # El Enter del repro. Se trae la ventana adelante primero: sin foco el
    # plugin ni siquiera interpola (dlssg-needs-window-focus).
    Traer-Adelante $p.MainWindowHandle
    Start-Sleep -Seconds 2
    Write-Output "  ENTER"
    [W]::keybd_event(0x0D, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [W]::keybd_event(0x0D, 0, 2, [IntPtr]::Zero)

    # Vigilar: dump nuevo, dialogo de Fatal Error, o el proceso que se va.
    $veredicto = "SIN CRASH"
    for ($i = 0; $i -lt $SegundosJuego; $i++) {
        Start-Sleep -Seconds 1
        $nuevo = Get-ChildItem "$Dumps\*.dmp" -ErrorAction SilentlyContinue |
                 Where-Object { $_.LastWriteTime -gt $marca }
        if ($nuevo) { $veredicto = "CRASH (dump: $($nuevo[0].Name))"; break }
        $fatal = Get-Process $Proc -ErrorAction SilentlyContinue |
                 Where-Object { $_.MainWindowTitle -match "Fatal" }
        if ($fatal) { $veredicto = "CRASH (dialogo Fatal Error)"; break }
        if ((Get-Process $Proc -ErrorAction SilentlyContinue) -eq $null) {
            $veredicto = "EL PROCESO SE FUE (sin dump)"; break
        }
        # Mantener el foco: si lo pierde deja de interpolar y el ciclo no prueba nada.
        $q = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($q -and $q.MainWindowHandle -ne 0 -and
            [W]::GetForegroundWindow() -ne $q.MainWindowHandle) {
            Traer-Adelante $q.MainWindowHandle
        }
    }

    Write-Output "  $veredicto"
    $resultados += $veredicto
    Matar-Todo
}

Write-Output ""
Write-Output "=== resumen ==="
for ($i = 0; $i -lt $resultados.Count; $i++) {
    Write-Output ("  ciclo {0}: {1}" -f ($i + 1), $resultados[$i])
}
$ok = ($resultados | Where-Object { $_ -eq "SIN CRASH" }).Count
Write-Output "  sin crash: $ok de $($resultados.Count)"
