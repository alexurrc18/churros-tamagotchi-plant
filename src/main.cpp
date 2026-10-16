#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <AnimatedGIF.h>
#include <Preferences.h>

#include "animatii.h" 

// de modificat in functie de planta
const unsigned long WATER_INTERVAL = 3; // zile dintre udari
const int LIGHT_EXPOSURE = 200; //minim
const float HUMIDITY_EXPOSURE = 40.0; // umiditate minima
const float TEMPERATURE = 18.0; //minim temperatura
// -----------------------------------

const unsigned long DAY_IN_MS = 86400000UL;
const unsigned long WATER_TIMEOUT = WATER_INTERVAL * DAY_IN_MS; 

#define SOIL_PIN 4
#define LIGHT_PIN 5
#define I2C_SDA 7
#define I2C_SCL 6

Adafruit_AHTX0 aht;
AnimatedGIF gif;
Preferences memory; 

String currentPlantState = "neutral";
String previousPlantState = "";
unsigned long lastSensorCheck = 0;

int neglectLevel = 0;
const int angryThreshold = 5;

unsigned long lastWateringTime = 0; 
unsigned long lastLightTime = 0;
unsigned long lastCareCheckTime = 0;

bool isHydrated = true; 

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.pin_sclk = 11;
      cfg.pin_mosi = 12;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 1;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs          = 13;
      cfg.pin_rst         = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 240;
      cfg.invert          = true;
      cfg.rgb_order       = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX lcd;

void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s = pDraw->pPixels;
  uint16_t *pPalette = (uint16_t *)pDraw->pPalette;
  int x = pDraw->iX;
  int y = pDraw->iY + pDraw->y;
  int w = pDraw->iWidth;

  if (x + w > 240) w = 240 - x;
  if (w <= 0 || y >= 240) return;

  if (pDraw->ucHasTransparency) {
    uint8_t ucTransparent = pDraw->ucTransparent;
    for (int i = 0; i < w; i++) {
      uint8_t index = *s++;
      if (index != ucTransparent) lcd.drawPixel(x + i, y, pPalette[index]);
    }
  } else {
    lcd.startWrite();
    lcd.setAddrWindow(x, y, w, 1);
    for (int i = 0; i < w; i++) lcd.writeColor(pPalette[*s++], 1);
    lcd.endWrite();
  }
}

void playGif(const uint8_t *gifData, int gifSize, int repetitions, bool freezeLastFrame) {
  gif.begin(BIG_ENDIAN_PIXELS);
  if (gif.open((uint8_t *)gifData, gifSize, GIFDraw)) {
    int delay_ms = 0;
    for (int r = 0; r < repetitions; r++) {
      while (gif.playFrame(true, &delay_ms)) {
        if (delay_ms > 0) delay(delay_ms);
        yield();
      }
      if (r < repetitions - 1) gif.reset();
    }
    if (!freezeLastFrame) gif.close();
  }
}

void setup() {
  Serial.begin(115200);
  
  memory.begin("logs", false); 
  neglectLevel = memory.getInt("neglect", 0); 
  isHydrated = memory.getBool("hydrated", true); 

  unsigned long currentMillis = millis();
  lastCareCheckTime = currentMillis;
  lastLightTime = currentMillis;

  if (isHydrated) {
    lastWateringTime = currentMillis;
  } else {
    lastWateringTime = currentMillis - WATER_TIMEOUT;
  }

  lcd.setColorDepth(16);
  lcd.init();
  lcd.setRotation(0);
  lcd.fillScreen(TFT_BLACK);

  Wire.begin(I2C_SDA, I2C_SCL);
  aht.begin();
  randomSeed(analogRead(9)); 

  playGif(blink, sizeof(blink), 1, true);
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastSensorCheck >= 2000) {
    lastSensorCheck = currentMillis;

    int oldNeglect = neglectLevel;
    bool oldHydrated = isHydrated;

    int soilValue = analogRead(SOIL_PIN);
    int lightValue = analogRead(LIGHT_PIN);
    sensors_event_t humidity, temp;
    aht.getEvent(&humidity, &temp);

    if (soilValue < 2000) { 
      if (!isHydrated) { 
         if (neglectLevel > 0) neglectLevel--;
      }
      lastWateringTime = currentMillis;
      isHydrated = true;
    }
    
    if ((currentMillis - lastWateringTime) >= WATER_TIMEOUT || soilValue > 3000) {
      isHydrated = false;
    }

    if (lightValue >= LIGHT_EXPOSURE) {
      lastLightTime = currentMillis;
    }

    if (currentMillis - lastCareCheckTime >= DAY_IN_MS) {
      lastCareCheckTime = currentMillis;

      if (currentMillis - lastLightTime >= DAY_IN_MS) {
        if (neglectLevel < 20) neglectLevel++;
        lastLightTime = currentMillis;
      }

      // verificare environment
      bool isEnvironmentBad = (soilValue > 3000 || (temp.temperature < TEMPERATURE && temp.temperature > 0) || humidity.relative_humidity < HUMIDITY_EXPOSURE);
      
      if (isEnvironmentBad || !isHydrated) {
        if (neglectLevel < 20) neglectLevel++;
      } else {
        if (neglectLevel > 0) neglectLevel--;
      }
    }

    // salvare memorie
    if (neglectLevel != oldNeglect || isHydrated != oldHydrated) {
      memory.putInt("neglect", neglectLevel);
      memory.putBool("hydrated", isHydrated);
    }

    // setare stari
    currentPlantState = "neutral"; 
    
    if (lightValue < LIGHT_EXPOSURE) {
      currentPlantState = "sleep";
    }
    else if (neglectLevel >= angryThreshold) {
      currentPlantState = "angry";
    }
    else if (!isHydrated) {
      currentPlantState = "sad";
    }
    else if (temp.temperature < TEMPERATURE && temp.temperature > 0) {
      currentPlantState = "cold";
    }
    else if (temp.temperature >= TEMPERATURE && temp.temperature <= TEMPERATURE + 8.0 && soilValue < 1500 && humidity.relative_humidity >= HUMIDITY_EXPOSURE) {
      currentPlantState = "flower";
    }
    else if (temp.temperature >= TEMPERATURE && temp.temperature <= TEMPERATURE + 10.0 && soilValue < 2500) {
      currentPlantState = "happy";
    }

    // debug info
    Serial.println("=================== DEBUG INFO =====================");
    Serial.printf("State: %s | Neglect Level: %d\n", currentPlantState.c_str(), neglectLevel);
    Serial.printf("Soil: %d | Light: %d | Temp: %.1fC | Hum: %.1f%%\n", soilValue, lightValue, temp.temperature, humidity.relative_humidity);
    Serial.printf("Time in dark (hours): %.2f | Is Hydrated: %s\n", (float)(currentMillis - lastLightTime) / 3600000.0, isHydrated ? "YES" : "NO");

    // animatii
    if (currentPlantState != previousPlantState) {
      
      // iesire stare
      if (previousPlantState != "" && previousPlantState != "neutral") {
        if (previousPlantState == "angry") playGif(blinkToAngry_reversed, sizeof(blinkToAngry_reversed), 1, true);
        else if (previousPlantState == "happy") playGif(blinkToHappy_reversed, sizeof(blinkToHappy_reversed), 1, true);
        else if (previousPlantState == "flower") playGif(blinkToFlower_reversed, sizeof(blinkToFlower_reversed), 1, true);
        else if (previousPlantState == "sad") playGif(blinkToSad_reversed, sizeof(blinkToSad_reversed), 1, true);
        else if (previousPlantState == "sleep") playGif(blinkToSleep_reversed, sizeof(blinkToSleep_reversed), 1, true);
        else if (previousPlantState == "cold") {
          playGif(sadBlink, sizeof(sadBlink), 1, true);
          playGif(blinkToSad_reversed, sizeof(blinkToSad_reversed), 1, true);
        }
        delay(100); 
      }

      // intrare stare
      if (currentPlantState == "angry") {
        playGif(blinkToAngry, sizeof(blinkToAngry), 1, true);
        playGif(angryBlink, sizeof(angryBlink), 1, true);
      } 
      else if (currentPlantState == "happy") {
        playGif(blinkToHappy, sizeof(blinkToHappy), 1, true);
        playGif(happyBlink, sizeof(happyBlink), 1, true);
      }
      else if (currentPlantState == "flower") {
        playGif(blinkToFlower, sizeof(blinkToFlower), 1, true);
        playGif(flowerBlink, sizeof(flowerBlink), 1, true);
      }
      else if (currentPlantState == "sleep") {
        playGif(blinkToSleep, sizeof(blinkToSleep), 1, true);
      } 
      else if (currentPlantState == "sad") {
        playGif(blinkToSad, sizeof(blinkToSad), 1, true);
        playGif(sadBlink, sizeof(sadBlink), 1, true);
      } 
      else if (currentPlantState == "cold") {
        playGif(blinkToSad, sizeof(blinkToSad), 1, true);
        playGif(sadBlink, sizeof(sadBlink), 1, true);
        playGif(cold, sizeof(cold), 1, true);
      } 
      else {
        playGif(blink, sizeof(blink), 1, true); 
      }
      
      previousPlantState = currentPlantState;
    } 
    else {
      // animatii in timp ce sta in stare 
      if (currentPlantState == "sleep") {
        playGif(sleeping, sizeof(sleeping), 1, true);
      } 
      else if (random(0, 100) < 15) {
        if (currentPlantState == "angry")       playGif(angryBlink, sizeof(angryBlink), 1, true);
        else if (currentPlantState == "happy")  playGif(happyBlink, sizeof(happyBlink), 1, true);
        else if (currentPlantState == "flower") playGif(flowerBlink, sizeof(flowerBlink), 1, true);
        else if (currentPlantState == "sad")    playGif(sadBlink, sizeof(sadBlink), 1, true);
        else if (currentPlantState == "neutral")playGif(blink, sizeof(blink), 1, true);
      }
    }
  }
}