# SAT-TR DETAIL DSP Plan

Estado: diseno previo. El parametro `DETAIL/DTL` ya existe en UI/APVTS, pero todavia no debe alterar DSP hasta que este plan se valide.

## Objetivo

`DETAIL` debe actuar como un control de preservacion de detalle en saturacion/clipping:

- `0%`: salida identica al estado actual del algoritmo.
- `100%`: maxima preservacion de detalle en zonas que el saturador aplasta.
- No debe generar audio sin entrada.
- No debe sustituir a `INSTABILITY`; `DETAIL` es un mecanismo dependiente de clipping/transitorios, no drift analogico.
- Debe ejecutarse por loader y respetar series, oversampling, mono/stereo y `RAW`.

La referencia conceptual mas cercana es Newfangled Saturate: su documentacion describe `DETAIL PRESERVATION` como un algoritmo que preserva detalle fino dentro de las secciones clippeadas, con `0%` como clipper normal y `100%` como preservacion completa. La tecnica de Au5 lo aproxima con una ruta de delta/foldback filtrada en agudos y usada para ducking/ring-mod sidechain.

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

Orden elegido: `react`, `detail`, `instability`, porque en la UI `DETAIL` va antes de `INSTABILITY` y conceptualmente queda entre dinamica propia del modelo y variacion analogica.

### 2. Estado DSP

Anadir estado dentro de `SatEngine::State`, per-series y per-channel:

```cpp
struct DetailState
{
    float hp1 = 0.0f;
    float hp2 = 0.0f;
    float env = 0.0f;
    float lastCorrection = 0.0f;
};

DetailState detail[kMaxSeries][2];
```

Motivos:

- Per-channel evita corrupcion L/R.
- Per-series mantiene coherencia con la cadena interna del loader.
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

- Preferible: filtro TPT/SVF high-pass de 2o orden si queremos respuesta mas controlada.
- Alternativa simple y barata: dos one-poles high-pass cascados.
- No usar filtro dinamico al principio; evita nuevas discontinuidades.

Decision propuesta: empezar fijo a `2500 Hz`, 12 dB/oct, en dominio oversampled si OS esta activo.

### 5. Limitador interno del delta

La ruta de delta no debe disparar picos ni convertir `DETAIL` en exciter agresivo.

```text
detailDrive = detail^1.35
deltaLimited = softLimit(hpDelta * detailGain)
```

Mapeo inicial:

```text
detailGain = 0.0 .. 1.5
ceiling = 0.12 .. 0.35 relativo al core
```

El techo debe depender de `DETAIL`, no de `DRIVE` directamente. El detector ya depende de cuanto clippea la senal.

### 6. Aplicacion al core: foldback/duck sample-accurate

Hay dos formas candidatas:

#### A. Correccion foldback firmada

```text
out = core - hpDeltaLimited * foldDepth
```

Ventaja: se parece a la idea "foldback" y actua solo donde hay delta.

Riesgo: signo incorrecto puede matar detalle o invertirlo de forma rara.

#### B. Ducking dependiente de sidechain

```text
duck = abs(hpDeltaLimited) * duckDepth
out = core * (1 - duck)
out += hpDeltaLimited * preserveDepth
```

Ventaja: mas controlado y cercano a RMSC/Compactor como reduccion por amplitud de sidechain.

Riesgo: si se exagera, puede sonar a compresion HF.

Decision propuesta: implementar primero una forma hibrida y muy acotada:

```text
correction = hpDeltaLimited * preserveDepth
duck = abs(hpDeltaLimited) * duckDepth
out = core * (1 - duck) + correction
```

Con:

```text
preserveDepth = detail^1.35 * 0.50
duckDepth     = detail^1.55 * 0.20
```

Esto permite que `DETAIL` a 100% preserve textura sin volverse un excitador libre.

### 7. Relacion con RAW

Recomendacion inicial:

- `RAW off`: `DETAIL` activo.
- `RAW on`: `DETAIL` activo, pero sin usar de-emphasis/post filtros externos como dependencia.

Motivo: `RAW` debe mostrar el caracter crudo del modelo, y `DETAIL` es parte del modelo si el usuario lo activa. Si perceptualmente se vuelve confuso, se puede decidir que `RAW` ignore `DETAIL`, pero no seria mi primera opcion.

### 8. Relacion con CLEAN

`Clean` debe seguir haciendo return temprano. `DETAIL` no debe actuar en `Clean`.

## Punto de insercion en SAT-TR actual

Ruta actual:

```text
PluginProcessor::processLoader()
  -> aplica IN
  -> filtros/chaos/pre si procede
  -> SatEngine::processBlock()
       -> smoothing parametros
       -> series
       -> pre-emphasis
       -> react/sag
       -> processFoo()
       -> safety soft clip
       -> girth
       -> de-emphasis
       -> DC blocker
       -> trim/final limiter
  -> filtros/post/delay/limit/out
```

Lugar recomendado:

```text
Despues de processFoo()
Antes de GIRTH
Antes de de-emphasis
Antes del DC blocker
```

Razon:

- Usa el resultado real del core.
- No mete detalle despues del DC blocker/final limiter.
- Permite que de-emphasis y DC blocker limpien cualquier residuo.
- Evita alterar las etapas loader-level fuera del scope.

Para modelos con tap claro (`Clipper`, `Diode`, `Transistor`), se puede calcular el delta dentro de `processFoo()` y devolver metadatos. Si queremos evitar cambiar demasiadas firmas, primera implementacion puede usar un helper post-core con `xBeforeCore` y `xAfterCore`, aunque esto es menos exacto.

## Fases de implementacion

### Fase 0: Confirmar plan

Sin codigo DSP. Este documento es la salida de fase 0.

### Fase 1: Cableado sin efecto audible

- Leer `pDetailA/B/C` en `processLoader`.
- Pasar `detailParam` a `SatEngine::processBlock`.
- Suavizar `sDetail` igual que otros parametros.
- Anadir `DetailState` y resets.
- Mantener `detail <= epsilon` como bypass exacto.

Criterio: con `DETAIL 0%` y `100%`, salida aun no cambia si el helper esta desactivado.

### Fase 2: Prototipo solo en CLIPPER

- Implementar delta usando el tap natural de `processClipper`.
- HP 12 dB/oct fijo a 2500 Hz.
- Limiter interno del delta.
- Correccion hibrida `core * (1 - duck) + correction`.

Criterio:

- Sine + saw clippeada: el saw debe sobrevivir mejor en las crestas.
- Breakbeat + sub: los transitorios altos no deben desaparecer tanto.
- Sin entrada no hay salida.
- No clicks automatizando `DETAIL`.

### Fase 3: Generalizar a DIODE y TRANSISTOR

- `Diode`: usar `clipIn` y thresholds propios.
- `Transistor`: usar `railIn` y thresholds de rail.
- Mantener intensidades algo mas bajas que `Clipper` si hace falta.

Criterio: no alterar el caracter base con `DETAIL 0%`.

### Fase 4: TAPE y TUBE conservadores

- `Tape`: delta posterior a pregain/tape core, profundidad reducida.
- `Tube`: delta dependiente del headroom/sag, profundidad reducida para no competir con SAG.

Criterio: `Tube SAG` sigue siendo el fenomeno dominante; `DETAIL` solo recupera textura de clipping.

### Fase 5: Tuning final y README

- Ajustar curva `detail^p`.
- Ajustar `preserveDepth` y `duckDepth`.
- Actualizar README solo cuando el DSP este activo y validado.

## Pruebas obligatorias

- `DETAIL 0%`: null test contra version anterior.
- `DETAIL 100%`: no overs peligrosos antes/despues del limiter.
- Entrada silencio: salida silencio.
- Mono/stereo: sin phasing ni diferencias no deseadas.
- Series `1..4`: determinismo y sin acumulacion explosiva.
- Oversampling `x1/x2/x4/x8/x16`: sin discrepancias grandes.
- Modelos: `Clipper`, `Diode`, `Transistor`, `Tape`, `Tube`, `Clean`.
- Automatizacion rapida: `DETAIL`, `DRIVE`, `BODY/GIRTH`, `TYPE/MOD`, `BIAS`, `REACT/SAG`, `INSTABILITY`.

## Riesgos

- Signo incorrecto en foldback: puede borrar detalle en lugar de preservarlo.
- Additive detail demasiado alto: se convierte en exciter HF.
- HP con demasiada pendiente: puede sonar artificial o phasey.
- HP demasiado bajo: graves controlan el sidechain y aparece pumping.
- Aplicarlo despues del limiter final: generaria overs o clicks.
- Aplicarlo antes del core sin delta real: seria otra distorsion, no detail preservation.

## Decision tecnica propuesta

Implementar `DETAIL` como "high-passed clipped-residual preservation":

```text
detailSignal = HP12(clipDelta)
detailSignal = softLimit(detailSignal)
out = core * (1 - abs(detailSignal) * duckDepth)
    + detailSignal * preserveDepth
```

Conservador, sample-accurate, dependiente de clipping real, sin audio ex nihilo, y con `DETAIL 0%` como bypass exacto.
