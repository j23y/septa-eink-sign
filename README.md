# SEPTA bus + weather e-ink sign

A small e-ink display that shows the next SEPTA bus arrivals at one stop, plus the current weather and a short forecast. It runs on a single ESP32 e-paper board over Wi-Fi and updates every minute.

**Left side:** greeting, current temperature, "feels like", chance of precipitation, and a 4-step forecast (3 hours apart).
**Right side:** the clock, then the next 4 buses at your stop with minutes until arrival.

Built and tested with SEPTA Route 40 at Lombard St & 10th St in Philadelphia. Other SEPTA bus routes and stops should work by changing the settings below, but only Route 40 has been tested.

---

## What you need

| Item | Notes |
|---|---|
| **Elecrow CrowPanel ESP32 5.79" E-paper HMI Display** (792×272) | The only part. Screen, ESP32-S3 processor and Wi-Fi are on one board. [Elecrow wiki](https://elecrow.com/wiki/CrowPanel_ESP32_E-paper_5.79-inch_HMI_Display.html) · [Amazon listing](https://www.amazon.com/dp/B0FX4PDW6M) |
| USB-C **data** cable | Some cables only charge. If your computer doesn't see the board, try another cable. |
| USB power adapter | The sign needs constant USB power. Wi-Fi stays on and it updates every minute. |
| A 2.4 GHz Wi-Fi network | The ESP32 can't join 5 GHz-only networks. |
| A free [OpenWeather](https://openweathermap.org/) account | For the weather API key. The free key covered both the current-weather and forecast requests this uses. |

---

## Setup

### 1. Install the software
1. Install the [Arduino IDE](https://www.arduino.cc/en/software).
2. In **Boards Manager**, install **esp32** by Espressif Systems (this project was built with the version current in October 2026).
3. In **Library Manager**, install **Arduino_JSON** by Arduino.

### 2. Get the code
1. Download this repository (green **Code** button → **Download ZIP**) and unzip it.
2. Put the `septa_weather` folder in your Arduino sketchbook folder (on a Mac, `Documents/Arduino`).

### 3. Add Elecrow's display driver files
1. Download [Elecrow's CrowPanel 5.79" repository](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792).
2. From `example/arduino/Demos/5.79_wifi_http_openweather/`, copy these 7 files into your `septa_weather` folder:
   `EPD.cpp`, `EPD.h`, `EPD_Init.cpp`, `EPD_Init.h`, `EPDfont.h`, `spi.cpp`, `spi.h`

### 4. Add your Wi-Fi and weather key
1. In `septa_weather`, make a copy of `secrets.example.h` and name it **`secrets.h`**.
2. Fill in your Wi-Fi name, Wi-Fi password and OpenWeather API key.

`secrets.h` is listed in `.gitignore`, so it won't be uploaded if you push your own copy to GitHub.

### 5. Set your stop
Open `septa_weather.ino`. All settings are in the **YOUR SETTINGS** section near the top.

| Setting | What to put |
|---|---|
| `ROUTE_ID` | The bus route, e.g. `"40"` |
| `STOP_ID` | Your stop's ID (see below) |
| `DIRECTION_ID` | `0` or `1`, the direction that stops at your stop (see below) |
| `STOP_LABEL` | The stop name shown on screen, e.g. `"10th & Lombard St"` |
| `LAT`, `LON` | Location for the weather. Your stop's coordinates work well. |
| `YOUR_NAME` | Shown in the greeting ("Good Morning, ___!") |

**Finding your stop ID, direction and coordinates:** open this address in a browser, with your route number at the end:

```
https://www3.septa.org/api/v2/stops/?route_id=40
```

This returns every stop on the route. Find your stop by `stop_name` and copy its `stop_id`, `direction_id`, `stop_lat` and `stop_lon`. A stop can appear more than once with different `release_name` values, which are SEPTA schedule versions. The IDs are the same, so use either. If your street corner has a stop on each side, they have different IDs and directions, so pick the one for the direction you ride.

Optional settings in the same section:
- `SHOW_SCHEDULED`: set to `false` to hide buses that only have a scheduled time (see icons below).
- `PAUSE_START_HOUR` / `PAUSE_END_HOUR`: overnight pause for bus downloads. The default is midnight to 6 AM, and setting both to `0` turns it off.
- `BUS_NEAR_MINUTES` / `BUS_FAR_INTERVAL_MS`: download every minute when a bus is within 20 minutes, otherwise every 5 minutes.
- `WEATHER_INTERVAL_MS`: weather refresh, every 15 minutes by default.

### 6. Upload
1. Plug in the board and choose these settings in the **Tools** menu:
   - **Board:** ESP32S3 Dev Module
   - **PSRAM:** OPI PSRAM (required, since the real-time bus feed is loaded into PSRAM)
   - **Partition Scheme:** Huge APP (3MB No OTA/1MB SPIFFS)
   - **Upload Speed:** 115200. Uploads failed at the default 921600 on our board ("Unable to verify flash chip connection"), and 115200 fixed it.
   - **Port:** the `usbserial` entry that appears when the board is plugged in
2. Click **Upload** (the → button).
3. Open **Tools → Serial Monitor** at **115200** baud to watch it connect, download data and refresh the screen.

The sketch stays on the board after unplugging. When power returns, it reconnects and redraws on its own.

---

## How the bus times work

The sign combines three SEPTA sources:

1. **SEPTA's official real-time feed** (GTFS-Realtime, `rtTripUpdates.pb`). This has live predictions for upcoming trips. Google maps pulls from the same source.
2. **SEPTA's stop schedule** (`BusSchedules`). This fills in later trips that the real-time feed doesn't cover yet.
3. **SEPTA's trip list** (`api/v2/trips`). This gives destination names for buses already running, when the schedule doesn't have one.

**Icons next to the minutes:**
- **Two arcs:** real-time prediction with a vehicle assigned
- **One arc:** real-time prediction, no vehicle listed
- **No icon:** scheduled time only
  
---

## Changing the fonts

`sans_fonts.h` holds bitmap versions of DejaVu Sans, generated by `tools/make_fonts.py`. To change sizes, edit the `FONTS` list in that script and run:

```
pip install pillow
python3 tools/make_fonts.py /path/to/dejavu/fonts septa_weather/sans_fonts.h
```

The script needs `DejaVuSans.ttf` and `DejaVuSans-Bold.ttf` from [dejavu-fonts.github.io](https://dejavu-fonts.github.io/). If you add or rename sizes, update the matching names in `septa_weather.ino`.

---

## Known issues and limits

- **Screen occasionally goes white.** It tends to refresh after plugging and unplugging the usb-c. 
- **Celsius only.** Switching to Fahrenheit needs a code change in the weather request and display.
- **SEPTA only.** The bus code uses SEPTA's specific data feeds.
- **As regular users of SEPTA's Google Maps feed know, there are issues with ghost buses, inaccurate predictions, and broken GPSs. Hopefully SEPTA will continue to upgrade their real-time monitoring technology. 

---

## Credits and licenses

- This project's code: MIT License (see `LICENSE`).
- Display driver files: [Elecrow](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792), not included here.
- Fonts: [DejaVu Sans](https://dejavu-fonts.github.io/), converted to bitmaps. See `LICENSES/DejaVu-Fonts-License.txt`.
- Bus data: SEPTA. Weather data: OpenWeather.
