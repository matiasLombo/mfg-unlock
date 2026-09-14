// collector.h -- la capa que toca D3D12: registra recursos, PSOs y pasadas, y
// mide tiempo de GPU sin frenar al juego.
//
// ENTRA: los hooks (hooks.cpp) o, en el testbed, la app llamando directo.
// SALE: eventos al ring, y de ahi al JSONL por el hilo escritor.
// DEPENDE DE: d3d12.h, dxgi1_6.h, core/*.
//
// La API es la MISMA para los hooks y para el testbed, y eso es deliberado:
// el testbed la llama directo y corre sobre WARP en CI, asi que el colector se
// prueba de verdad -- con un device, un query heap y una fence reales -- en
// vez de solo compilar. Un colector que solo se prueba dentro de un juego se
// prueba una vez por semana y con suerte.
//
// Reglas del hot path, todas verificables leyendo el .cpp:
//   - no se aloca
//   - no se toma ningun lock salvo en cmd_begin/cmd_close (una vez por lista)
//   - no se hashea (el bytecode se copia a una arena y se hashea afuera)
//   - no se escribe a disco
//   - no se espera a la GPU NUNCA: los timestamps se leen tres frames despues,
//     cuando la fence de esa ejecucion ya paso
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include "core/events.h"
#include "core/keys.h"
#include "core/ring.h"

namespace gp {

struct CollectorConfig {
    // Cuantas queries de timestamp por frame en modo light. Cada pasada usa 2.
    u32  query_budget = 64;
    // Uno de cada cuantos frames se instrumenta entero.
    u32  deep_every = 120;
    // Ruta del JSONL. Si esta vacia, se arma en %LOCALAPPDATA%\gpuprobe.
    const wchar_t *session_path = nullptr;
    const char    *exe_name = "desconocido.exe";
};

// Lo que el ejecutor devuelve cuando se va a crear un recurso. Neutro por
// defecto: el colector solo observa.
struct ResourceOverride {
    u32  w = 0;        // 0 = no tocar
    u32  h = 0;
    i8   mip_bias = 0;
    bool deny = false; // no crear (no se usa todavia; queda explicito)
};

// Interfaz que implementa el ejecutor. El colector la consulta; si es nullptr,
// gpuprobe es puro OBSERVE.
//
// Las note_* no son opcionales ni decorativas: son los hechos que despues
// habilitan (o no) una accion. Un ejecutor que no se entera de que a un
// recurso lo copian con extents fijos es un ejecutor que lo va a escalar.
struct Actions {
    virtual ~Actions() = default;
    virtual ResourceOverride on_create(const ResourceDesc &, CallsiteId) = 0;
    // El sesgo de mip para ESTE recurso, si hay una accion activa. Va aparte
    // de on_create porque se pregunta en otro momento (al crear la vista) y
    // porque un recurso puede matchear las dos acciones.
    virtual i8 mip_bias_for(const ResourceDesc &) { return 0; }
    virtual void note_resource(DescKey, const ResourceDesc &, bool placed,
                               bool reserved) = 0;
    virtual void note_copy(DescKey, bool as_source) = 0;
    virtual void note_viewport(DescKey) = 0;
    virtual bool skip_pass(PassKey, DescKey rt) = 0;
    virtual bool drop_barrier(u32 before, u32 after) = 0;
    // Limite de frame: el unico momento en el que las acciones cambian de
    // estado. A mitad de una lista grabada seria un RT a media resolucion con
    // el viewport de la otra.
    virtual void begin_frame(u64 index) = 0;
    // Devuelve un sufijo ("a3-on") cuando corresponde sacar una captura, o
    // nullptr. El costo visual no se mide: se mira, y para mirarlo hacen falta
    // las dos imagenes del mismo punto.
    virtual const char *capture_due(u64 index) { (void)index; return nullptr; }
};

class Collector {
public:
    // Devuelve false si algo no se pudo crear. Fallar aca NO es fatal: el
    // llamador queda en passthrough y el juego ni se entera.
    bool init(ID3D12Device *device, const CollectorConfig &cfg);
    void shutdown();

    bool armed() const { return armed_; }
    void set_actions(Actions *a) { actions_ = a; }
    void set_output(u32 w, u32 h);
    OutputInfo output() const { return out_; }

    // --- recursos ---------------------------------------------------------
    // desc/heap vienen tal cual de la llamada interceptada. site es el hash de
    // la pila de creacion (0 si no se pudo sacar).
    void on_resource(ID3D12Resource *res, const D3D12_RESOURCE_DESC &desc,
                     D3D12_HEAP_TYPE heap, bool placed, CallsiteId site);
    void on_resource_release(ID3D12Resource *res);
    // Registra los buffers del swapchain como recursos marcados. Sin esto, el
    // backbuffer aparece como un RT full-res que se escribe y nunca se lee --
    // que es exactamente la forma de un recurso huerfano -- y el reporte lo
    // propondria en todos los juegos.
    void note_backbuffers(IDXGISwapChain *swapchain);
    // El ejecutor decide si el recurso se crea mas chico. Se llama ANTES de
    // crearlo; devuelve el override ya validado por los gates.
    ResourceOverride want_override(const D3D12_RESOURCE_DESC &desc,
                                   D3D12_HEAP_TYPE heap, CallsiteId site);

    // --- vistas -----------------------------------------------------------
    // Sin esto no se sabe que recurso hay detras de un RTV al momento del
    // OMSetRenderTargets: el handle es un puntero a un descriptor, no al
    // recurso.
    void on_rtv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res);
    void on_dsv(D3D12_CPU_DESCRIPTOR_HANDLE h, ID3D12Resource *res);
    // mip_bias: si hay una accion activa para este recurso, devuelve true y
    // deja en out una vista que empieza un mip mas abajo. Es la unica forma
    // honesta de aplicar un sesgo de mip desde afuera: cambiar el descriptor
    // del RECURSO cambiaria lo que el juego cree que creo. Ojo con lo que
    // ahorra: menos ancho de banda y menos presion de cache, NO menos VRAM --
    // los mips grandes siguen residentes.
    bool srv_override(ID3D12Resource *res,
                      const D3D12_SHADER_RESOURCE_VIEW_DESC *in,
                      D3D12_SHADER_RESOURCE_VIEW_DESC *out);

    // --- PSOs -------------------------------------------------------------
    PsoKey on_graphics_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                           ID3D12PipelineState *pso, u64 compile_ns,
                           bool render_thread);
    PsoKey on_compute_pso(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc,
                          ID3D12PipelineState *pso, u64 compile_ns,
                          bool render_thread);

    // --- grabacion de command lists --------------------------------------
    void cmd_begin(ID3D12GraphicsCommandList *list);
    void cmd_set_render_targets(ID3D12GraphicsCommandList *list, u32 n,
                                const D3D12_CPU_DESCRIPTOR_HANDLE *rtvs,
                                BOOL single_handle,
                                const D3D12_CPU_DESCRIPTOR_HANDLE *dsv);
    void cmd_set_pso(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso);
    // Devuelven true si el juego TIENE que ejecutar la llamada. Un false es
    // un skip_pass activo. El testbed ignora el resultado; el hook no.
    bool cmd_draw(ID3D12GraphicsCommandList *list, u32 count, u32 instances);
    bool cmd_dispatch(ID3D12GraphicsCommandList *list, u32 x, u32 y, u32 z);
    // Registra los barriers y devuelve cuantos hay que dejar pasar, ya
    // copiados a out (que tiene que tener lugar para n). Sin ejecutor activo
    // devuelve n y copia todo: filtrar es una accion, observar no.
    u32 cmd_barrier(ID3D12GraphicsCommandList *list, u32 n,
                    const D3D12_RESOURCE_BARRIER *barriers,
                    D3D12_RESOURCE_BARRIER *out = nullptr);
    // Los viewports importan por dos motivos: sin verlos no se puede escalar
    // un RT (el gate viewports_tracked), y cuando se escala hay que escalarlos
    // tambien. Devuelve true si el llamador tiene que usar 'out'.
    bool cmd_viewports(ID3D12GraphicsCommandList *list, u32 n,
                       const D3D12_VIEWPORT *in, D3D12_VIEWPORT *out);
    void cmd_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *dst,
                  ID3D12Resource *src);
    void cmd_close(ID3D12GraphicsCommandList *list);

    // --- ejecucion y presentacion ----------------------------------------
    void on_execute(ID3D12CommandQueue *queue, u32 n,
                    ID3D12CommandList *const *lists);
    // Se llama DESPUES del Present real. Cierra el frame, cobra los
    // timestamps que ya esten listos y emite el evento de VRAM.
    void on_present(IDXGISwapChain *swapchain);

    // El adaptador, para el budget de VRAM. Opcional: sin el, no hay evento
    // de VRAM y el analizador lo dice en vez de inventarlo.
    void set_adapter(IDXGIAdapter3 *adapter);

    // Residencia: el juego pidiendo o soltando memoria a mano. Se cuenta por
    // frame y sale en el JSONL.
    void note_residency(bool evicting, u32 count);

    u64  frame_index() const { return frame_; }
    bool deep_frame() const { return deep_; }

    // --- lo que mira el overlay ------------------------------------------
    // Una fila por pasada viva, con su costo suavizado. No es para analizar
    // -- para eso esta el JSONL y el analizador -- es para poder ver en vivo
    // que se movio al prender un candidato.
    struct PassLive {
        PassKey key;
        DescKey rt_dkey;
        double  gpu_ms = 0.0;
        u32     draws = 0;
        u32     rt_w = 0, rt_h = 0;
        u32     queue = 0;
        u64     last_frame = 0;
    };
    // Copia ordenada por costo. Toma un lock: el overlay corre una vez por
    // frame en el hilo de presentacion, no en el de grabacion.
    void snapshot(PassLive *out, size_t max, size_t *count) const;
    double frame_ms() const;    // mediana movil del frametime
    void   vram_now(u64 *budget, u64 *usage) const;

    // Deja constancia de que una accion se prendio o se apago. Es lo que
    // despues le permite al analizador partir el frametime en dos condiciones
    // y medir la ganancia; sin estos eventos, el harness A/B no existe.
    void note_action(u64 action_id, DescKey target, u32 kind, bool on);

    // Publica solo para que los helpers de collector.cpp la vean; nadie de
    // afuera la incluye (esta declarada, no definida, en este header).
    struct Impl;

private:
    Impl *impl_ = nullptr;
    Actions *actions_ = nullptr;
    OutputInfo out_{};
    u64  frame_ = 0;
    bool deep_ = false;
    bool armed_ = false;
};

// El colector del proceso. Uno solo: los hooks no tienen donde guardar un
// puntero y el testbed no necesita dos.
Collector &collector();

// Hash de la pila de llamadas de quien esta creando un recurso. Devuelve 0 si
// no se pudo: un callsite ausente es visible en el JSONL, uno inventado no.
CallsiteId capture_callsite();

}  // namespace gp
