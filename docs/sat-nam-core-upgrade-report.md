# SAT-TR: informe de actualizacion NAM v0.5.4

## Implementado

- Fuentes NAM actualizadas al tag upstream `v0.5.4`.
- `Linear` separado en `linear.cpp`/`linear.h`, incluyendo su ruta FFT.
- Eigen conservada en la revision fijada por el tag; se anadieron solo los
  headers FFT oficiales que faltaban en la copia local.
- Proyecto Visual Studio actualizado para compilar `linear.cpp`.
- `SatNamModel` e IDs SAT-TR sin cambios funcionales.

## Evidencia

- Compilacion SAT `Release|x64`: correcta.
- Shared library, standalone y VST3: generados.
- Nueve modelos oficiales cargados correctamente.
- Cuatro modelos oficiales renderizados correctamente a 48 kHz.
- Comparativa A/B con cinco modelos comunes: 5760 muestras por modelo, 48 kHz,
  mismo WAV de entrada y SHA-256 idéntico entre 0.5.3 y 0.5.4.
- Prueba específica de `Linear` con 1536 taps y bloques irregulares: ruta
  directa frente a FFT con diferencia máxima de `8.94e-08`.
- Prueba de sample rate con `WaveNet` declarado a 48 kHz: 48 kHz acepta el
  render; 44.1, 88.2 y 96 kHz se rechazan explícitamente por incompatibilidad
  con el sample rate del modelo. Esto respeta el metadato NAM y no fija el host
  a 48 kHz.
- Probe realtime enlazado contra la `SAT-TR.lib` de producción, a 48 kHz, con
  bloques 32/64/128/256/512: `WaveNet` queda entre 0.64% y 0.87% del tiempo de
  audio disponible; `WaveNet A2 Max`, entre 8.25% y 10.10%. Todas las muestras
  fueron finitas tras 2000 bloques medidos por tamaño.
- La prueba de impulso del wrapper devuelve la primera muestra no nula en el
  índice 0: `SatNamModel` no añade latencia DSP/PDC propia.
- Medición orientativa de carga+render sobre `WaveNet A1`: 0.5.3, media
  421.68 ms; 0.5.4, media 276.85 ms. No es benchmark realtime porque incluye
  deserialización y arranque del proceso.

## Pendiente antes de publicar

1. Escucha ciega con material de usuario, que es una validación subjetiva de
   producto y no un bloqueo técnico del core.

El probe reproducible está en `Tests/SatNamRealtimeProbe.cpp` y se enlaza
contra la misma librería compartida utilizada por SAT-TR.

La suite de asignaciones del upstream no se considera criterio de bloqueo para
SAT porque usa `dlfcn.h` y no es portable a MSVC sin un adaptador de test.
