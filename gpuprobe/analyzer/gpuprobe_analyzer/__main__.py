import sys

from .cli import main

try:
    sys.exit(main())
except BrokenPipeError:
    # `gpuprobe passes sesion.jsonl | head` cierra el pipe y Python larga un
    # traceback que no le sirve a nadie. Se cierra stdout a mano para que el
    # interprete no vuelva a intentar vaciarlo al salir.
    try:
        sys.stdout.close()
    except Exception:
        pass
    sys.exit(0)
except KeyboardInterrupt:
    sys.exit(130)
