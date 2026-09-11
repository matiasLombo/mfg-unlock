# Los crashes que el testigo no ve.
#
# El testigo de excepciones (src/exceptions.h) es un vectored exception
# handler: un __fastfail (0xC0000409) no pasa por ahi, y el log termina sin
# EXCEPCION como si el juego se hubiera cerrado solo. Paso el 11/09 con GTA V
# tres veces en un dia ("no abrio a la primera"): el crash estaba en el Event
# Log de Windows, en sl.pcl.dll, y hubo que ir a buscarlo a mano cada vez.
#
# Resume los eventos 1000 (Application Error) y 1001 (Windows Error Reporting)
# de los cuatro juegos: hora, exe, modulo, codigo y offset EN HEX (el Event
# Log ya los da en hex; el log nuestro los daba en decimal detras de un "0x",
# que es lo que costo media hora el mismo dia).
#
#   tools/crashlog.ps1              # ultimas 24 h
#   tools/crashlog.ps1 -Horas 72
#   tools/crashlog.ps1 -Juego GTA5  # filtra por nombre de exe
param([int]$Horas = 24, [string]$Juego = "")

$exes = "GTA5_Enhanced|Cyberpunk2077|HaloCampaignEvolved|MetroExodus"
if ($Juego -ne "") { $exes = $Juego }
$desde = (Get-Date).AddHours(-$Horas)
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001; StartTime = $desde } -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -match $exes }
if (-not $ev) { Write-Output "sin crashes de $exes en las ultimas $Horas h"; exit 0 }

$filas = foreach ($e in $ev) {
  $m = $e.Message
  if ($e.Id -eq 1000) {
    [pscustomobject]@{
      hora    = $e.TimeCreated.ToString("MM-dd HH:mm:ss")
      exe     = [regex]::Match($m, 'application name: ([^,]+)').Groups[1].Value
      modulo  = [regex]::Match($m, 'module name: ([^,]+)').Groups[1].Value
      codigo  = [regex]::Match($m, 'Exception code: (\S+)').Groups[1].Value
      offset  = [regex]::Match($m, 'Fault offset: (\S+)').Groups[1].Value -replace '^0x0+', '0x'
      fuente  = "1000"
    }
  } else {
    # 1001 (WER). El orden de P7/P8 depende del tipo: APPCRASH lleva P7 codigo,
    # P8 offset; BEX64 (fail-fast) lleva P7 offset, P8 codigo. Otros tipos
    # (AppHangB1, etc.) no son crashes y se saltan.
    $p = @{}
    foreach ($l in ($m -split "`n")) { if ($l -match '^\s*P(\d+):\s*(\S*)') { $p[[int]$matches[1]] = $matches[2] } }
    $tipo = [regex]::Match($m, 'Event Name: (\S+)').Groups[1].Value
    if ($tipo -eq 'BEX64') { $cod = $p[8]; $off = $p[7] }
    elseif ($tipo -eq 'APPCRASH') { $cod = $p[7]; $off = $p[8] }
    else { continue }
    [pscustomobject]@{
      hora    = $e.TimeCreated.ToString("MM-dd HH:mm:ss")
      exe     = $p[1]
      modulo  = $p[4]
      codigo  = ("0x" + $cod)
      offset  = ("0x" + ($off -replace '^0+', ''))
      fuente  = "1001"
    }
  }
}
# Un crash produce un 1000 y un 1001 con lo mismo: se muestra el 1000 y el
# 1001 solo si no hay un 1000 del mismo exe a menos de un minuto.
$mil = @($filas | Where-Object { $_.fuente -eq "1000" })
$sueltos = @($filas | Where-Object { $_.fuente -eq "1001" } | Where-Object {
  $f = $_; -not ($mil | Where-Object { $_.exe -eq $f.exe -and [math]::Abs(([datetime]::ParseExact($_.hora, "MM-dd HH:mm:ss", $null) - [datetime]::ParseExact($f.hora, "MM-dd HH:mm:ss", $null)).TotalSeconds) -lt 60 }) })
($mil + $sueltos) | Sort-Object hora -Descending |
  Format-Table hora, exe, modulo, codigo, offset -AutoSize | Out-String -Width 160 | Write-Output
Write-Output "0xC0000409 = fail-fast (no lo ve el testigo); 0xC0000005 = access violation; 0xC00000FD = pila desbordada (recursion)"
