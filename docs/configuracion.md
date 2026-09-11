# Configuracion

Dos archivos al lado de `version.dll`:

- **`mfg-settings.txt`** — lo que el panel guarda: `mode`, `target`, `dynfps`,
  `hud`. El panel lo reescribe entero; no pongas nada mas ahi.
- **`mfg-config.txt`** — todo lo demas, una clave por linea, `clave valor`.
  Nadie lo escribe salvo vos. Se lee una vez al cargar.

Los archivos-bandera viejos (`mfg-sinseis.txt`, `mfg-ota.txt`, ...) siguen
andando. Si una clave y un archivo dicen cosas distintas, gana la clave.

Polaridad DIRECTA en el archivo: `seis 0` apaga el 6X, `ota 1` enciende OTA.
El defecto es lo que corre sin ningun archivo; lo que hace funcionar el mod va
encendido por defecto y una clave solo lo apaga ([[flags-only-disable]]).

Fuente: `src/config.h` (la tabla `kFlags` y `kNumerics`). Test:
`tools/test_config.cpp`.

## Banderas

| clave | defecto | archivo viejo | que hace |
|---|---|---|---|
| `seis` | 1 | `mfg-sinseis.txt` apaga | tope 6X (5 si esta apagado) |
| `frac` | 1 | `mfg-nofrac.txt` apaga | planificador fraccional (CUSTOM/DYNAMIC) |
| `slowalt` | 1 | `mfg-noslowalt.txt` apaga | alternancia por bloques del fraccional |
| `wic` | 1 | `mfg-nowic.txt` apaga | parche del contador por frame (el byte) |
| `cubins` | 1 | `mfg-nocubins.txt` apaga | kernels Blackwell en el snippet ([[cubins-are-the-fluidity-fix]]) |
| `panel` | 1 | `mfg-nopanel.txt` apaga | el panel (tecla `) |
| `sat` | 1 | `mfg-sinsat.txt` apaga | deteccion de techo de DYNAMIC |
| `latch` | 1 | `mfg-nolatch.txt` apaga | reparto latcheado por ciclo |
| `deuda` | 1 | `mfg-sin-deuda.txt` apaga | integrador de deuda de DYNAMIC |
| `debug` | 0 | `mfg-debug.txt` | grabador F9 |
| `watch` | 0 | `mfg-watch.txt` | relee mfg-settings.txt dos veces por segundo |
| `novsync` | 0 | `mfg-novsync.txt` | diagnostico: Present con intervalo 0 y ALLOW_TEARING |
| `pinlatency` | 0 | `mfg-pinlatency.txt` | diagnostico: parche que fija la frame latency del plugin |
| `pacefollow` | 0 | `mfg-pacefollow.txt` | parche: el pacer espera la cuenta del frame propio |
| `sub2` | 0 | `mfg-sub2.txt` | permite objetivos bajo 2.00x (piso 1.10) |
| `twocopies` | 0 | `mfg-twocopies.txt` | experimento: segunda copia del plugin ([[two-plugin-copies]]) |
| `ceilfirst` | 0 | `mfg-ceilfirst.txt` | A1: declara el techo del ciclo mientras apagado |
| `host` | 0 | `mfg-host.txt` | experimento: modo host, nuestro Streamline en un juego que solo trae DLSS 2 (`src/host.h`); el dll va como `winmm.dll` y `mode` de mfg-settings manda igual |
| `ota` | 0 | `mfg-ota.txt` | fuerza OTA en slInit ([[forzar-ota-para-parchear]]) |
| `quiet` | 0 | `mfg-quiet.txt` | sin el volcado por ventana en el log |
| `nullalt` | 0 | `mfg-nullalt.txt` | fraccional: alterna entre iguales (experimento) |
| `pathsplugins` | 0 | `mfg-pathsplugins.txt` | apunta pathsToPlugins a nuestra carpeta (fase D) |
| `peralt` | 0 | `mfg-peralt.txt` | difusion de error por frame |
| `coninterposer` | 0 | `mfg-coninterposer.txt` | sustituye TAMBIEN el interposer (pierde constantes en Halo) |
| `sinbase` | 0 | `mfg-sinbase.txt` | no sustituye el snippet |
| `mfcmax` | 0 | `mfg-mfcmax.txt` | intenta subir MultiFrameCountMax a 5 |
| `topefijo` | 0 | `mfg-topefijo.txt` | tope 5 fijo: LINEA BASE, crashea |
| `x6` | 0 | `mfg-x6.txt` | viejo: 6X a mano (hoy `seis` ya lo cubre) |
| `dyndiag` | 0 | `mfg-dyndiag.txt` | diagnostico por cambio de ratio |
| `optsv3` | 0 | `mfg-optsv3.txt` | banco: baja DLSSGOptions a version 3 |
| `blockalt` | 0 | `mfg-blockalt.txt` | fraccional: bloques tambien arriba de 2.0x |
| `nowaitable` | 0 | `mfg-nowaitable.txt` | quita FRAME_LATENCY_WAITABLE_OBJECT al crear |
| `monoidx` | 0 | `mfg-monoidx.txt` | parche del indice monotonico |
| `meter_off` | 0 | `mfg-nometer.txt` | parche: apaga el flip metering del driver |
| `presetb` | 0 | `mfg-presetb.txt` | parche: fuerza el preset B (UIR) de interpolacion |
| `sllog` | 0 | `mfg-sllog.txt` | sl.log nivel 2 al lado de la dll (megabytes por sesion) |
| `indicator` | 0 | `mfg-indicator.txt` | indicador de NVIDIA en pantalla (__NGX_SHOW_INDICATOR) |

## Numericos

| clave | rango | archivo viejo | que hace |
|---|---|---|---|
| `blockms N` | 1..99999 | `mfg-blockms.txt` | largo del bloque del fraccional, ms |
| `blocks N` | 2..64 | `mfg-blocks.txt` | bloques por ciclo |
| `jitter N` | 1..90 | `mfg-jitter.txt` | banco: jitter de frame, % |
| `clamplatency N` | 1..9 | `mfg-clamplatency.txt` | latencia maxima de frames |
| `queue N` | 0..3 | `mfg-queue.txt` | modo de paralelismo de la cola |
| `markergap A B [C D]` | ms | `mfg-markergap.txt` | banco: apagones de marcadores Reflex |
| `slowframe A [B C]` | us | `mfg-slowframe.txt` | banco: frame lento ([[bench-needs-a-slow-frame]]) |

## Ejemplo

```
# Halo: 6X apagado y sl.log encendido
seis 0
sllog 1
```
