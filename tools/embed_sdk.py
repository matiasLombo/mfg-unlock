"""El set de Streamline adentro del dll.

    python tools/embed_sdk.py [ruta-al-streamline-sdk-vX.Y.Z.zip]

El dll no descarga nada: hasta ahora leia %LOCALAPPDATA%\\mfg-unlock\\sdk\\2.12,
una carpeta armada a mano con el zip del SDK. Eso contradice
[[ships-as-one-dll]]: el usuario tiene el dll y nada mas. Aca los seis
archivos que se cargan de verdad (los otros cinco sl.* no se piden nunca) se
empaquetan en un zip chico con deflate y ese zip se enlaza como bytes
(src/sdk_blob.S, .incbin); src/sdk_manifest.h lleva nombre y tamano de cada
uno para saber si la cache esta completa. Al arrancar, si falta alguno o el
tamano no coincide, el dll los escribe (src/loader.h, ensure_sdk_cache).

Fuente: el zip oficial del SDK (bin/x64/), o si no se da ruta, la cache
misma. Los binarios son de NVIDIA: la licencia del SDK permite
redistribuirlos con aplicaciones; el que decide distribuir el dll es quien
decide eso.
"""
import io
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MINOR = 12
FILES = ["sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.pcl.dll", "sl.reflex.dll", "nvngx_dlssg.dll"]


def read_from_sdk_zip(path):
    out = {}
    with zipfile.ZipFile(path) as z:
        for f in FILES:
            out[f] = z.read("bin/x64/" + f)
    return out


def read_from_cache():
    d = os.path.join(os.environ["LOCALAPPDATA"], "mfg-unlock", "sdk", "2.%d" % MINOR)
    return {f: open(os.path.join(d, f), "rb").read() for f in FILES}


def main():
    src = read_from_sdk_zip(sys.argv[1]) if len(sys.argv) > 1 else read_from_cache()
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    blob = os.path.join(ROOT, "build", "sdk-2.%d.zip" % MINOR)
    with zipfile.ZipFile(blob, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in FILES:
            z.writestr(f, src[f])
    total_raw = sum(len(v) for v in src.values())
    with open(os.path.join(ROOT, "src", "sdk_blob.S"), "w", newline="\n") as s:
        s.write("# Generado por tools/embed_sdk.py -- no editar a mano, no commitear.\n"
                "# El zip con los seis archivos del set de Streamline, como bytes.\n"
                "    .section .rdata\n"
                "    .globl sdk_zip\n"
                "    .globl sdk_zip_end\n"
                "    .p2align 4\n"
                "sdk_zip:\n"
                "    .incbin \"build/sdk-2.%d.zip\"\n"
                "sdk_zip_end:\n" % MINOR)
    with open(os.path.join(ROOT, "src", "sdk_manifest.h"), "w", newline="\n") as h:
        h.write("// Generado por tools/embed_sdk.py -- no editar a mano, no commitear.\n"
                "// Los archivos del set embebido y su tamano crudo, para saber si la cache esta completa.\n"
                "#pragma once\n"
                "static const unsigned kSdkMinor = %d;\n"
                "struct SdkFile { const wchar_t *name; unsigned size; };\n"
                "static const SdkFile kSdkFiles[] = {\n" % MINOR)
        for f in FILES:
            h.write('    { L"%s", %d },\n' % (f, len(src[f])))
        h.write("};\n"
                "static const int kSdkFilesN = %d;\n" % len(FILES))
    print("%s: %d bytes (crudo %d), %d archivos" % (blob, os.path.getsize(blob), total_raw, len(FILES)))


if __name__ == "__main__":
    main()
