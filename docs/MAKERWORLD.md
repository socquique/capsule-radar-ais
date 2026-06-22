# MakerWorld listing — Capsule Radar · Marine

Paste-ready copy for the MakerWorld model page. English first, Spanish below.
Cover images: use the built-device photos (tilt stand + glowing scope) and a slicer
screenshot of the plate.

---

## English

**Title:** Capsule Radar — Marine · Live AIS Ship Radar (ESP32-S3 Round AMOLED)

**Summary:** A glowing round desk radar on a tilt stand that plots real ships around you from a live AIS feed — magnetic snap-on back, built-in alert speaker, flashed from your browser in a minute. No AMS, no screws.

### 🚢 Capsule Radar — Marine

A polished desk gadget that turns the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (round 466×466 AMOLED, capacitive touch) into a **live AIS ship radar**. It pulls nearby vessels over WiFi and plots them on a glowing radar scope centred on your home, harbour, or a spot by a navigable river — useful by the coast *and* inland. Marine sibling of **Capsule Radar** (the aircraft version).

#### ✨ Features
- **Live vessel traffic** on an animated radar scope with a rotating sweep + coastline map.
- **Ship glyphs rotated by course (COG)**, fading trails; anchored/moored vessels as quiet dots.
- **Tap a vessel** → name, distance, speed (SOG), course, heading, nav status, destination.
- **Swipe** between Radar / List / Stats; on-screen range button (2 / 5 / 10 / 20 / 40 nm); themes.
- **Adjustable tilt stand** to angle the scope just right.
- **Magnetic snap-on back** — fast, tool-light assembly.
- **Built-in speaker** for zone / proximity alert pings (e.g. a vessel entering the harbour).
- Optional **GPS** auto-locates you (-G board variant).

#### 🖨️ Printing — no AMS / no multi-material needed
Each part prints in a **single colour**, so any printer works (I used a black case + orange stand).
- Parts: **front case/bezel**, **magnetic back cover (+ speaker holder)**, **tilt stand**.
- Material: **PLA or PETG** · Layer height: **0.2 mm** · Walls: **3** · Infill: **15%**
- Supports: **minimal — only on the bezel ring that holds the screen**.
- Total print time: **~2 hours**.
- ⚠️ **Board size:** the **bare PCB is ≈48.96 mm**; **with the Waveshare shell it's ≈51 mm**. This case fits the **bare PCB — remove the board from its shell** first.

#### 🧰 What you need
- **Waveshare ESP32-S3-Touch-AMOLED-1.75** board.
- **USB-C data cable** (power + flashing); optional 3.7 V LiPo (MX1.25) for cordless use.
- **6× ⌀6 × 2 mm neodymium disc magnets** (press-fit — 3 in the case, 3 in the back cover).
- The Waveshare **speaker** (MX1.25) for alert sounds (optional).
- **No screws** — the tilt pivot uses **printed pins**.
- A **free [aisstream.io](https://aisstream.io) API key** (non-commercial) for the live feed.

#### 🔩 Assembly (easy — no tools)
1. Press-fit the **6 magnets** into the case and back cover.
2. Take the board out of the Waveshare shell and seat it in the case.
3. Plug the speaker into the **SPK** connector and tuck it into the back.
4. Snap the **magnetic back** on.
5. Clip the case into the **tilt stand** with the **printed pins**; angle to taste.
6. **Flash & set up** (next).

#### ⚡ Flash it in ~60 seconds — no toolchain
Browser web-flasher (Chrome/Edge), plug in, click Install:
👉 **https://socquique.github.io/capsule-radar-ais/**
Then join the **`CapsuleMarine-Setup`** WiFi to enter your network, aisstream key and home location. Fine-tune later at `http://capsule-marine.local/`.

#### 🛠️ Open source
MIT-licensed firmware: **https://github.com/socquique/capsule-radar-ais** — PRs, themes & remixes welcome. Built with PlatformIO + LVGL + arduinoWebSockets.

#### 📝 Notes
Educational / **non-commercial** project. AIS data via **aisstream.io** (free tier — respect their terms); a self-hosted RTL-SDR / dAISy receiver works as a fully local alternative. Coverage is best near coasts, ports and busy waterways.

**Tags:** `ESP32` `ESP32-S3` `AIS` `radar` `marine` `boat` `ship-tracker` `AMOLED` `Waveshare` `desk-gadget` `LVGL` `nautical` `no-AMS`

---

## Español

**Título:** Capsule Radar — Marine · Radar de barcos AIS en vivo (ESP32-S3 AMOLED redonda)

**Resumen:** Un radar redondo de sobremesa sobre soporte basculante que dibuja los barcos reales a tu alrededor desde un feed AIS en vivo — trasera magnética, altavoz de avisos integrado, y se flashea desde el navegador en un minuto. Sin AMS, sin tornillos.

### 🚢 Capsule Radar — Marine

Un gadget de sobremesa que convierte la placa **Waveshare ESP32-S3-Touch-AMOLED-1.75** (AMOLED redonda 466×466, táctil capacitiva) en un **radar de barcos AIS en vivo**. Trae por WiFi los buques cercanos y los dibuja en un scope de radar centrado en tu casa, puerto o un punto junto a un río/canal navegable — útil en la costa *y* en interior. Hermano marino de **Capsule Radar** (la versión de aviones).

#### ✨ Características
- **Tráfico de buques en vivo** sobre un scope animado con barrido giratorio + mapa de costa.
- **Glifos de barco rotados por rumbo (COG)**, estelas que se desvanecen; fondeados/amarrados como puntos.
- **Toca un barco** → nombre, distancia, velocidad (SOG), rumbo, heading, estado de navegación, destino.
- **Desliza** entre Radar / Lista / Stats; botón de rango en pantalla (2 / 5 / 10 / 20 / 40 nm); temas.
- **Soporte basculante** para inclinar la pantalla a tu gusto.
- **Trasera magnética** — montaje rápido y casi sin herramientas.
- **Altavoz integrado** para avisos de zona / proximidad (p. ej. un barco entrando al puerto).
- **GPS** opcional que te autolocaliza (variante -G de la placa).

#### 🖨️ Impresión — sin AMS / sin multimaterial
Cada pieza se imprime a **un solo color**, así que vale cualquier impresora (yo usé carcasa negra + soporte naranja).
- Piezas: **carcasa/bisel frontal**, **tapa trasera magnética (+ alojamiento del altavoz)**, **soporte basculante**.
- Material: **PLA o PETG** · Altura de capa: **0,2 mm** · Perímetros: **3** · Relleno: **15%**
- Soportes: **mínimos — solo en el aro que sujeta la pantalla**.
- Tiempo total de impresión: **~2 horas**.
- ⚠️ **Tamaño de placa:** la **PCB desnuda mide ≈48,96 mm**; **con la carcasa Waveshare son ≈51 mm**. Esta carcasa encaja con la **PCB desnuda — saca la placa de su carcasa** primero.

#### 🧰 Qué necesitas
- Placa **Waveshare ESP32-S3-Touch-AMOLED-1.75**.
- **Cable USB-C de datos** (alimentación + flasheo); LiPo 3,7 V (MX1.25) opcional para usarlo sin cable.
- **6 imanes de neodimio ⌀6 × 2 mm** (a presión — 3 en la carcasa, 3 en la tapa).
- El **altavoz** Waveshare (MX1.25) para los avisos (opcional).
- **Sin tornillos** — el pivote del soporte usa **pasadores impresos**.
- Una **API key gratuita de [aisstream.io](https://aisstream.io)** (no comercial) para el feed.

#### 🔩 Montaje (fácil — sin herramientas)
1. Mete a presión los **6 imanes** en la carcasa y la tapa.
2. Saca la placa de la carcasa Waveshare y colócala en el alojamiento.
3. Conecta el altavoz al conector **SPK** y acomódalo en la trasera.
4. Encaja la **tapa magnética**.
5. Monta la carcasa en el **soporte basculante** con los **pasadores impresos**; inclínala a gusto.
6. **Flashea y configura** (abajo).

#### ⚡ Flasheo en ~60 s — sin toolchain
Flasher web (Chrome/Edge), enchufa, pulsa Install:
👉 **https://socquique.github.io/capsule-radar-ais/**
Luego únete al WiFi **`CapsuleMarine-Setup`** para meter tu red, la API key de aisstream y tu ubicación. Ajusta lo demás en `http://capsule-marine.local/`.

#### 🛠️ Open source
Firmware con licencia MIT: **https://github.com/socquique/capsule-radar-ais** — PRs, temas y remixes bienvenidos. Hecho con PlatformIO + LVGL + arduinoWebSockets.

#### 📝 Notas
Proyecto educativo / **no comercial**. Datos AIS vía **aisstream.io** (capa gratuita — respeta sus términos); un receptor propio RTL-SDR / dAISy funciona como alternativa totalmente local. La cobertura es mejor cerca de costas, puertos y vías navegables concurridas.

**Etiquetas:** `ESP32` `ESP32-S3` `AIS` `radar` `marine` `barcos` `AMOLED` `Waveshare` `gadget` `LVGL` `nautico` `sin-AMS`
