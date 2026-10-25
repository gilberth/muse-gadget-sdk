# ElevenLabs directo en Muse Gadget (Waveshare ESP32-S3 LCD 1.54)

El ESP32 consulta la **API pública de ElevenLabs directamente por HTTPS**.
**No requiere Docker, VPS, proxy ni servicio local**. Conserva el Muse
Gadget SDK, su Jollybot original, menús, subtítulos, emparejamiento y OTA.
Está diseñado para `esp32/devices/sdkconfig.muse-waveshare-s3-lcd154`.

## Arquitectura

```text
Micrófonos ES7210 → Muse AI (voz y texto)
                                ↓
                       texto final de Muse
                                ↓
ESP32 HTTPS → api.elevenlabs.io (xi-api-key + Voice ID)
                                ↓
MP3 22.05 kHz / 32 kbps → minimp3 de Muse → PCM 16 kHz → ES8311
                                ↓
Jollybot original SPEAKING + subtítulos sincronizados
```

Se usa el API oficial `POST /v1/text-to-speech/{voice_id}/stream`,
modelo `eleven_flash_v2_5` y `output_format=mp3_22050_32`.
Las llamadas TLS verifican la cadena de certificados de ESP-IDF.
El cliente no sigue redirecciones y los errores vuelven a subtítulos sin voz.

## Primera instalación / compilación

```bash
cd "/Users/gilberth/Downloads/sdk muse/esp32"
source ~/esp/esp-idf-v6.0.1/export.sh
git fetch origin
git switch feat/elevenlabs-tts-proxy
git pull --ff-only
tools/muse/board.sh build lcd154
```

El perfil Waveshare incluye `CONFIG_MUSE_TTS_ELEVENLABS=y` por
defecto (sin cambiar ajustes de la interfaz original).
Al compilar no hace falta una cuenta ElevenLabs ni conexión al servicio.
**No agregues ninguna API key al archivo `sdkconfig` ni al repositorio.**

Instala la imagen en el mismo ESP32 que ya funciona, **sin ejecutar**
`erase-flash`, para conservar NVS y el vínculo con Muse:

```bash
tools/muse/board.sh flash lcd154 /dev/cu.usbmodemXXXX
```

Verifica el puerto real antes de flashear; se exige el puerto explícito
en este perfil experimental.

## Configurar ElevenLabs una vez por USB

1. Genera una API key en https://elevenlabs.io/app/settings/api-keys.
   Idealmente usa una clave restringida al endpoint de síntesis, con límites
   de consumo.
2. Copia el `voice_id` de la voz que deseas utilizar desde tu cuenta.
3. Conecta el ESP32, cierra cualquier monitor serie y ejecuta:

```bash
python3 tools/muse/elevenlabs_setup.py --port /dev/cu.usbmodemXXXX
```

El script solicita la API key por **entrada oculta**, y el Voice ID por
entrada normal. Los envía directamente por el USB serie y **no los imprime,
guarda en archivos ni agrega al historial de la shell**. Puedes volver a
ejecutarlo para actualizar la clave o voz.

Internamente están disponibles los comandos de Muse:

```text
>tts.key=TU_API_KEY
>tts.voice=VOICE_ID
>tts.status
>tts.clear
```

Los comandos `tts.key` y `tts.voice` se almacenan en el namespace NVS
`eleven_tts`. `tts.clear` elimina esos valores NVS y desactiva la voz.
Evita pegar la clave como argumento de shell o publicarla en capturas.

## Seguridad

La **API key no aparece en el firmware compilado ni en GitHub**: el
firmware contiene el cliente HTTP y su configuración está en NVS.
Sin embargo, la NVS ordinaria **no está cifrada por defecto**; una persona
con acceso físico y capacidad de leer la Flash podría recuperar el secreto.
Para protegerlo frente a ese escenario, hace falta habilitar y provisionar
**NVS Encryption / Flash Encryption** correctamente. Esto se debe evaluar
antes de distribuir el producto a terceros.

La clave sí viaja por HTTPS a `api.elevenlabs.io`, que es necesario para
la síntesis remota de voz. No existe un modelo ElevenLabs local en el ESP32.

## Pruebas

- Revisa que Settings → Sound → Volume sigue reproduciendo el chirp.
- Pregunta algo corto a Muse y escucha el audio de ElevenLabs.
- Confirma la animación original `SPEAKING` y los subtítulos.
- Prueba otro turno y la interrupción pulsando BOOT.
- Borra temporalmente la clave con `tts.clear` y comprueba el
  funcionamiento original sin voz. Configúrala de nuevo después.
- Los fallos de HTTP/clave inválida dejan los subtítulos de Muse disponibles.

Logs esperados: `muse_tts_elevenlabs` y `muse_chat_session`.
El motor TTS recibe mensajes completos, hasta 1023 bytes de texto por
mensaje (límite del almacenamiento de texto existente en Muse).


## Diagnóstico TLS: certificado CA no encontrado

Si Muse responde por texto y el comando serie `m` reproduce audio,
pero aparecen los errores `No matching trusted root certificate found` y
`ESP_ERR_HTTP_CONNECT`, el fallo ocurre **antes del HTTP** (no es todavía
un fallo de Voice ID o API key).

Este perfil activa el bundle completo y
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` para cadenas de
CA con certificados cruzados (ESP-IDF 6.0.1). Esto **mantiene obligatoria**
la validación del servidor.

En el Mac, comprueba la cadena realmente presentada por la red, sin
proporcionar ninguna API key:

```sh
curl -Iv --connect-timeout 10 https://api.elevenlabs.io
echo | openssl s_client -connect api.elevenlabs.io:443 -servername api.elevenlabs.io -showcerts -verify_return_error 2>&1 | grep -E 'depth=|subject=|issuer=|Verify return code|verify error'
```

`curl` debe validar la cadena TLS incluso si la URL devuelve HTTP 404
o 405 (el código HTTP no importa para este diagnóstico). Si `curl` falla,
comprueba proxy TLS, DNS, hora del dispositivo y certificados de tu red.
**No uses `skip_cert_common_name_check`, `cert_pem = NULL` o
`CONFIG_ESP_TLS_INSECURE` para que arranque.** Nunca copies como
"certificado confiable" uno interceptado sin verificar su procedencia.

Tras actualizar el código, verifica en el `sdkconfig` efectivo (puede
mantener ajustes de una compilación previa):

```sh
grep -E 'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_(DEFAULT_FULL|CROSS_SIGNED_VERIFY)' build-muse-waveshare-s3-lcd154/sdkconfig
```

Si la opción `CROSS_SIGNED_VERIFY` permanece en `# ... is not set`,
actívala en `idf.py menuconfig` bajo **Component config → mbedTLS →
Certificate Bundle** (o recrea la configuración del perfil sin borrar
NVS), y vuelve a compilar antes de flashear. Si aún falla tras activar
esta opción, compara el `issuer` de la conexión del Mac con las CA
incluidas en el bundle, en lugar de desactivar TLS.

