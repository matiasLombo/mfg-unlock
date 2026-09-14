// executor.h -- el envoltorio de Windows alrededor de ExecutorCore: leer el
// perfil, recargarlo en caliente, y recordar entre sesiones lo observado.
//
// ENTRA: %LOCALAPPDATA%\gpuprobe\profiles\<exe>.toml (y lo que el colector
//   observa durante la sesion).
// SALE: decisiones, via la interfaz Actions que consulta el colector.
// DEPENDE DE: core/executor_core.h, windows.h.
//
// Nada se escribe nunca en la carpeta del juego: perfil, libreta de seguridad
// y sesiones viven todas en %LOCALAPPDATA%\gpuprobe.
#pragma once

#include "core/executor_core.h"
#include "d3d12/collector.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace gp {

class Executor final : public Actions {
public:
    // exe es el nombre del ejecutable del juego: decide que perfil se carga.
    bool init(const char *exe, OutputInfo out);
    void shutdown();

    // --- Actions ----------------------------------------------------------
    ResourceOverride on_create(const ResourceDesc &, CallsiteId) override;
    void note_resource(DescKey, const ResourceDesc &, bool placed,
                       bool reserved) override;
    void note_copy(DescKey, bool as_source) override;
    void note_viewport(DescKey) override;
    bool skip_pass(PassKey, DescKey rt) override;
    bool drop_barrier(u32 before, u32 after) override;
    void begin_frame(u64 index) override;
    const char *capture_due(u64 index) override;

    // --- overlay ----------------------------------------------------------
    ExecutorCore &core() { return core_; }
    const std::string &profile_path() const { return profile_path_; }
    const std::string &last_error() const { return last_error_; }
    u64  reloads() const { return reloads_; }
    void force(u64 action_id, bool on);

private:
    void load_profile(bool first);
    void load_ledger();
    void save_ledger();

    ExecutorCore core_;
    std::mutex   mu_;          // el perfil se recarga desde el limite de frame
    std::string  profile_path_;
    std::string  ledger_path_;
    std::string  last_error_;
    u64          last_write_ = 0;
    u64          reloads_ = 0;
    u64          last_check_frame_ = 0;
    bool         ledger_dirty_ = false;
    bool         armed_ = false;
    // Estado de captura por accion: cuando cambio, y si ya capturamos esa
    // combinacion de (accion, estado).
    struct CapState { bool on = false; u64 changed = 0; bool shot_on = false;
                      bool shot_off = false; };
    std::unordered_map<u64, CapState> caps_;
    char         capture_suffix_[32] = {};
};

// El ejecutor del proceso, como el colector: uno solo.
Executor &executor();

}  // namespace gp
