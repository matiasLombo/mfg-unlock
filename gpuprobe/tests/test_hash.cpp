// test_hash.cpp -- XXH64 contra el vector conocido y contra si mismo.
//
// El vector de la cadena vacia es el unico numero de XXH64 que se puede citar
// sin la libreria al lado; el resto de los tests son propiedades, que es lo
// que realmente nos importa: determinismo, sensibilidad a un bit, y que la
// semilla mande.
#include "../core/hash.h"

#include "harness.h"

#include <set>
#include <string>
#include <vector>

using namespace gp;

int main() {
    // Vector de referencia de XXH64 (seed 0, entrada vacia).
    CHECK_EQ_U64(hash_bytes("", 0, 0), 0xEF46DB3751D8E999ull);

    // Determinismo: dos llamadas, el mismo numero.
    const char *msg = "una pasada de sombras de 4096 que se lee a 1/8";
    CHECK_EQ_U64(hash_str(msg), hash_str(msg));

    // Un bit distinto, hash distinto. Recorremos longitudes que cruzan los
    // tres caminos del algoritmo (bloque de 32, cola de 8, cola de 1).
    for (size_t len : {1u, 4u, 7u, 8u, 15u, 31u, 32u, 33u, 64u, 129u}) {
        std::vector<u8> a(len, 0xAB);
        std::vector<u8> b = a;
        b[len / 2] ^= 0x01;
        CHECK(hash_bytes(a.data(), len) != hash_bytes(b.data(), len));
    }

    // La semilla cambia el resultado.
    CHECK(hash_str(msg, 0) != hash_str(msg, 1));

    // Sin colisiones en un corpus chico de descriptores plausibles.
    std::set<u64> seen;
    for (u32 w = 256; w <= 4096; w *= 2)
        for (u32 h = 256; h <= 4096; h *= 2)
            for (u32 f = 0; f < 40; ++f) {
                u64 k = hash_combine(hash_combine(w, h), f);
                CHECK(seen.insert(k).second);
            }

    // hash_combine no es conmutativo: el orden de los campos importa.
    CHECK(hash_combine(1, 2) != hash_combine(2, 1));

    return gp_test::report("test_hash");
}
