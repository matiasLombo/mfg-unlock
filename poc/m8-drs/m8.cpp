// M8: los overrides de DLSS-G como ajuste de perfil del driver (DRS).
//
// Hipotesis: el multiplicador, el modo (incluido DYNAMIC) y el target de fps de
// DLSS-G son settings del driver por perfil de aplicacion, no algo que haya que
// parchear en la llamada de cada juego. Si el driver los honra en Ada, se cae la
// razon por la que hoy hace falta un fix por juego.
//
// Los IDs salen del SDK publico de NVIDIA (NvApiDriverSettings.h):
//   0x10308298 NGX_DLSSG_MODE                        OFF=1 ON=2 AUTO=3 DYNAMIC=4
//   0x104D6667 NGX_DLSSG_MULTI_FRAME_COUNT           MIN=1 MAX=15
//   0x10562D0F NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX
//   0x10CF4125 NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE   AUTO=0x01000000
//
// ESTA POC NO ESCRIBE NADA. Solo lee: recorre los perfiles del driver y dice
// cuales traen estos settings y con que valor. Escribirlos cambia configuracion
// persistente de la maquina y es una decision aparte.
//
// Criterio de rechazo: si NvAPI_DRS_GetSetting no encuentra el setting en ningun
// perfil, o si el driver no resuelve las funciones DRS, la hipotesis se cae aca
// y no hay que probar nada mas.
//
// g++ -std=c++17 -O2 -o m8.exe m8.cpp
#include <windows.h>
#include <cstdio>
#include <cstring>

typedef unsigned int   NvU32;
typedef unsigned short NvU16;
typedef unsigned char  NvU8;
typedef int            NvAPI_Status;

#define NVAPI_UNICODE_STRING_MAX 2048
#define NVAPI_BINARY_DATA_MAX    4096
#define MAKE_NVAPI_VERSION(t, v) (NvU32)(sizeof(t) | ((v) << 16))

typedef NvU16 NvAPI_UnicodeString[NVAPI_UNICODE_STRING_MAX];
typedef void* NvDRSSessionHandle;
typedef void* NvDRSProfileHandle;

typedef struct { NvU32 valueLength; NvU8 valueData[NVAPI_BINARY_DATA_MAX]; } NVDRS_BINARY_SETTING;

typedef struct _NVDRS_SETTING_V1 {
    NvU32 version;
    NvAPI_UnicodeString settingName;
    NvU32 settingId;
    NvU32 settingType;
    NvU32 settingLocation;
    NvU32 isCurrentPredefined;
    NvU32 isPredefinedValid;
    union { NvU32 u32PredefinedValue; NVDRS_BINARY_SETTING b; NvAPI_UnicodeString w; } pre;
    union { NvU32 u32CurrentValue;    NVDRS_BINARY_SETTING b; NvAPI_UnicodeString w; } cur;
} NVDRS_SETTING;
#define NVDRS_SETTING_VER MAKE_NVAPI_VERSION(NVDRS_SETTING, 1)

typedef struct _NVDRS_PROFILE_V1 {
    NvU32 version;
    NvAPI_UnicodeString profileName;
    NvU32 gpuSupport;
    NvU32 isPredefined;
    NvU32 numOfApps;
    NvU32 numOfSettings;
} NVDRS_PROFILE;
#define NVDRS_PROFILE_VER MAKE_NVAPI_VERSION(NVDRS_PROFILE, 1)

typedef void* (__cdecl *QI_t)(NvU32);
static QI_t QI = nullptr;
template <typename T> static T res(NvU32 id) { return (T)QI(id); }

typedef NvAPI_Status (__cdecl *Init_t)();
typedef NvAPI_Status (__cdecl *CreateSession_t)(NvDRSSessionHandle*);
typedef NvAPI_Status (__cdecl *DestroySession_t)(NvDRSSessionHandle);
typedef NvAPI_Status (__cdecl *LoadSettings_t)(NvDRSSessionHandle);
typedef NvAPI_Status (__cdecl *GetBaseProfile_t)(NvDRSSessionHandle, NvDRSProfileHandle*);
typedef NvAPI_Status (__cdecl *EnumProfiles_t)(NvDRSSessionHandle, NvU32, NvDRSProfileHandle*);
typedef NvAPI_Status (__cdecl *GetProfileInfo_t)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_PROFILE*);
typedef NvAPI_Status (__cdecl *GetSetting_t)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING*);

struct Conocido { NvU32 id; const char* nombre; };
static const Conocido kSettings[] = {
    { 0x10308298, "NGX_DLSSG_MODE" },
    { 0x104D6667, "NGX_DLSSG_MULTI_FRAME_COUNT" },
    { 0x10562D0F, "NGX_DLSSG_DYN_MULTI_FRAME_COUNT_MAX" },
    { 0x10CF4125, "NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE" },
};

static void ancho_a_ascii(const NvU16* w, char* out, int max) {
    int i = 0;
    for (; i < max - 1 && w[i] != 0; ++i) out[i] = (w[i] < 128) ? (char)w[i] : '?';
    out[i] = 0;
}

int main(void) {
    HMODULE nv = LoadLibraryA("nvapi64.dll");
    if (nv == nullptr) { printf("M8: no se pudo cargar nvapi64.dll\n"); return 1; }
    QI = (QI_t)GetProcAddress(nv, "nvapi_QueryInterface");
    if (QI == nullptr) { printf("M8: no hay nvapi_QueryInterface\n"); return 1; }

    Init_t           Init           = res<Init_t>(0x0150E828);
    CreateSession_t  CreateSession  = res<CreateSession_t>(0x0694D52E);
    DestroySession_t DestroySession = res<DestroySession_t>(0xDAD9CFF8);
    LoadSettings_t   LoadSettings   = res<LoadSettings_t>(0x375DBD6B);
    GetBaseProfile_t GetBaseProfile = res<GetBaseProfile_t>(0xDA8466A0);
    EnumProfiles_t   EnumProfiles   = res<EnumProfiles_t>(0xBC371EE0);
    GetProfileInfo_t GetProfileInfo = res<GetProfileInfo_t>(0x61CD6FD6);
    GetSetting_t     GetSetting     = res<GetSetting_t>(0x73BF8338);

    printf("M8 punteros: Init=%p Create=%p Load=%p Base=%p Enum=%p Info=%p Get=%p\n",
           (void*)Init, (void*)CreateSession, (void*)LoadSettings,
           (void*)GetBaseProfile, (void*)EnumProfiles, (void*)GetProfileInfo, (void*)GetSetting);
    if (!Init || !CreateSession || !LoadSettings || !EnumProfiles || !GetProfileInfo || !GetSetting) {
        printf("M8: el driver no expone alguna funcion DRS. La hipotesis se cae aca.\n");
        return 1;
    }
    printf("M8 sizeof(NVDRS_SETTING)=%zu ver=0x%08X  sizeof(NVDRS_PROFILE)=%zu ver=0x%08X\n",
           sizeof(NVDRS_SETTING), NVDRS_SETTING_VER, sizeof(NVDRS_PROFILE), NVDRS_PROFILE_VER);

    NvAPI_Status st = Init();
    printf("NvAPI_Initialize: %d\n", st);
    if (st != 0) return 1;

    NvDRSSessionHandle ses = nullptr;
    st = CreateSession(&ses);
    printf("DRS_CreateSession: %d\n", st);
    if (st != 0) return 1;
    st = LoadSettings(ses);
    printf("DRS_LoadSettings: %d\n", st);
    if (st != 0) return 1;

    printf("\n=== perfil base (global)\n");
    NvDRSProfileHandle base = nullptr;
    if (GetBaseProfile != nullptr && GetBaseProfile(ses, &base) == 0) {
        int hallados = 0;
        for (int k = 0; k < 4; ++k) {
            NVDRS_SETTING v; memset(&v, 0, sizeof(v)); v.version = NVDRS_SETTING_VER;
            if (GetSetting(ses, base, kSettings[k].id, &v) != 0) continue;
            char nom[256]; ancho_a_ascii(v.settingName, nom, sizeof(nom));
            printf("  0x%08X %-38s actual=%u (0x%08X) predef=%u tipo=%u loc=%u esPredef=%u \"%s\"\n",
                   kSettings[k].id, kSettings[k].nombre,
                   v.cur.u32CurrentValue, v.cur.u32CurrentValue,
                   v.pre.u32PredefinedValue, v.settingType, v.settingLocation,
                   v.isCurrentPredefined, nom);
            hallados++;
        }
        if (hallados == 0) printf("  (ninguno de los cuatro esta en el perfil base)\n");
    } else {
        printf("  (no se pudo obtener el perfil base)\n");
    }

    printf("\n=== perfiles que traen alguno de los cuatro settings\n");
    NvU32 total = 0, conAlguno = 0;
    for (NvU32 i = 0; ; ++i) {
        NvDRSProfileHandle p = nullptr;
        if (EnumProfiles(ses, i, &p) != 0) break;
        total++;
        NVDRS_PROFILE info; memset(&info, 0, sizeof(info)); info.version = NVDRS_PROFILE_VER;
        char nom[256]; strcpy(nom, "?");
        if (GetProfileInfo(ses, p, &info) == 0) ancho_a_ascii(info.profileName, nom, sizeof(nom));
        bool alguno = false;
        for (int k = 0; k < 4; ++k) {
            NVDRS_SETTING v; memset(&v, 0, sizeof(v)); v.version = NVDRS_SETTING_VER;
            if (GetSetting(ses, p, kSettings[k].id, &v) != 0) continue;
            // Solo interesa lo que NO es el default: valor distinto de cero, o
            // un predefinido que NVIDIA haya puesto en ese perfil.
            if (v.cur.u32CurrentValue == 0 && v.pre.u32PredefinedValue == 0) continue;
            if (!alguno) {
                printf("  perfil \"%s\" (apps=%u settings=%u predef=%u)\n",
                       nom, info.numOfApps, info.numOfSettings, info.isPredefined);
                alguno = true;
            }
            printf("      0x%08X %-38s actual=%u (0x%08X) predef=%u esPredef=%u\n",
                   kSettings[k].id, kSettings[k].nombre,
                   v.cur.u32CurrentValue, v.cur.u32CurrentValue,
                   v.pre.u32PredefinedValue, v.isCurrentPredefined);
        }
        if (alguno) conAlguno++;
    }
    printf("\nM8 RESUMEN: %u perfiles recorridos, %u traen alguno de los cuatro settings.\n",
           total, conAlguno);

    if (DestroySession != nullptr) DestroySession(ses);
    return 0;
}
