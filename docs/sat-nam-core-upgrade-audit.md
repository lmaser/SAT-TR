# SAT-TR: auditoria de NeuralAmpModelerCore

## Estado actual

SAT-TR integraba una copia vendorizada de NeuralAmpModelerCore `0.5.3`.
La integracion registra `Linear`, `ConvNet`, `LSTM`, `WaveNet` y
`SlimmableContainer`, carga dos instancias mono por loader y aplica el tamano
slimmable mediante `SetSlimmableSize`.

## Estado upstream revisado

La version estable posterior es `0.5.4` (`1f42f88`, junio de 2026). El cambio
publicado como `0.5.4` corrige las anotaciones `restrict` de GEMM inline para
MSVC. La rama posterior tambien contiene cambios de mayor alcance: convolucion
FFT para modelos `Linear`, reorganizacion de sus fuentes, API publica de
prewarm, ajustes de Eigen y mejoras de seguridad/eficiencia en caminos A2.

## Valoracion

### Actualizacion recomendable

Debe actualizarse al menos a `0.5.4` antes de una entrega profesional para
incorporar la correccion especifica de MSVC y mantener el runtime alineado con
el upstream estable.

### No hacer un reemplazo directo de `main`

La rama `main` contiene cambios de comportamiento y rendimiento que pueden
afectar a modelos `Linear`, WaveNet/A2 y a los tiempos de reset. Cambiarla sin
comparativas puede alterar el sonido, el consumo o el tiempo de carga.

## Plan de migracion

1. Sustituir el core por el tag `v0.5.4`, conservando inicialmente la API de
   `SatNamModel` y los IDs de SAT-TR.
2. Actualizar el proyecto para incluir las fuentes nuevas de `Linear` y las
   rutas reorganizadas, sin mezclar todavia cambios de `main`.
3. Compilar con MSVC Release y comprobar todos los parsers registrados.
4. Comparar modelos representativos `Linear`, `ConvNet`, `LSTM`, `WaveNet` y
   `SlimmableContainer` a 44.1, 48, 88.2 y 96 kHz.
5. Medir RMS, pico, diferencia muestra a muestra, CPU, memoria, tiempo de
   carga y comportamiento de reset/prewarm.
6. Aceptar solo si no aparecen regresiones audibles o de tiempo real; después
   valorar mejoras posteriores de `main` por separado.

## Resultado de la migracion v0.5.4

Se ha vendorizado el tag upstream `v0.5.4`, commit
`1f42f88535884450104b8711d7595019afa0495b`, manteniendo la Eigen fijada por
el propio tag (`bc3b39870ecb690a623a3f49149a358b95c5781d`). Se han incorporado
las fuentes nuevas de `Linear` y el soporte `unsupported/Eigen/FFT`, y se ha
anadido `linear.cpp` al proyecto MSVC. No se han cambiado IDs, el contrato de
`SatNamModel`, el flujo de carga mono-estereo ni la politica de sample rate.

Verificaciones realizadas:

- `SAT-TR_SharedCode.vcxproj`, `Release|x64`: compilacion correcta.
- Targets SAT generados: shared library, standalone y VST3.
- `loadmodel` oficial MSVC: carga correcta de los 9 modelos de ejemplo del tag.
- Render oficial a 48 kHz con audio mono: `LSTM`, `WaveNet`,
  `SlimmableContainer` y `WaveNet A2` producen WAV de salida valido.
- Comparativa A/B 0.5.3-v0.5.4 con cinco modelos comunes: SHA-256 identico en
  los WAV de salida y diferencia muestra a muestra nula.
- `Linear` con 1536 taps, ruta directa frente a FFT y bloques irregulares:
  diferencia maxima `8.94e-08`.
- Sample rate: un modelo declarado a 48 kHz acepta 48 kHz y rechaza
  explicitamente 44.1/88.2/96 kHz; el wrapper SAT recibe el sample rate real del
  host y aplica la misma validacion.
- Probe realtime SAT enlazado contra `SAT-TR.lib`: a 48 kHz, WaveNet consume
  0.64%-0.87% y WaveNet A2 Max 8.25%-10.10% del tiempo de audio disponible en
  bloques de 32 a 512 muestras, sin audio no finito.
- Impulso a traves de `SatNamModel`: primera muestra no nula en indice 0; no hay
  latencia DSP/PDC interna adicional.
- No se han detectado errores nuevos de API, doble definicion ni incompatibilidad
  del wrapper SAT.

Limitaciones conocidas de esta fase:

- La suite de asignaciones oficial no compila en MSVC porque incluye `dlfcn.h`,
  una dependencia POSIX; no es un fallo del core ni del wrapper SAT.
- La medicion de CPU realtime y latencia en host real queda pendiente; el
  tiempo offline de carga+render no se considera benchmark de audio.
- El header upstream `version.h` continua declarando `0.5.3` dentro del tag
  `v0.5.4`; el commit/tag es la referencia de versionado usada para esta copia.

## Decision

La migracion v0.5.4 queda aceptada a nivel de compilacion, integracion
funcional, comparativa offline y probe realtime del wrapper SAT. SAT-TR queda
tecnicamente cerrado para esta actualizacion. La escucha con material de
usuario queda como control subjetivo de producto, no como bloqueo de
implementacion. No se adopta `main` hasta una auditoria independiente.
