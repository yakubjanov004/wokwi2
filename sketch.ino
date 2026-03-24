#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>

// --- KONFIGURATSIYA ---
constexpr size_t TABLE_COUNT = 9;
constexpr uint8_t LCD_ADDR = 0x27;

// --- GLOBAL O'ZGARUVCHILAR ---
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);
int currentTableStatus[TABLE_COUNT + 1] = {0}; 

// I2C Master polling logic
void pollTables() {
    for (int id = 1; id <= TABLE_COUNT; id++) {
        uint8_t address = 0x08 + id;
        
        // 1. Send Status to Node
        Wire.beginTransmission(address);
        Wire.write((uint8_t)currentTableStatus[id]);
        Wire.endTransmission();

        // 2. Request Button State from Node
        Wire.requestFrom(address, (uint8_t)1);
        if (Wire.available()) {
            uint8_t btnPressed = Wire.read();
            if (btnPressed == 1) {
                currentTableStatus[id] = !currentTableStatus[id];
                Serial.printf("Stol %d: Status %s\n", id, currentTableStatus[id] ? "BAND" : "BO'SH");
            }
        }
        delay(5); 
    }
}

// Update LCD screen
void updateLcd() {
    lcd.setCursor(0, 0);
    for(int i=1; i<=5; i++) {
        lcd.print(i);
        lcd.print(currentTableStatus[i] ? "B " : "F ");
    }
    lcd.setCursor(0, 1);
    for(int i=6; i<=9; i++) {
        lcd.print(i);
        lcd.print(currentTableStatus[i] ? "B " : "F ");
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin(); 
    
    lcd.init();
    lcd.backlight();
    lcd.clear();
    lcd.print("SMART BILLIARD");
    delay(1000);
    lcd.clear();
}

unsigned long lastPoll = 0;
unsigned long lastLcd = 0;

void loop() {
    if (millis() - lastPoll > 150) {
        pollTables();
        lastPoll = millis();
    }
    if (millis() - lastLcd > 500) {
        updateLcd();
        lastLcd = millis();
    }
}
