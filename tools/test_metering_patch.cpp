#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

static int patch_metering_off(unsigned char *base) {
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *text = nullptr;
    size_t len = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const char *n = reinterpret_cast<const char *>(sec[i].Name);
        if (n[0]=='.'&&n[1]=='t'&&n[2]=='e'&&n[3]=='x'&&n[4]=='t') { text=base+sec[i].VirtualAddress; len=sec[i].Misc.VirtualSize; break; }
    }
    if (text == nullptr) return 0;
    unsigned field = 0;
    for (size_t i = 0; i + 22 <= len; ++i) {
        if (text[i]!=0x83||text[i+1]!=0xFA||text[i+2]!=0x1E) continue;
        if (text[i+3]!=0x41||text[i+4]!=0x0F||text[i+5]!=0x93) continue;
        if (text[i+7]!=0x41||text[i+8]!=0x80||(text[i+9]&0xC7)!=0x86) continue;
        if (text[i+14]!=0x00) continue;
        if (text[i+15]!=0x0F||text[i+16]!=0x94||text[i+17]!=0xC0) continue;
        field = *reinterpret_cast<const unsigned*>(text+i+10);
        break;
    }
    if (field == 0) {
        for (size_t i = 0; i + 14 <= len; ++i) {
            if (text[i]!=0x41||text[i+1]!=0x0F||text[i+2]!=0xB6||text[i+3]!=0x9E) continue;
            if (text[i+8]!=0x84||text[i+9]!=0xDB) continue;
            if (text[i+10]!=0x40||text[i+11]!=0x0F||text[i+12]!=0x94||text[i+13]!=0xC6) continue;
            field = *reinterpret_cast<const unsigned*>(text+i+4);
            break;
        }
    }
    static const unsigned kKnown[] = { 0x44A0, 0x44F8 };
    if (field == 0) {
        for (unsigned k : kKnown) {
            for (size_t i = 0; i + 13 <= len && field == 0; ++i) {
                if (text[i]!=0xC6||text[i+1]!=0x83) continue;
                if (*reinterpret_cast<const unsigned*>(text+i+2)!=k) continue;
                if (text[i+6]!=0x00) continue;
                if (text[i+7]!=0x80||text[i+8]!=0x3D) continue;
                field = k;
            }
            if (field != 0) break;
        }
        if (field == 0) { std::printf("  (metering field not located)\n"); return 0; }
    }
    std::printf("  metering field at ctx+%u (0x%X)\n", field, field);
    int hits = 0;
    for (size_t i = 0; i + 13 <= len; ++i) {
        if (text[i]!=0xC6||text[i+1]!=0x83) continue;
        if (*reinterpret_cast<const unsigned*>(text+i+2)!=field) continue;
        if (text[i+6]!=0x00) continue;
        if (text[i+7]!=0x80||text[i+8]!=0x3D) continue;
        unsigned char *imm = text+i+6;
        DWORD old=0; if(!VirtualProtect(imm,1,PAGE_EXECUTE_READWRITE,&old)) continue;
        *imm = 1; VirtualProtect(imm,1,old,&old); ++hits;
    }
    if (hits != 0) return hits;
    for (size_t i = 0; i + 7 <= len; ++i) {
        if (text[i]!=0x40||text[i+1]!=0x88) continue;
        if ((text[i+2]&0xC7)!=0x83) continue;
        if (*reinterpret_cast<const unsigned*>(text+i+3)!=field) continue;
        unsigned char *disp = text+i+3;
        DWORD old=0; if(!VirtualProtect(disp,4,PAGE_EXECUTE_READWRITE,&old)) continue;
        *reinterpret_cast<unsigned*>(disp) = field+4;
        VirtualProtect(disp,4,old,&old); ++hits;
    }
    if (hits == 0) std::printf("  (field found, no store form recognised)\n");
    return hits;
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: %s <sl.dlss_g.dll>\n", argv[0]); return 1; }
    wchar_t wpath[MAX_PATH]={0};
    MultiByteToWideChar(CP_ACP,0,argv[1],-1,wpath,MAX_PATH);
    HMODULE h = LoadLibraryExW(wpath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (h == nullptr) { std::printf("LoadLibraryExW failed %lu\n", GetLastError()); return 2; }
    int n = patch_metering_off(reinterpret_cast<unsigned char*>(h));
    std::printf("total sites: %d\n", n);
    FreeLibrary(h);
    return 0;
}
