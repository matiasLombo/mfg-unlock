// Valida zipmini.h contra el zip real: tamano y SHA-256 de cada entrada.
//
// Un inflate mal hecho no falla ruidosamente: entrega bytes plausibles. Por eso
// se compara byte a byte contra lo que saca Python, no "parece un PE".
//
//   g++ -std=c++20 -O2 -I src tools/test_zipmini.cpp -o /tmp/tz.exe
//   /tmp/tz.exe <ruta al zip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "zipmini.h"

// SHA-256 compacto, solo para comparar contra Python.
namespace sha {
typedef unsigned int u32;
typedef unsigned long long u64;
static const u32 K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static inline u32 ror(u32 x,int n){return (x>>n)|(x<<(32-n));}
static void hash(const unsigned char*d,size_t n,unsigned char out[32]){
    u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total=n; std::vector<unsigned char> m(d,d+n);
    m.push_back(0x80); while(m.size()%64!=56) m.push_back(0);
    u64 bits=(u64)total*8; for(int i=7;i>=0;--i) m.push_back((unsigned char)(bits>>(i*8)));
    for(size_t off=0;off<m.size();off+=64){
        u32 w[64];
        for(int i=0;i<16;++i) w[i]=(m[off+i*4]<<24)|(m[off+i*4+1]<<16)|(m[off+i*4+2]<<8)|m[off+i*4+3];
        for(int i=16;i<64;++i){u32 s0=ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            u32 s1=ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;}
        u32 a=h[0],b=h[1],c=h[2],dd=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;++i){
            u32 S1=ror(e,6)^ror(e,11)^ror(e,25), ch=(e&f)^((~e)&g);
            u32 t1=hh+S1+ch+K[i]+w[i];
            u32 S0=ror(a,2)^ror(a,13)^ror(a,22), maj=(a&b)^(a&c)^(b&c);
            u32 t2=S0+maj;
            hh=g;g=f;f=e;e=dd+t1;dd=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=dd;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    for(int i=0;i<8;++i){out[i*4]=(unsigned char)(h[i]>>24);out[i*4+1]=(unsigned char)(h[i]>>16);
        out[i*4+2]=(unsigned char)(h[i]>>8);out[i*4+3]=(unsigned char)h[i];}
}
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("uso: test_zipmini <zip>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no se pudo abrir %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> zip((size_t)n);
    if (fread(zip.data(), 1, (size_t)n, f) != (size_t)n) { printf("lectura corta\n"); return 2; }
    fclose(f);

    std::vector<zipmini::Entrada> objetivo;
    zipmini::recorrer(zip.data(), zip.size(), [&](const zipmini::Entrada &e) {
        std::string nom(e.nombre, e.nombre_len);
        if (nom.rfind("bin/x64/", 0) == 0 && nom.find('/', 8) == std::string::npos &&
            nom.size() > 4 && nom.compare(nom.size() - 4, 4, ".dll") == 0)
            objetivo.push_back(e);
    });
    printf("entradas bin/x64/*.dll: %d\n", (int)objetivo.size());

    int fallos = 0;
    for (const auto &e : objetivo) {
        std::string nom(e.nombre, e.nombre_len);
        std::vector<unsigned char> out(e.crudo ? e.crudo : 1);
        const long got = zipmini::extraer(zip.data(), zip.size(), e, out.data(), out.size());
        if (got < 0) { printf("  FALLO   %-30s no se pudo extraer\n", nom.c_str() + 8); ++fallos; continue; }
        unsigned char h[32];
        sha::hash(out.data(), (size_t)got, h);
        char hex[65];
        for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", h[i]);
        hex[64] = 0;
        printf("  %-30s %9ld  %s\n", nom.c_str() + 8, got, hex);
    }
    printf(fallos ? "FALLOS: %d\n" : "todas extraidas\n", fallos);
    return fallos ? 1 : 0;
}
