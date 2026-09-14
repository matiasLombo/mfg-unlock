# De donde sale cada decision

El objetivo pedia estudiar cuatro referencias antes de disenar. Lo que sigue es
que se tomo de cada una, que se dejo afuera a proposito, y donde eso se ve en
el codigo.

Una aclaracion de metodo que corresponde: esto se escribio a partir de conocer
las tecnicas de esos proyectos, no leyendo sus fuentes en esta sesion -- el
entorno donde se construyo gpuprobe no tiene acceso a esos repositorios. Donde
una decision depende de un detalle que habria que confirmar mirando el codigo
ajeno, esta dicho abajo.

## Special K -- el wrapper de swapchain

**Lo que se tomo.** La idea de que el punto de entrada util no es el device
sino la swapchain: es lo unico que todo juego crea, es donde esta el frame, y
de ella cuelga todo lo demas. Y el rigor con el Present: hacer el trabajo
propio ANTES del Present real cuando se quiere dibujar, y DESPUES cuando se
quiere medir el frame que se fue.

**Lo que se dejo afuera.** Special K envuelve los objetos COM con wrappers
propios (un `IDXGISwapChain` de mentira que reenvia). Es mas robusto frente a
otras capas, pero es mucho mas codigo y tiene un costo por llamada en el hot
path. gpuprobe parcha la vtable en su lugar, que es mas barato y mas frágil,
y compensa la fragilidad con la tabla de vtables y el degradado.

**Donde se ve.** `d3d12/hooks.cpp` (`hooks_attach_swapchain`, `hk_Present`).

## 3Dmigoto -- reemplazo por hash

**Lo que se tomo.** La disciplina de tener una clave estable por recurso,
derivada del contenido y no del puntero, para poder hablar del mismo objeto
entre dos corridas.

**Lo que se rechazo, y es la decision mas importante del proyecto.** 3Dmigoto
identifica por hash de shader porque su caso de uso es reemplazar shaders. Para
optimizar pasadas eso es lo peor que se puede elegir: un parche del juego
recompila shaders y mueve todos los hashes, y el perfil entero deja de
matchear. gpuprobe matchea por DESCRIPTOR normalizado -- un shadow map sigue
siendo un D32 de 4096 en array de 4 despues de un update -- y guarda el hash
del bytecode solo para medir y comparar sesiones.

La contracara esta asumida y escrita en el reporte: el descriptor identifica
una CLASE, asi que cuando varios recursos comparten descriptor, la accion los
toca a todos y el analizador lo avisa.

**Donde se ve.** `core/keys.h` (`desc_key`, `pso_key`) y la regla de ambiguedad
en `analyzer/gpuprobe_analyzer/rules.py`.

## OptiScaler -- Quirks.h

**Lo que se tomo.** Que la realidad son los casos especiales por juego, y que
conviene que vivan en un lugar declarativo en vez de repartidos en `if`s por
todo el codigo.

**Lo que cambia.** Un `Quirks.h` compilado adentro del binario obliga a
recompilar y redistribuir por cada juego nuevo. En gpuprobe el equivalente es
el perfil TOML por juego, en `%LOCALAPPDATA%`, con hot-reload. Un juego nuevo
es un archivo de texto, no un release.

Y una diferencia de fondo: los quirks de OptiScaler son lo que hay que hacer
para que ande; las acciones de gpuprobe son lo que se puede probar. Por eso
toda accion nace apagada, entra al harness A/B y se queda solo si la ganancia
se mide.

**Donde se ve.** `core/profile.h`, `docs/perfil.md`, `d3d12/executor.cpp`
(hot-reload).

## PIX y Nsight Graphics GPU Trace -- que exponen y como agrupan

**Lo que se tomo.**

- **El corte entre pasadas.** Los dos agrupan por cambio de render targets
  cuando el juego no marca nada. gpuprobe hace lo mismo, y agrega el caso que
  esa regla sola no cubre: una command list de compute nunca llama
  `OMSetRenderTargets`, asi que su pasada empieza en el primer dispatch y su
  identidad es el PSO. (Ese hueco lo encontro el testbed, no el diseno.)
- **Timestamps por rango con resolucion diferida**, sin esperar a la GPU.
- **Mostrar el solapamiento en vez de esconderlo.** GPU Trace muestra las
  queues en carriles separados justamente porque la suma no es el frame.
  gpuprobe no dibuja carriles, pero reporta suma, union y solapamiento por
  separado, y limita la ganancia de un candidato a su tiempo exclusivo.

**Lo que no se intenta.** Contadores de hardware (ocupacion, cache hit rate,
SM activo). Necesitan NvPerf o la SDK del vendor, una dependencia externa que
el proyecto no quiere, y responden una pregunta distinta: por que una pasada es
lenta. gpuprobe contesta cual pasada esta mal dimensionada, que es una pregunta
mas chica y accionable desde afuera del juego.

## Este mismo repositorio -- mfg-unlock

La referencia mas util resulto ser la de al lado. Tres cosas se tomaron tal
cual, y las tres estan pagadas con crashes ajenos:

- **Tabla de vtables, no "la" vtable** (`docs/present-por-vtable.diff`): un
  juego puede tener dos vtables vivas para la misma interfaz. Guardar un solo
  "original" hace que la segunda llame al de la primera, y eso se ve recien
  cuando el usuario hace alt-tab.
- **Robar la vtable con un objeto descartable propio** (README, "adoptar por un
  descartable D3D12"): la vtable vive en el runtime y es la misma para todo el
  proceso. Evita necesitar un trampolin y, de paso, MinHook.
- **El testigo de excepciones** (`src/exceptions.h`): un VEH que mira quien
  causo la falla, no la traga y no intenta recuperarse. En gpuprobe ademas
  apaga gpuprobe si la falla fue nuestra.
