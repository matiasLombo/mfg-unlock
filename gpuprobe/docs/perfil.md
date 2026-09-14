# El perfil de gpuprobe

Un archivo TOML por juego, en `%LOCALAPPDATA%\gpuprobe\profiles\<exe>.toml`
(sin la extension del ejecutable: `Cyberpunk2077.exe` lee
`Cyberpunk2077.toml`). Nunca se escribe nada en la carpeta del juego.

El perfil lo genera el analizador (`gpuprobe analyze --profile`), y despues se
edita a mano. Se recarga en caliente: guardar el archivo aplica los cambios sin
reiniciar el juego.

## Las tres reglas del formato

1. **Un error invalida el archivo entero.** Si el TOML tiene una clave
   desconocida, un `scale` fuera de rango o una categoria mal escrita, no se
   aplica *nada* y sigue vigente el perfil anterior. Con hot-reload, un perfil
   a medio aplicar deja un estado que despues nadie puede reproducir.
2. **Toda accion nace apagada.** Sin `enabled = true` explicito, la accion se
   carga, se valida y no hace nada.
3. **El matcheo es por DESCRIPTOR, no por hash de shader.** Un parche del juego
   recompila shaders y mueve todos los hashes mucho mas seguido de lo que
   cambia "el shadow map es un D32 de 4096 en array de 4".

## Un perfil completo

```toml
[gpuprobe]
exe = "Cyberpunk2077.exe"
note = "medido en la corrida del 2026-09-14, escena del mercado"

# false = el ejecutor puede actuar. Empeza siempre en true.
observe_only = false

# Uno de cada cuantos frames se instrumenta ENTERO (cada pasada, cada draw).
# El resto van en modo liviano, con un presupuesto fijo de queries.
deep_every = 120
query_budget = 64

# Harness A/B: cada cuantos frames conmuta, y cuantos descarta despues de
# conmutar (los primeros miden caches frias, no la optimizacion).
ab_period = 60
ab_warmup = 5

[[action]]
name = "sombras 4k a la mitad"
kind = "resource_scale"
match = { category = "shadowmap", min_w = 2048, format = "D32_FLOAT" }
scale = 0.5
verify = "full"
enabled = true
ab = true

[[action]]
name = "SSR a media resolucion"
kind = "resource_scale"
match = { category = "rt_full", format = "R16G16B16A16_FLOAT" }
scale = 0.5
verify = "no_copy_observed"

[[action]]
name = "sacar la vuelta por COMMON del gbuffer"
kind = "barrier_filter"
match = { from = "PIXEL_SHADER_RESOURCE", to = "COMMON" }
enabled = true
```

## `[gpuprobe]`

| clave | default | que hace |
| --- | --- | --- |
| `exe` | — | solo documentacion; el archivo ya se elige por nombre |
| `note` | — | para acordarse de en que escena se midio |
| `observe_only` | `true` | `true` = medir y no tocar nada |
| `deep_every` | `120` | uno de cada N frames se instrumenta entero. `0` es invalido |
| `query_budget` | `64` | maximo de queries de timestamp por frame en modo liviano. Minimo 8 |
| `ab_period` | `60` | frames por condicion del harness A/B |
| `ab_warmup` | `5` | frames a descartar despues de cada conmutacion |

## `[[action]]`

| clave | aplica a | que hace |
| --- | --- | --- |
| `name` | todas | aparece en el overlay y en el log |
| `kind` | todas | ver la tabla de abajo |
| `match` | todas | el matcher, tabla inline (ver **Matchers**) |
| `scale` | `resource_scale` | factor en `(0, 1]`. `0.5` = mitad de ancho y de alto |
| `mip_bias` | `mip_bias` | entero distinto de cero |
| `drs_id`, `drs_value` | `drs_settings` | id y valor del setting del driver |
| `verify` | todas | el gate de seguridad (ver **Gates**) |
| `enabled` | todas | `false` por defecto |
| `ab` | todas | entra al ciclo del harness A/B |

### `kind`

| kind | estado | que hace |
| --- | --- | --- |
| `resource_scale` | **aplicado** | crea el recurso mas chico y escala los viewports que lo tienen bindeado |
| `skip_pass` | **aplicado** | descarta los draws de la pasada que escribe sobre ese recurso |
| `barrier_filter` | **aplicado** | no ejecuta esa transicion |
| `mip_bias` | **aplicado** | las vistas (SRV) de esa textura empiezan N mips mas abajo |
| `drs_settings` | parseado, **todavia no aplicado** | falta NVAPI, que es una dependencia externa |

Sobre `mip_bias`, dos precisiones que cambian para que sirve:

- Se aplica sobre la **vista**, no sobre el recurso. Cambiar el descriptor del
  recurso cambiaria lo que el juego cree que creo; mover `MostDetailedMip` en
  el SRV no. Si la accion se apaga, la proxima vista vuelve a ser la original.
- **No ahorra VRAM.** Los mips grandes siguen residentes; lo que baja es el
  ancho de banda de texturas y la presion de cache. Contra un techo de VRAM
  esto no alcanza: ahi lo que sirve es bajar un escalon la calidad de texturas
  del juego.

`drs_settings` se valida y se muestra en el overlay, pero hoy no cambia nada.
Preferimos que el perfil documente la intencion y que el estado se diga aca a
que una accion parezca activa y no lo este.

## Matchers

Un matcher vacio matchea todo. Cada clave agrega una condicion (todas tienen
que cumplirse).

| clave | ejemplo | notas |
| --- | --- | --- |
| `category` | `"shadowmap"` | ver la lista de abajo |
| `format` | `"D32_FLOAT"` | el mismo nombre que sale en el reporte |
| `scale` | `"half"` | clase de escala contra la resolucion de salida |
| `min_w`, `max_w` | `2048` | en pixeles, sobre el tamano real |
| `min_h`, `max_h` | `1024` | idem |
| `min_array` | `4` | slices del array (las cascadas de sombra) |
| `square` | `true` | ancho igual a alto |
| `require_flags`, `deny_flags` | `4` | bits de `D3D12_RESOURCE_FLAGS` |
| `dkey` | `0x7354ea63...` | clave exacta de descriptor, del reporte |
| `from`, `to` | `"COMMON"` | solo para `barrier_filter` |

### Categorias

`backbuffer`, `buffer`, `shadowmap`, `shadowcube`, `depthbuffer`, `cubemap`,
`volume`, `rt_full`, `rt_half`, `rt_quarter`, `rt_other`, `texture`, `staging`,
`unknown`.

Se infieren del descriptor, porque D3D12 no tiene ningun campo que diga "esto
es un shadow map". Cada regla esta escrita para caer en `unknown` antes que en
una categoria equivocada: un `unknown` se ve en el reporte, un shadow map mal
etiquetado se convierte en una accion que rompe el render.

**`backbuffer` es especial**: gpuprobe nunca lo escala, ni con
`verify = "none"`. No hay perfil que lo habilite.

### Clases de escala

`full` (>= 90% de la salida), `half`, `quarter`, `eighth`, `larger`, `fixed`
(sin relacion con la salida).

Un matcher por clase de escala sobrevive un cambio de resolucion y el DRS: la
misma regla escrita a 1440p matchea a 2160p. Un recurso que no escala con la
salida -- un shadow map, un atlas -- se identifica por sus numeros reales, que
es justo lo que lo distingue.

## Gates (`verify`)

Un gate es una condicion que gpuprobe tiene que haber **observado** antes de
aplicar la accion. No se asume seguro por defecto, y eso tiene una consecuencia
concreta: **la primera sesion de un juego es siempre observacion**. Se mira, se
escribe lo aprendido en `%LOCALAPPDATA%\gpuprobe\safety\<exe>.txt`, y recien la
proxima corrida puede actuar.

| valor | exige |
| --- | --- |
| `full` (default) | las tres condiciones de abajo |
| `no_copy_observed` | que el recurso nunca haya sido origen ni destino de una copia |
| `not_placed` | que no comparta heap con otro recurso ni sea tiled |
| `viewports_tracked` | que sus viewports pasen por `RSSetViewports` |
| `none` | nada: el usuario se hace cargo |

Por que cada uno:

- **copia**: `CopyTextureRegion` y `CopyResource` llevan extents fijos. Un
  recurso mas chico del que la copia espera es corrupcion o un device removed
  varios minutos despues, lejos de la causa.
- **placed**: en un heap compartido, achicar un recurso deja un hueco y el que
  viene atras sigue en su offset. No se gana memoria y se puede pisar al vecino.
- **viewports**: si el juego setea el viewport con constantes que no pasan por
  `RSSetViewports`, el RT queda a media resolucion y el viewport a resolucion
  completa. Eso se ve como un cuarto de pantalla dibujado.

Cuando un gate rechaza una accion, el motivo queda en el log y en el overlay.
Una accion que no se aplica sin decir por que es peor que una que no existe.

## Que pasa cuando se guarda el archivo

1. Se lee y se parsea entero.
2. Si hay algun error, se escribe el primero en el log con su numero de linea y
   **se sigue con el perfil anterior**.
3. Si esta bien, el perfil nuevo reemplaza al viejo en el limite del frame
   siguiente. Nunca a mitad de una lista grabada: seria un RT a media
   resolucion con el viewport del otro.
4. Las acciones que el usuario haya tocado a mano en el overlay mantienen su
   estado.
