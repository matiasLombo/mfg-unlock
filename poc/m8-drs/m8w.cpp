// M8w: ESCRIBE los overrides de DLSS-G en un perfil de driver y los relee.
//
// Prueba la mitad que m8.exe no podia responder: el driver conoce los settings
// (m8 lo confirmo, devuelve sus nombres oficiales), pero no sabemos si los
// honra en Ada. Para eso hay que ponerles un valor y correr algo.
//
// Alcance deliberadamente chico y reversible:
//   - crea UN perfil propio, "MFG DRS Test", que no existia
//   - le asocia UN ejecutable, el del banco (StreamlineSample.exe)
//   - le pone NGX_DLSSG_MODE=ON y NGX_DLSSG_MULTI_FRAME_COUNT=<n>
//   - relee para confirmar que quedo guardado
// No toca el perfil base ni ningun perfil de NVIDIA.
//
//   m8w.exe                -> escribe modo=ON, count=4 (default)
//   m8w.exe <n>            -> escribe modo=ON, count=<n>
//   m8w.exe limpiar        -> pone todo en 0 y borra el perfil
//
// g++ -std=c++17 -O2 -o m8w.exe m8w.cpp
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

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

// Las versiones de NVDRS_APPLICATION difieren entre drivers. Se prueban de la
// mas nueva a la mas vieja y se usa la primera que el driver acepte.
typedef struct {
    NvU32 version;
    NvU32 isPredefined;
    NvAPI_UnicodeString appName;
    NvAPI_UnicodeString userFriendlyName;
    NvAPI_UnicodeString launcher;
    NvAPI_UnicodeString fileInFolder;
    NvU32 bits;
    NvAPI_UnicodeString commandLine;
} NVDRS_APPLICATION_V4;

typedef struct {
    NvU32 version;
    NvU32 isPredefined;
    NvAPI_UnicodeString appName;
    NvAPI_UnicodeString userFriendlyName;
    NvAPI_UnicodeString launcher;
    NvAPI_UnicodeString fileInFolder;
    NvU32 bits;
} NVDRS_APPLICATION_V3;

typedef struct {
    NvU32 version;
    NvU32 isPredefined;
    NvAPI_UnicodeString appName;
    NvAPI_UnicodeString userFriendlyName;
    NvAPI_UnicodeString launcher;
} NVDRS_APPLICATION_V1;

typedef void* (__cdecl *QI_t)(NvU32);
static QI_t QI = nullptr;
template <typename T> static T res(NvU32 id) { return (T)QI(id); }

typedef NvAPI_Status (__cdecl *Init_t)();
typedef NvAPI_Status (__cdecl *CreateSession_t)(NvDRSSessionHandle*);
typedef NvAPI_Status (__cdecl *DestroySession_t)(NvDRSSessionHandle);
typedef NvAPI_Status (__cdecl *LoadSettings_t)(NvDRSSessionHandle);
typedef NvAPI_Status (__cdecl *SaveSettings_t)(NvDRSSessionHandle);
typedef NvAPI_Status (__cdecl *CreateProfile_t)(NvDRSSessionHandle, NVDRS_PROFILE*, NvDRSProfileHandle*);
typedef NvAPI_Status (__cdecl *DeleteProfile_t)(NvDRSSessionHandle, NvDRSProfileHandle);
typedef NvAPI_Status (__cdecl *FindProfileByName_t)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*);
typedef NvAPI_Status (__cdecl *CreateApplication_t)(NvDRSSessionHandle, NvDRSProfileHandle, void*);
typedef NvAPI_Status (__cdecl *SetSetting_t)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*);
typedef NvAPI_Status (__cdecl *GetSetting_t)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING*);

static const NvU32 ID_MODE  = 0x10308298;   // OFF=1 ON=2 AUTO=3 DYNAMIC=4
static const NvU32 ID_COUNT = 0x104D6667;   // MIN=1 MAX=15
static const char* kPerfil = "MFG DRS Test";
static const char* kExe    = "StreamlineSample.exe";

static void a_ancho(const char* s, NvU16* out) {
    int i = 0;
    for (; s[i] != 0 && i < NVAPI_UNICODE_STRING_MAX - 1; ++i) out[i] = (NvU16)s[i];
    out[i] = 0;
}

int main(int argc, char** argv) {
    bool limpiar = (argc > 1 && strcmp(argv[1], "limpiar") == 0);
    NvU32 count = 4;
    if (argc > 1 && !limpiar) count = (NvU32)atoi(argv[1]);

    HMODULE nv = LoadLibraryA("nvapi64.dll");
    if (nv == nullptr) { printf("M8w: no se pudo cargar nvapi64.dll\n"); return 1; }
    QI = (QI_t)GetProcAddress(nv, "nvapi_QueryInterface");
    if (QI == nullptr) { printf("M8w: no hay nvapi_QueryInterface\n"); return 1; }

    Init_t              Init          = res<Init_t>(0x0150E828);
    CreateSession_t     CreateSession = res<CreateSession_t>(0x0694D52E);
    DestroySession_t    Destroy       = res<DestroySession_t>(0xDAD9CFF8);
    LoadSettings_t      Load          = res<LoadSettings_t>(0x375DBD6B);
    SaveSettings_t      Save          = res<SaveSettings_t>(0xFCBC7E14);
    CreateProfile_t     CreateProfile = res<CreateProfile_t>(0xCC176068);
    DeleteProfile_t     DeleteProfile = res<DeleteProfile_t>(0x17093206);
    FindProfileByName_t FindProfile   = res<FindProfileByName_t>(0x7E4A9A0B);
    CreateApplication_t CreateApp     = res<CreateApplication_t>(0x4347A9DE);
    SetSetting_t        SetSetting    = res<SetSetting_t>(0x577DD202);
    GetSetting_t        GetSetting    = res<GetSetting_t>(0x73BF8338);

    if (!Init || !CreateSession || !Load || !Save || !CreateProfile || !FindProfile
        || !CreateApp || !SetSetting || !GetSetting) {
        printf("M8w: falta alguna funcion DRS de escritura.\n");
        return 1;
    }

    NvAPI_Status st = Init();
    if (st != 0) { printf("NvAPI_Initialize: %d\n", st); return 1; }
    NvDRSSessionHandle ses = nullptr;
    if ((st = CreateSession(&ses)) != 0) { printf("CreateSession: %d\n", st); return 1; }
    if ((st = Load(ses)) != 0) { printf("LoadSettings: %d\n", st); return 1; }

    // "duenio": CreateApplication devolvio -167 (EXECUTABLE_ALREADY_IN_USE), o sea
    // que el ejecutable ya esta en otro perfil. Hay que saber en cual, porque es
    // ese el que el driver va a aplicar.
    if (argc > 1 && strcmp(argv[1], "duenio") == 0) {
        typedef NvAPI_Status (__cdecl *FindApp_t)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*, void*);
        typedef NvAPI_Status (__cdecl *GetInfo_t)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_PROFILE*);
        FindApp_t FindApp = res<FindApp_t>(0xEEE566B2);
        GetInfo_t GetInfo = res<GetInfo_t>(0x61CD6FD6);
        const char* exe = (argc > 2) ? argv[2] : kExe;
        NvU16 wexe[NVAPI_UNICODE_STRING_MAX]; a_ancho(exe, wexe);
        NvDRSProfileHandle duenio = nullptr;
        NVDRS_APPLICATION_V4 a; memset(&a, 0, sizeof(a));
        a.version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        NvAPI_Status r = FindApp ? FindApp(ses, wexe, &duenio, &a) : -1;
        printf("FindApplicationByName(\"%s\"): %d\n", exe, r);
        if (r == 0 && duenio != nullptr && GetInfo != nullptr) {
            NVDRS_PROFILE info; memset(&info, 0, sizeof(info)); info.version = NVDRS_PROFILE_VER;
            char nom[256]; strcpy(nom, "?");
            if (GetInfo(ses, duenio, &info) == 0) {
                int i = 0;
                for (; i < 255 && info.profileName[i] != 0; ++i)
                    nom[i] = (info.profileName[i] < 128) ? (char)info.profileName[i] : '?';
                nom[i] = 0;
            }
            printf("  perfil dueno: \"%s\" (apps=%u settings=%u predef=%u)\n",
                   nom, info.numOfApps, info.numOfSettings, info.isPredefined);
            for (NvU32 id : { ID_MODE, ID_COUNT }) {
                NVDRS_SETTING v; memset(&v, 0, sizeof(v)); v.version = NVDRS_SETTING_VER;
                NvAPI_Status g = GetSetting(ses, duenio, id, &v);
                printf("  0x%08X -> status=%d actual=%u predef=%u tipo=%u loc=%u"
                       " esPredef=%u predefValido=%u idLeido=0x%08X\n",
                       id, g, v.cur.u32CurrentValue, v.pre.u32PredefinedValue,
                       v.settingType, v.settingLocation, v.isCurrentPredefined,
                       v.isPredefinedValid, v.settingId);
            }
        }
        if (Destroy) Destroy(ses);
        return 0;
    }

    // "app <exe> <n>": escribe los settings en el perfil que YA es dueno del
    // ejecutable, que es el unico que el driver va a aplicar. Con n=0 se vuelve
    // al default de NVIDIA (0 = DISABLED / OFF en los dos settings).
    if (argc > 2 && strcmp(argv[1], "app") == 0) {
        typedef NvAPI_Status (__cdecl *FindApp_t)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*, void*);
        FindApp_t FindApp = res<FindApp_t>(0xEEE566B2);
        const char* exe = argv[2];
        NvU32 n = (argc > 3) ? (NvU32)atoi(argv[3]) : 4;
        NvU16 wexe[NVAPI_UNICODE_STRING_MAX]; a_ancho(exe, wexe);
        NvDRSProfileHandle duenio = nullptr;
        NVDRS_APPLICATION_V4 a; memset(&a, 0, sizeof(a));
        a.version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        NvAPI_Status r = FindApp ? FindApp(ses, wexe, &duenio, &a) : -1;
        printf("FindApplicationByName(\"%s\"): %d\n", exe, r);
        if (r != 0 || duenio == nullptr) { printf("M8w: no hay perfil dueno.\n"); return 1; }
        NvU32 modo = (n == 0) ? 0 : 2;   // 0 = default, 2 = ON
        struct { NvU32 id; NvU32 val; } w[] = { { ID_MODE, modo }, { ID_COUNT, n } };
        for (auto& e : w) {
            NVDRS_SETTING v; memset(&v, 0, sizeof(v));
            v.version = NVDRS_SETTING_VER; v.settingId = e.id;
            v.settingType = 0; v.cur.u32CurrentValue = e.val;
            printf("  SetSetting 0x%08X = %u -> %d\n", e.id, e.val, SetSetting(ses, duenio, &v));
        }
        printf("  SaveSettings -> %d\n", Save(ses));
        printf("=== relectura\n");
        for (NvU32 id : { ID_MODE, ID_COUNT }) {
            NVDRS_SETTING v; memset(&v, 0, sizeof(v)); v.version = NVDRS_SETTING_VER;
            printf("  0x%08X -> status=%d actual=%u esPredef=%u\n",
                   id, GetSetting(ses, duenio, id, &v), v.cur.u32CurrentValue, v.isCurrentPredefined);
        }
        if (Destroy) Destroy(ses);
        return 0;
    }

    NvU16 wperfil[NVAPI_UNICODE_STRING_MAX]; a_ancho(kPerfil, wperfil);
    NvDRSProfileHandle perfil = nullptr;
    NvAPI_Status buscado = FindProfile(ses, wperfil, &perfil);
    printf("FindProfileByName(\"%s\"): %d\n", kPerfil, buscado);

    if (limpiar) {
        if (buscado != 0 || perfil == nullptr) { printf("M8w: no hay nada que limpiar.\n"); return 0; }
        for (NvU32 id : { ID_MODE, ID_COUNT }) {
            NVDRS_SETTING v; memset(&v, 0, sizeof(v));
            v.version = NVDRS_SETTING_VER; v.settingId = id;
            v.settingType = 0; v.cur.u32CurrentValue = 0;
            printf("  SetSetting 0x%08X = 0 -> %d\n", id, SetSetting(ses, perfil, &v));
        }
        printf("  DeleteProfile -> %d\n", DeleteProfile ? DeleteProfile(ses, perfil) : -1);
        printf("  SaveSettings -> %d\n", Save(ses));
        if (Destroy) Destroy(ses);
        printf("M8w: perfil limpiado.\n");
        return 0;
    }

    if (buscado != 0 || perfil == nullptr) {
        NVDRS_PROFILE np; memset(&np, 0, sizeof(np));
        np.version = NVDRS_PROFILE_VER;
        a_ancho(kPerfil, np.profileName);
        np.gpuSupport = 1;   // geforce
        st = CreateProfile(ses, &np, &perfil);
        printf("CreateProfile: %d\n", st);
        if (st != 0) { printf("M8w: no se pudo crear el perfil.\n"); return 1; }
    }

    // La app: se prueban las versiones de NVDRS_APPLICATION de mas nueva a mas vieja.
    {
        NVDRS_APPLICATION_V4 a4; memset(&a4, 0, sizeof(a4));
        a4.version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V4, 4);
        a4.isPredefined = 0; a_ancho(kExe, a4.appName); a_ancho(kExe, a4.userFriendlyName);
        st = CreateApp(ses, perfil, &a4);
        printf("CreateApplication v4: %d\n", st);
        if (st != 0) {
            NVDRS_APPLICATION_V3 a3; memset(&a3, 0, sizeof(a3));
            a3.version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V3, 3);
            a_ancho(kExe, a3.appName); a_ancho(kExe, a3.userFriendlyName);
            st = CreateApp(ses, perfil, &a3);
            printf("CreateApplication v3: %d\n", st);
        }
        if (st != 0) {
            NVDRS_APPLICATION_V1 a1; memset(&a1, 0, sizeof(a1));
            a1.version = MAKE_NVAPI_VERSION(NVDRS_APPLICATION_V1, 1);
            a_ancho(kExe, a1.appName); a_ancho(kExe, a1.userFriendlyName);
            st = CreateApp(ses, perfil, &a1);
            printf("CreateApplication v1: %d\n", st);
        }
        // st != 0 puede ser simplemente que la app ya estaba asociada.
    }

    struct { NvU32 id; NvU32 val; const char* nombre; } escrituras[] = {
        { ID_MODE,  2,     "NGX_DLSSG_MODE=ON" },
        { ID_COUNT, count, "NGX_DLSSG_MULTI_FRAME_COUNT" },
    };
    for (auto& e : escrituras) {
        NVDRS_SETTING v; memset(&v, 0, sizeof(v));
        v.version = NVDRS_SETTING_VER;
        v.settingId = e.id;
        v.settingType = 0;              // NVDRS_DWORD_TYPE
        v.cur.u32CurrentValue = e.val;
        NvAPI_Status r = SetSetting(ses, perfil, &v);
        printf("SetSetting 0x%08X (%s) = %u -> %d\n", e.id, e.nombre, e.val, r);
    }

    st = Save(ses);
    printf("SaveSettings: %d\n", st);

    printf("\n=== relectura del perfil\n");
    for (NvU32 id : { ID_MODE, ID_COUNT }) {
        NVDRS_SETTING v; memset(&v, 0, sizeof(v)); v.version = NVDRS_SETTING_VER;
        NvAPI_Status r = GetSetting(ses, perfil, id, &v);
        printf("  0x%08X -> status=%d actual=%u loc=%u esPredef=%u\n",
               id, r, v.cur.u32CurrentValue, v.settingLocation, v.isCurrentPredefined);
    }

    if (Destroy) Destroy(ses);
    printf("\nM8w: listo. Para revertir: m8w.exe limpiar\n");
    return 0;
}
