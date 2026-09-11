@echo off
rem Compila version.dll con build-proxy.sh (Git Bash) y la copia a la carpeta
rem de Halo. Toda la salida va a build-e-instalar.log al lado de este archivo.
setlocal
cd /d "%~dp0"
set LOG=%~dp0build-e-instalar.log
echo === %DATE% %TIME% === > "%LOG%"
echo [1/3] build-proxy.sh >> "%LOG%"
"C:\Program Files\Git\bin\bash.exe" --login -c "cd '%~dp0' && sh build-proxy.sh" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo BUILD FALLO, errorlevel %errorlevel% >> "%LOG%"
  goto fin
)
if not exist version.dll (
  echo BUILD FALLO: no hay version.dll >> "%LOG%"
  goto fin
)
echo [2/3] tamano y fecha de version.dll >> "%LOG%"
dir version.dll >> "%LOG%"
set HALO=C:\Program Files (x86)\Steam\steamapps\common\Halo Campaign Evolved\Meteorite\Binaries\Win64
echo [3/3] copiando a "%HALO%" >> "%LOG%"
copy /y version.dll "%HALO%\version.dll" >> "%LOG%" 2>&1
if errorlevel 1 (
  echo COPIA FALLO, errorlevel %errorlevel% >> "%LOG%"
  goto fin
)
dir "%HALO%\version.dll" >> "%LOG%"
echo LISTO >> "%LOG%"
:fin
type "%LOG%"
echo.
echo (esta ventana se cierra sola en 15 s)
timeout /t 15 >nul
endlocal
