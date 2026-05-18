# SAT-TR DETAIL DSP Plan

Estado: DSP implementado en `Clipper`, `Diode`, `Transistor`, `Tape` y `Tube`; README actualizado; pendiente de tuning perceptivo final.

## Objetivo

`DETAIL` debe actuar como un control de preservacion de detalle en saturacion/clipping:

- `0%`: salida identica al estado actual del algoritmo.
- `50%`: maxima preservacion calibrada en zonas que el saturador aplasta.
- `100%`: maxima preservacion calibrada mas enfasis de aire dentro de la sidechain.
- No debe generar audio sin entrada.
- No debe sustituir a `INSTABILITY`; `DETAIL` es un mecanismo dependiente de clipping/transitorios, no drift analogico.
- Debe ejecutarse por loader y respetar series, oversampling, mono/stereo y `RAW`.

La referencia conceptual mas cercana es Newfangled Saturate: su documentacion describe `DETAIL PRESERVATION` como un algoritmo que preserva detalle fino dentro de las secciones clippeadas, con `0%` como clipper normal y `100%` como preservacion completa. En SAT-TR ese comportamiento calibrado queda cubierto de `0-50%`, y el tramo `50-100%` se reserva para revelar aire/textura. La tecnica de Au5 lo aproxima con una ruta de delta/foldback filtrada en agudos y usada para ducking/ring-mod sidechain.

## Referencias

- Newfangled Audio, Saturate blog: https://www.newfangledaudio.com/post/saturate-1-7-0-preserving-detail-while-clipping
- Newfangled Audio, Saturate product page: https://www.newfangledaudio.com/saturate
- Newfangled Audio, Saturate user guide: https://www.newfangledaudio.com/_files/ugd/bc5df0_03a9484bdfb54bcda962ddf5120bc436.pdf
- Mannix Audio Technology, Detail Preserving Clipper: https://mannixsquared.com/plugins/detail-preserving-clipper/
- Kilohearts Compactor: https://kilohearts.com/products/compactor
- Stevon RMSC: https://www.stevon-av.com/max-for-live/rmsc
- Polarity/RMSC analysis: https://polarity.me/posts/polarity-music/2024-10-28-ringmod-sidechain-in-bitwig/

## Semantica comercial de "DETAIL"

La palabra `DETAIL` es coherente si la usamos como preservacion/recuperacion de detalle bajo clipping, no como un simple control de brillo:

- Newfangled usa `DETAIL PRESERVATION` para mantener detalle fino que otros clippers eliminan.
- Mannix usa `Detail Preserving Clipper` con una ruta de detalle separada, `Detail Frequency` como high-pass de la ruta y `Foldback` como aportacion de esa ruta alrededor del clipping.
- En pedales de guitarra, "detail" a veces se usa como control tonal de agudos/presencia. Ese uso existe, pero no es el que queremos en SAT-TR.

Conclusion UX: `DETAIL/DTL` es correcto si README y tooltip dejan claro que es "clipped-detail preservation", no "treble".

## Concepto DSP

Un clipper normal hace esto:

```text
input -> drive -> clipper -> output
```

Cuando la senal clippea, el clipper aplasta tanto el componente grande como los detalles pequenos que estaban montados encima. La idea de `DETAIL` no es anadir brillo estatico, sino detectar el material que el clipper esta eliminando y usar solo la parte util para evitar que el resultado quede plano.

La aproximacion propuesta:

```text
driven tap -> reference clip -> clipped delta -> high-pass -> limiter -> detail control
                                                       |
core saturator output <---------------- sample-accurate foldback/duck correction
```

### Respuesta a la duda: "solo quedarnos con los armonicos"

No exactamente. Lo correcto no es sintetizar o aislar "armonicos" de forma generica. Lo que queremos conservar es el residuo de clipping:

```text
clipDelta = drivenTap - referenceClip(drivenTap)
```

Ese residuo contiene material que solo existe cuando la senal supera el umbral de clipping: transitorios, pequenos componentes montados sobre graves grandes y nuevos parciales creados por la no linealidad. Despues se filtra en agudos para evitar que el sub/low-mid controle el mecanismo.

Por tanto:

- Si no hay clipping, `clipDelta` tiende a cero.
- Si hay sub fuerte comiendose el detalle, el delta aparece solo en la zona afectada.
- El high-pass decide que parte del delta puede actuar como "detalle".
- No se debe anadir una excitacion HF fija ni un generador de armonicos independiente.

## Diseno recomendado

### 1. Parametro

`DETAIL` ya esta creado como parametro por loader:

```text
detail_a / detail_b / detail_c
0.0 .. 1.0
default 0.0
UI: DETAIL / DTL
```

La siguiente fase debe leerlo en `SATTRAudioProcessor::processLoader()` y pasarlo a `SatEngine::processBlock()`.

Firma propuesta:

```cpp
SatEngine::processBlock (...,
                         float reactParam,
                         float detailParam,
                         float instabilityParam,
                         ...);
```

Orden elegido: `react`, `series`, `detail`, `instability` en la UI. `DETAIL`
actua despues del stack interno de `SERIES`, asi que debe aparecer debajo de
`SERIES` para reflejar el flujo real.

### 2. Estado DSP

Anadir estado dentro de `SatEngine::State`, post-series y per-channel:

```cpp
struct DetailState
{
    float hpZ1 = 0.0f;
    float hpZ2 = 0.0f;
    float env = 0.0f;
    float lastReduction = 0.0f;
};

DetailState detail[2];
```

Motivos:

- Per-channel evita corrupcion L/R.
- Post-series evita que pases posteriores de `SERIES` vuelvan a emborronar el
  detalle ya preservado.
- Estado pequeno, coste bajo.
- Reset junto al resto de estados cuando cambia modelo.

### 3. Generacion del delta

La fuente mas estable es un "reference clipper" simple alimentado por un tap real del modelo:

```text
drivenTap = senal antes o justo dentro del clipper principal
reference = clamp/softClip(drivenTap, threshold)
delta = drivenTap - reference
```

Por modelo:

- `Clipper`: mejor primer objetivo. Ya tiene `clipIn`, thresholds y ADAA clipper. Es el caso mas directo y mas parecido a Newfangled/Au5.
- `Diode`: tambien tiene `clipIn`, thresholds y ADAA. Segundo objetivo natural.
- `Transistor`: usar `railIn` como tap. Es donde se produce el rail clipping real.
- `Tape`: usar la senal antes del ADAA tape core o un tap posterior al pregain, con intensidad mas conservadora.
- `Tube`: usar el tap del core ya condicionado por headroom/sag, no el input crudo. Debe ser sutil para no romper el caracter de SAG.

No recomiendo arrancar aplicandolo a todos los modelos a la vez. Primero `Clipper`, validar con pruebas, despues generalizar.

### 4. Filtro HP de detalle

Au5 usa alrededor de 2-3 kHz con 12 dB/oct para reducir fase y no meter graves en el mecanismo. Para SAT-TR:

```text
detailHpHz = 2500 Hz fijo inicialmente
slope = 12 dB/oct
```

Implementacion:

- Activo: biquad high-pass Butterworth de 2o orden (`Q = 0.7071`).
- Es mas cercano al HP 12 dB/oct de un EQ nativo que dos one-poles cascados.
- No usar filtro dinamico al principio; evita nuevas discontinuidades.

Decision activa: `2500 Hz`, 12 dB/oct, en dominio oversampled si OS esta activo.

### 5. Limitador interno del delta

La ruta de delta no debe disparar picos ni convertir `DETAIL` en exciter agresivo.

```text
parallelClip = hardclip(stageInput, thresholdFromRawDrive)
deltaLimited = limit(HP12(stageInput - parallelClip), ceiling)
```

Mapeo activo:

```text
ceiling = 0.57
preserveAmount = min(DETAIL * 2, 1)
side = abs(deltaLimited) * preserveAmount
```

El ceiling es literal y la intensidad de sidechain llega al 100% cuando
`DETAIL` alcanza el 50%. A partir de ahi no se incrementa el duck para evitar
perdidas de loudness o carve dinamico excesivo. El umbral del hardclip paralelo
depende del `DRIVE` crudo suavizado, no del drive curvado por modelo.

### 6. Aplicacion al core: duck RMSC/Compactor

Tras revisar la semantica de RMSC/Compactor, la ruta correcta no debe sumar el
delta como audio. El delta HP actua como sidechain de amplitud y reduce la
magnitud del core saturado:

```text
parallelClip = hardclip(stageInput, thresholdFromRawDrive)
preserveAmount = min(DETAIL * 2, 1)
side = abs(limit(HP12(stageInput - parallelClip), ceiling)) * preserveAmount
env  = peak, attack ~0.05 ms, release 0 ms
reduction = min(env, abs(core))
out = sign(core) * (abs(core) - reduction)
```

Motivos:

- No hay audio ex nihilo: si `core` es cero, la salida sigue siendo cero.
- Evita que el tramo RMSC de `DETAIL` se convierta en exciter HF o generador aditivo.
- Es mas fiel a la idea "sidechain amplitude subtractor" que una correccion firmada.
- El release corto evita zipper/crackle sin convertirlo en compresion lenta.

### 7. Tramo superior: enfasis de sidechain

Para que `DETAIL` tenga un extremo creativo sin sobredimensionar el duck
RMSC/Compactor ni subir el volumen del core, el tramo `50-100%` mantiene la
preservacion al 100% y mezcla un high shelf amplio dentro de la sidechain
filtrada:

```text
airAmount = smoothStep01((DETAIL - 0.5) * 2)
sideSource = lerp(HP12(delta), Shelf12(HP12(delta)), airAmount)
```

Implementacion activa:

- Shelf maximo fijo a `+18 dB` en la ruta de sidechain.
- Frecuencia base `2000 Hz`.
- Pendiente suave tipo high shelf/tilt, no EQ quirurgica.
- Estado per-channel dentro de `DetailState`.
- El shelf procesa el residual HP mientras `DETAIL` esta activo y se mezcla
  solo por encima del 50%, antes del ceiling y del detector.

Motivo: el tramo alto debe hacer que el detector reaccione mas a los agudos
clippeados, no actuar como boost post-core.

### 8. Relacion con RAW

Recomendacion inicial:

- `RAW off`: `DETAIL` activo.
- `RAW on`: `DETAIL` activo, pero sin usar de-emphasis/post filtros externos como dependencia.

Motivo: `RAW` debe mostrar el caracter crudo del modelo, y `DETAIL` es parte del modelo si el usuario lo activa. Si perceptualmente se vuelve confuso, se puede decidir que `RAW` ignore `DETAIL`, pero no seria mi primera opcion.

### 9. Relacion con CLEAN

`Clean` debe seguir haciendo return temprano. `DETAIL` no debe actuar en `Clean`.

## Punto de insercion en SAT-TR actual

Ruta actual:

```text
PluginProcessor::processLoader()
  -> aplica IN
  -> filtros/chaos/pre si procede
  -> SatEngine::processBlock()
       -> smoothing parametros
       -> series:
          -> pre-emphasis
          -> react/sag
          -> processFoo()
          -> safety soft clip
          -> girth
          -> de-emphasis
          -> DC blocker
          -> trim/final limiter
       -> DETAIL post-series
  -> filtros/post/delay/limit/out
```

Lugar recomendado:

```text
Despues de completar todos los pases de SERIES
Antes de volver a la ruta loader-level de PluginProcessor
```

Razon:

- Usa el resultado real del black box completo, no solo de un pase interno.
- Evita que una etapa posterior de SERIES vuelva a aplastar el efecto.
- Mantiene el mecanismo dentro de SatEngine, antes de filtros/delay/limit del loader.
- Evita alterar las etapas loader-level fuera del scope.

La ruta activa no usa taps parciales por modelo. El delta sale de una rama
hardclip comun alimentada por la entrada del black box completo, y se aplica
una sola vez al resultado final de `SERIES`.

## Fases de implementacion

### Fase 0: Confirmar plan

Sin codigo DSP. Este documento es la salida de fase 0.

### Fase 1: Cableado sin efecto audible

- Leer `pDetailA/B/C` en `processLoader`.
- Pasar `detailParam` a `SatEngine::processBlock`.
- Suavizar `sDetail` igual que otros parametros.
- Anadir `DetailState` y resets.
- Mantener `detail <= epsilon` como bypass exacto.

Estado: implementado. Criterio de fase ya superado: el cableado quedo verificado antes de activar el helper de clipped-residual.

### Fase 2: Prototipo solo en CLIPPER

- Implementar delta usando el tap natural de `processClipper`.
- HP 12 dB/oct fijo a 2500 Hz.
- Limiter interno del delta.
- Duck estilo RMSC/Compactor: la amplitud del delta HP reduce la magnitud del core saturado; el delta no se suma como audio.

Estado: implementado inicialmente en `Model::Clipper`; despues generalizado con el mismo helper comun.

Criterio:

- Sine + saw clippeada: el saw debe sobrevivir mejor en las crestas.
- Breakbeat + sub: los transitorios altos no deben desaparecer tanto.
- Sin entrada no hay salida.
- No clicks automatizando `DETAIL`.

### Fase 3: Generalizar a DIODE y TRANSISTOR

- `Diode`: usar `clipIn` y thresholds propios.
- `Transistor`: usar `railIn` y thresholds de rail.
- Mantener intensidades algo mas bajas que `Clipper` si hace falta.

Estado: implementado. Ambos usan el mismo helper comun que `Clipper`; solo cambia el tap normalizado de delta.

Criterio: no alterar el caracter base con `DETAIL 0%`.

### Fase 4: TAPE y TUBE conservadores

- `Tape`: delta posterior a pregain/tape core, profundidad reducida.
- `Tube`: delta dependiente del headroom/sag, profundidad reducida para no competir con SAG.

Estado: implementado. `Tape` usa residuo de `satIn` antes del ADAA tape core y lo escala de forma conservadora; `Tube` usa el residuo del ceiling final del core y lo escala para no competir con SAG.

Criterio: `Tube SAG` sigue siendo el fenomeno dominante; `DETAIL` solo recupera textura de clipping.

### Fase 5: Tuning final y README

- Ajustar curva `detail^p`.
- Ajustar profundidad de reduccion y release del duck.
- Actualizar README solo cuando el DSP este activo y validado.

Estado parcial: el helper comun ya usa reduccion de magnitud RMSC-like con detector peak rapido. La sidechain usa un ceiling literal, no saturacion `tanh`, y el detector se calibra cerca del ajuste del video: ataque ~0.05 ms y release/correccion instantaneos. La ley de duck es sidechain-ring pura: `DETAIL` escala el sidechain hasta el 50% y la reduccion resta esa amplitud, sin makeup ni cap porcentual adicional. La sidechain no recibe ganancia extra antes del limitador: se calibra a 1x para evitar que `DETAIL` reduzca margen dinamico mas alla de la amplitud real del residual filtrado. El tramo 50-100% no boostea el core: mezcla un shelf de hasta +18 dB en la sidechain antes del ceiling.

## Pruebas obligatorias

- `DETAIL 0%`: null test contra version anterior.
- `DETAIL 50%`: equivalente al motor de preservacion completo.
- `DETAIL 100%`: no overs peligrosos antes/despues del limiter; sidechain shelf perceptible pero estable.
- Entrada silencio: salida silencio.
- Mono/stereo: sin phasing ni diferencias no deseadas.
- Series `1..4`: determinismo y sin acumulacion explosiva.
- Oversampling `x1/x2/x4/x8/x16`: sin discrepancias grandes.
- Modelos: `Clipper`, `Diode`, `Transistor`, `Tape`, `Tube`, `Clean`.
- Automatizacion rapida: `DETAIL`, `DRIVE`, `BODY/GIRTH`, `TYPE/MOD`, `BIAS`, `REACT/SAG`, `INSTABILITY`.

## Riesgos

- Reduccion excesiva: puede sentirse como compresion HF en lugar de preservacion.
- Release demasiado corto: puede sonar rasposo; demasiado largo, bombea.
- HP con demasiada pendiente: puede sonar artificial o phasey.
- HP demasiado bajo: graves controlan el sidechain y aparece pumping.
- Aplicarlo despues del limiter final: generaria overs o clicks.
- Aplicarlo antes del core sin delta real: seria otra distorsion, no detail preservation.

## Decision tecnica propuesta

Implementar `DETAIL` como "high-passed clipped-residual preservation":

```text
parallelClip = hardclip(stageInput, thresholdFromRawDrive)
detailSignal = HP12(stageInput - parallelClip)
detailSignal = limit(detailSignal, ceiling)
preserveAmount = min(DETAIL * 2, 1)
side = abs(detailSignal) * preserveAmount
reduction = min(sideEnvelope, abs(core))
out = sign(core) * (abs(core) - reduction)
```

La ruta activa usa un delta comun por etapa para todos los modelos: una rama
hardclip paralela genera `stageInput - parallelClip`, y ese residual alimenta
la sidechain. El threshold de esa rama depende del `DRIVE` crudo suavizado, no
del drive curvado por modelo, para que `DETAIL` sea comun entre algoritmos. Esto
evita que `DETAIL` dependa de residuales internos parciales o escalados por
algoritmo (`Tube`, `Tape`, etc.). Conservador, sample-accurate, dependiente de
clipping real, sin audio ex nihilo, y con `DETAIL 0%` como bypass exacto.

Revision activa: `DETAIL` ahora usa rango doble. De 0 a 50% recorre el motor
RMSC completo; de 50 a 100% mantiene esa preservacion al maximo y mezcla un
shelf en la sidechain hasta +18 dB antes del ceiling.
