# MP3 Player 3DS

Reproductor de MP3 local para Nintendo 3DS (homebrew), con interfaz táctil en la pantalla inferior, control por botones y varias medidas para gastar poca batería. Funciona en Old y New 3DS.

## Instalación

1. Compila el proyecto (ver abajo) y copia `mp3player3ds.3dsx` a `sdmc:/3ds/` en la SD.
2. Copia tus MP3 a `sdmc:/music/` (se leen también las subcarpetas, hasta 3 niveles y 1000 canciones).
3. Necesitas `sdmc:/3ds/dspfirm.cdc` (se obtiene con la app **DSP1**); sin él no hay audio.

## Compilar sin instalar nada (GitHub)

1. Crea un repositorio en github.com y sube el contenido de esta carpeta.
2. Asegúrate de que existe el archivo `.github/workflows/build.yml` (si no, créalo con *Add file → Create new file*).
3. Abre la pestaña **Actions**, espera al ✓ verde y descarga el artefacto `mp3player3ds`.

También con Docker:

```bash
docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm sh -c \
  "curl -L -o include/minimp3.h https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h && make"
```

## Controles

**Pantalla táctil (abajo):** barra de progreso (toca o arrastra para saltar), anterior / play-pausa / siguiente, Mezcla, Repetir (no / todo / una), volumen − y +, **Lista** (toca una canción para reproducirla, flechas para desplazar), **Apagar pantallas** y tiempo de apagado automático (10 s / 30 s / 60 s / nunca).

**Botones:**

| Botón | Acción |
|---|---|
| A | Reproducir la seleccionada / pausa |
| B | Parar |
| L / R | Anterior / siguiente |
| ← / → | −10 s / +10 s |
| ↑ / ↓ | Mover selección en la lista |
| X | Mezcla (shuffle) |
| Y | Repetir (no / todo / una) |
| Pad circular ↑ / ↓ | Volumen |
| SELECT | Apagar / encender pantallas |
| START | Salir |

Con las pantallas apagadas los botones siguen funcionando (por ejemplo, A, L, R, volumen). Para encender las pantallas toca la pantalla inferior o pulsa SELECT.

## Ahorro de batería

- No usa red: no inicia Wi-Fi ni servicios de internet.
- La interfaz solo se redibuja cuando cambia algo (como mucho una vez por segundo) y se apaga sola con el tiempo configurado.
- Con las pantallas apagadas, el programa casi no hace nada salvo decodificar audio.
- El hilo de audio duerme hasta que el DSP libera un buffer; decodifica en ráfagas y lee la SD en bloques de 48 KB.
- En New 3DS se mantiene el reloj normal (sin aceleración).

Consejos extra: apaga el Wi-Fi con el interruptor físico de la consola, baja el brillo desde el menú HOME y usa auriculares con cable. No he medido la duración de batería en una consola real.

## Limitaciones

- Solo MP3. Los nombres que se muestran son los de los archivos (no lee etiquetas ID3).
- La duración y los saltos son aproximados en MP3 de bitrate variable (VBR), marcados con `~`.
- Con la tapa cerrada la consola entra en suspensión; no garantizo que siga sonando.
- Código sin probar en hardware real: la primera compilación puede necesitar pequeños ajustes.
