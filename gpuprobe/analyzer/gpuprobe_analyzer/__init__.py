"""gpuprobe -- analizador offline de sesiones.

Lee el JSONL que escribe el DLL, arma el modelo de la sesion y emite
candidatos de optimizacion con ganancia ESTIMADA. La ganancia MEDIDA no sale
de aca: sale del harness A/B, que es lo unico que sobrevive al drift de clocks
de la GPU. Este modulo dice donde mirar; el harness dice si sirvio.

Sin dependencias: solo la biblioteca estandar de Python 3.
"""

__version__ = "0.1.0"
SCHEMA = 1
