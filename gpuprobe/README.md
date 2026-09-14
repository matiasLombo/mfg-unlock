# gpuprobe

Encontrar pasadas mal dimensionadas en un juego de D3D12 y arreglarlas sin
tocar el binario, desde afuera del proceso de render.

Dos modos y un orden que importa:

- **OBSERVE** mide y propone. Es lo que corre siempre la primera vez.
- **ACT** aplica y *vuelve a medir* prendiendo y apagando cada cambio para ver
  si sirvio. La ganancia que va al reporte final es medida, no estimada.

```
juego  ->  version.dll  ->  colector  ->  sesion.jsonl  ->  analizador  ->  reporte.md
                              |                                              + perfil.toml
                              +--  ejecutor  <-------------------------------------+
                                     |
                                     +--  harness A/B  ->  ganancia MEDIDA + capturas PNG
```

## Lo que hay hoy

| modulo | estado |
| --- | --- |
| proxy `version.dll` + enganche por vtable | funciona, sin dependencias |
| colector (recursos, PSOs, pasadas, timestamps, VRAM) | funciona |
| modelo de frame y formato JSONL | funciona, con tests |
| analizador y reporte (Python) | funciona, 6 reglas |
| ejecutor (`resource_scale`, `skip_pass`, `barrier_filter`, `mip_bias`) | funciona, detras de gates |
| `drs_settings` | se parsea y se decide; **todavia no se aplica** (falta NVAPI) |
| overlay ImGui | opcional (ver abajo) |
| harness A/B + capturas PNG | funciona |
| testbed D3D12 propio + CI | funciona sobre WARP, sin GPU |
| D3D11 | no implementado; la interfaz ya es agnostica de la API |

**Ningun juego real se probo todavia.** Todo lo que dice este README esta
verificado contra el testbed propio corriendo sobre WARP en CI, que es mucho
mas de lo que suele haber, y bastante menos que una corrida en una 4070 Ti.

## Antes de instalarlo

**Anticheat.** Esto inyecta un DLL en el proceso del juego. Single-player
unicamente, y fijate que trae el juego antes de copiar nada. La regla del
proyecto es la misma que la de mfg-unlock: si el juego tiene anticheat activo,
no.

**No escribe nada en la carpeta del juego.** Sesiones, perfiles, libreta de
seguridad, capturas y log van todos a `%LOCALAPPDATA%\gpuprobe`. Borrar
`version.dll` deja la maquina exactamente como estaba.

**Si algo falla, no pasa nada.** Cualquier problema cae a passthrough: el juego
sigue como si gpuprobe no existiera. `gpuprobe-off.txt` al lado del DLL lo
apaga sin desinstalar nada, que es lo primero que hay que probar cuando algo
anda mal y no se sabe de quien es la culpa.

## Uso

### 1. Compilar

```sh
sh gpuprobe/build-gpuprobe.sh          # desde Linux, con mingw-w64
cmake -S gpuprobe -B build && cmake --build build --config Release   # en Windows
```

Con ImGui en `external/imgui` se compila ademas el overlay. Sin ImGui gpuprobe
mide, analiza y aplica igual: lo unico que falta es poder mirarlo mientras
pasa.

### 2. Observar

```
copiar build/version.dll a la carpeta del juego
jugar cinco minutos de la escena que interesa
```

Sale una sesion en `%LOCALAPPDATA%\gpuprobe\sessions\<fecha>.jsonl`.

### 3. Analizar

```sh
python3 -m gpuprobe_analyzer analyze sesion.jsonl \
    --report reporte.md --profile Cyberpunk2077.toml
```

El reporte dice a donde se va el frame, que pasadas corren tapadas por otra
queue (optimizarlas no gana nada), y los candidatos con ganancia **estimada**,
riesgo visual y estabilidad frente a parches del juego. El perfil sale con todo
apagado.

### 4. Actuar y medir

Copiar el perfil a `%LOCALAPPDATA%\gpuprobe\profiles\`, poner
`observe_only = false` y `ab = true` en los candidatos que interesen, y volver
a jugar la misma escena. El harness prende y apaga cada candidato cada 60
frames y deja dos capturas PNG por candidato.

```sh
python3 -m gpuprobe_analyzer ab sesion-nueva.jsonl
```

Ese reporte trae la ganancia medida, el intervalo de confianza y el veredicto.
Un candidato cuyo intervalo cruza el cero se descarta, por prometedora que
fuera la estimacion.

## Por que las cuentas dan lo que dan

**La ganancia de una pasada esta limitada por su tiempo exclusivo.** Una pasada
de compute que corre entera adentro de la de sombras puede costar 0.8 ms y
valer cero: bajarla a cero no acorta el frame. El reporte las lista aparte como
no-candidatos, con su costo al lado.

**Si la VRAM esta por encima del budget, todo lo demas es ruido.** Los ms por
pasada estan midiendo paginacion por PCIe. El analizador corta ahi y lo dice.

**La estimacion no es la medicion.** La ganancia estimada es una cuenta sobre
pixeles; la medida sale del harness alternando ON/OFF cada pocos frames, que es
lo unico inmune al drift de clocks de la GPU (en diez minutos de gameplay ese
drift es mas grande que muchas de las optimizaciones que se buscan).

**El matcheo es por descriptor.** Un parche del juego recompila shaders y mueve
todos los hashes; el descriptor de un shadow map no cambia. La contracara es
que el descriptor identifica una CLASE: si tres RTs comparten descriptor, la
accion los toca a los tres, y el reporte lo dice en vez de dejar que aparezca
como sorpresa.

## Verificacion

```sh
sh gpuprobe/run-host-tests.sh     # todo lo puro, sin GPU ni Windows
```

Eso corre los tests de C++ (hash, claves, ring, modelo de frame, perfil,
estadistica, gates del ejecutor, PNG), los del analizador, y el **contrato**:
que el writer de C++ escriba lo que el lector de Python entiende, y que el
veredicto A/B del overlay (C++) y el del reporte (Python) coincidan hasta 1e-9
sobre las mismas muestras.

El CI ademas compila la capa D3D12 con MSVC, corre el testbed sobre WARP -- un
device, query heaps y fences de verdad -- y le pasa el analizador a esa sesion
exigiendo que encuentre los cinco defectos que el testbed mete a proposito.
Tres bugs que los tests unitarios no vieron aparecieron ahi: las pasadas de
compute no se median, el recurso huerfano no se detectaba, y el backbuffer
salia propuesto como recurso huerfano.

## Limites conocidos

- **Agility SDK**: si el juego carga su propio `d3d12core.dll`, las vtables del
  runtime son otras y gpuprobe no engancha nada. Se queda en passthrough y lo
  dice en el log.
- **La primera sesion nunca aplica nada**, por diseno: sin haber observado un
  recurso no hay con que garantizar que escalarlo sea seguro.
- **Backbuffers HDR**: las capturas se saltean en vez de inventarles un
  tonemap. Una imagen que el juego nunca dibujo seria peor que ninguna.
- **El overlay usa D3DCompiler e IMM32** (los trae ImGui). El DLL sin overlay
  importa solo kernel32, msvcrt y user32.
