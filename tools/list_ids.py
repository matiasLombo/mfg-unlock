"""Lista los identificadores de un archivo C++ (solo codigo: sin cadenas ni
comentarios) con su frecuencia, para encontrar los que siguen en castellano.

    python tools/list_ids.py src/present.h [prefijo_a_excluir ...]
"""
import re, sys, collections

SEG = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)
KEYWORDS = set("""if else for while return static const void bool int unsigned long char double
float struct typedef nullptr true false sizeof reinterpret_cast static_cast auto break continue
switch case default do new delete this class inline volatile extern namespace using template
typename enum short signed goto operator public private protected virtual override constexpr
noexcept try catch throw""".split())

def main():
    s = open(sys.argv[1], encoding="utf-8", errors="replace").read()
    code = SEG.sub(" ", s)
    ids = collections.Counter(re.findall(r"\b[A-Za-z_][A-Za-z_0-9]*\b", code))
    skip = tuple(sys.argv[2:]) if len(sys.argv) > 2 else ()
    rows = [(k, v) for k, v in ids.items() if k not in KEYWORDS and len(k) >= 3
            and not k.startswith(skip) and not k.isupper()]
    rows.sort(key=lambda x: (-x[1], x[0]))
    print(" ".join("%s:%d" % r for r in rows))

if __name__ == "__main__":
    main()
