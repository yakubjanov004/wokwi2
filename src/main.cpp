#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <vector>

// --- KONFIGURATSIYA ---
#define NUM_TABLES 9
#define PIXELS_PER_TABLE 6 // Har bir stolga 6 tadan RGB LED
#define TOTAL_PIXELS (NUM_TABLES * PIXELS_PER_TABLE)
#define LED_PIN 13
#define HOURLY_RATE_NORMAL 30000
#define HOURLY_RATE_VIP 50000

// Tugmalar pinlari
const uint8_t BUTTON_PINS[NUM_TABLES] = {4, 15, 16, 17, 18, 19, 23, 25, 26};

// LCD va LEDlar
LiquidCrystal_I2C lcd(0x27, 16, 2);
Adafruit_NeoPixel pixels(TOTAL_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- GLOBAL O'ZGARUVCHILAR ---
struct Table {
  int id;
  bool active;
  String status; // "free", "busy", "booked", "vip"
  uint32_t elapsedSeconds;
  long gameAmount;
  long ordersAmount;
  long totalDue;
} tables[NUM_TABLES];

struct OrderItem {
  String name;
  int qty;
  long price;
};

struct Order {
  String id;
  int tableId;
  String status; // "new", "preparing", "ready", "delivered", "cancelled"
  String time;
  String note;
  std::vector<OrderItem> items;
};

long dailyRevenue = 0;
std::vector<String> logs;
std::vector<Order> orders;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

String getClockText() {
  // Wokwi simulyatsiya uchun oddiy virtual soat.
  uint32_t totalMinutes = (millis() / 60000UL) % (24UL * 60UL);
  uint8_t hh = totalMinutes / 60;
  uint8_t mm = totalMinutes % 60;
  char buf[6];
  sprintf(buf, "%02u:%02u", hh, mm);
  return String(buf);
}

String getTimeStampText() {
  return getClockText();
}

uint32_t getStatusColor(String status) {
  if (status == "busy")   return pixels.Color(255, 0, 0);     // Qizil
  if (status == "vip")    return pixels.Color(128, 0, 128);   // Binafsha
  if (status == "booked") return pixels.Color(255, 165, 0);   // Olovrang
  return pixels.Color(0, 255, 0);                             // Yashil
}

void updateLEDs() {
  Serial.println("LEDlar yangilanmoqda...");
  for (int i = 0; i < NUM_TABLES; i++) {
    uint32_t color = getStatusColor(tables[i].status);
    for (int j = 0; j < PIXELS_PER_TABLE; j++) {
      pixels.setPixelColor(i * PIXELS_PER_TABLE + j, color);
    }
  }
  pixels.show();
}

void addLog(String msg) {
  String entry = "LOG: " + msg;
  logs.push_back(entry);
  if (logs.size() > 30) logs.erase(logs.begin());
  Serial.println(entry);
}

int getOrdersCountForTable(int tableId) {
  int count = 0;
  for (const auto& order : orders) {
    if (order.tableId == tableId && order.status != "delivered" && order.status != "cancelled") {
      count++;
    }
  }
  return count;
}

long getOrdersAmountForTable(int tableId) {
  long total = 0;
  for (const auto& order : orders) {
    if (order.tableId != tableId) continue;
    if (order.status == "cancelled") continue;
    for (const auto& item : order.items) {
      total += (item.price * item.qty);
    }
  }
  return total;
}

int findOrderIndexById(const String& id) {
  for (size_t i = 0; i < orders.size(); i++) {
    if (orders[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int resolveTableIndex(AsyncWebServerRequest *request) {
  int id = -1;
  if (request->hasParam("id")) id = request->getParam("id")->value().toInt();
  if (request->hasParam("t")) id = request->getParam("t")->value().toInt();
  id -= 1;
  if (id < 0 || id >= NUM_TABLES) return -1;
  return id;
}

String formatTime(uint32_t seconds) {
  uint32_t h = seconds / 3600;
  uint32_t m = (seconds % 3600) / 60;
  uint32_t s = seconds % 60;
  char buf[10];
  sprintf(buf, "%02u:%02u:%02u", h, m, s);
  return String(buf);
}

String formatMoney(long n) {
  char buf[20];
  sprintf(buf, "%ld", n);
  String s = String(buf);
  int len = s.length();
  String res = "";
  for (int i = 0; i < len; i++) {
    res += s[i];
    if ((len - i - 1) % 3 == 0 && (len - i - 1) != 0) res += " ";
  }
  return res + " so'm";
}

void updateTableTotal(int i) {
  tables[i].ordersAmount = getOrdersAmountForTable(tables[i].id);
  long rate = (tables[i].status == "vip") ? HOURLY_RATE_VIP : HOURLY_RATE_NORMAL;
  tables[i].gameAmount = (tables[i].elapsedSeconds * rate) / 3600;
  tables[i].totalDue = tables[i].gameAmount + tables[i].ordersAmount;
}

void broadcastData() {
  JsonDocument doc;
  doc["daily_revenue"] = dailyRevenue;
  doc["active_tables"] = 0;
  doc["uptime_display"] = formatTime(millis() / 1000);
  doc["clock"] = getClockText();

  JsonArray tablesArr = doc["tables"].to<JsonArray>();
  for (int i = 0; i < NUM_TABLES; i++) {
    updateTableTotal(i);
    if (tables[i].active) doc["active_tables"] = (int)doc["active_tables"] + 1;

    JsonObject tDoc = tablesArr.add<JsonObject>();
    tDoc["id"] = tables[i].id;
    tDoc["status"] = tables[i].status;
    tDoc["active"] = tables[i].active;
    tDoc["elapsed_display"] = formatTime(tables[i].elapsedSeconds);
    tDoc["amount"] = tables[i].gameAmount;
    tDoc["orders_count"] = getOrdersCountForTable(tables[i].id);
    tDoc["orders_amount"] = tables[i].ordersAmount;
    tDoc["total_due"] = tables[i].totalDue;
  }

  JsonArray ordersArr = doc["orders"].to<JsonArray>();
  for (const auto& order : orders) {
    JsonObject o = ordersArr.add<JsonObject>();
    o["id"] = order.id;
    o["tableId"] = order.tableId;
    o["status"] = order.status;
    o["time"] = order.time;
    o["note"] = order.note;
    JsonArray itemsArr = o["items"].to<JsonArray>();
    for (const auto& item : order.items) {
      JsonObject i = itemsArr.add<JsonObject>();
      i["name"] = item.name;
      i["qty"] = item.qty;
      i["price"] = item.price;
    }
  }

  JsonArray logsArr = doc["logs"].to<JsonArray>();
  for (const auto& log : logs) logsArr.add(log);
  
  String output;
  serializeJson(doc, output);
  ws.textAll(output);
}

void setTableStatus(int i, String newStatus) {
  if (newStatus == "free") {
    if (tables[i].active) dailyRevenue += tables[i].totalDue;
    tables[i].active = false;
    tables[i].elapsedSeconds = 0;
    tables[i].gameAmount = 0;
    tables[i].totalDue = 0;
  } else if (newStatus == "busy" || newStatus == "vip") {
    tables[i].active = true;
    tables[i].elapsedSeconds = 0;
    tables[i].gameAmount = 0;
    tables[i].ordersAmount = getOrdersAmountForTable(tables[i].id);
  } else if (newStatus == "booked") {
    tables[i].active = false;
  }
  tables[i].status = newStatus;
  addLog("Stol " + String(tables[i].id) + " -> " + newStatus);
  updateLEDs();
  broadcastData();
}

void addDefaultOrder(int tableId, const String& note) {
  Order order;
  order.id = "ORD-" + String(millis());
  order.tableId = tableId;
  order.status = "new";
  order.time = getTimeStampText();
  order.note = note;
  order.items.push_back({"Choy", 1, 5000});
  order.items.push_back({"Suv", 1, 3000});
  orders.push_back(order);
  addLog("Buyurtma yaratildi: " + order.id + " (Stol " + String(tableId) + ")");
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- Smart Billiard Master Starting ---");

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Error: Bo'sh filesystem yaratilmoqda yoki xatolik!");
  } else {
    Serial.println("LittleFS OK.");
    // Fayllarni ko'rsatish
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
      Serial.print(" - "); Serial.print(file.name());
      Serial.print(" ("); Serial.print(file.size()); Serial.println(" bytes)");
      file = root.openNextFile();
    }
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Smart_Billiard_Master", "12345678");
  Serial.print("Connecting to Wokwi-GUEST");
  WiFi.begin("Wokwi-GUEST", "");
  
  // Wokwi network starts automatically, but we log the status
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 8000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("SUCCESS! STA IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Web server: http://localhost:9080/");
  } else {
    Serial.println("Wokwi-GUEST failure. Proxy 'localhost:9080' works ONLY with Wokwi-GUEST.");
  }

  for (int i = 0; i < NUM_TABLES; i++) {
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);
    tables[i].id = i + 1;
    tables[i].active = false;
    tables[i].status = "free";
    tables[i].elapsedSeconds = 0;
    tables[i].gameAmount = 0;
    tables[i].ordersAmount = 0;
    tables[i].totalDue = 0;
  }

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  pixels.begin(); pixels.setBrightness(150); updateLEDs();

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/index.html", "text/html"); });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/index.html", "text/html"); });
  server.on("/admin.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/admin.html", "text/html"); });
  server.on("/barmen.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/barmen.html", "text/html"); });
  server.on("/cashier.html", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/cashier.html", "text/html"); });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request){
    int id = resolveTableIndex(request);
    if (id >= 0) {
      if (tables[id].active) setTableStatus(id, "free");
      else setTableStatus(id, "busy");
    }
    request->send(200, "text/plain", "OK");
  });

  server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request){
    int id = resolveTableIndex(request);
    if (id < 0) {
      request->send(400, "text/plain", "Invalid table");
      return;
    }
    String mode = request->hasParam("mode") ? request->getParam("mode")->value() : "hourly";
    if (mode == "vip") setTableStatus(id, "vip");
    else setTableStatus(id, "busy");
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
    int id = resolveTableIndex(request);
    if (id < 0) {
      request->send(400, "text/plain", "Invalid table");
      return;
    }
    String payment = request->hasParam("payment") ? request->getParam("payment")->value() : "cash";
    addLog("To'lov qabul qilindi: Stol " + String(id + 1) + ", usul=" + payment);
    setTableStatus(id, "free");
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/book", HTTP_GET, [](AsyncWebServerRequest *request){
    int id = resolveTableIndex(request);
    if (id < 0) {
      request->send(400, "text/plain", "Invalid table");
      return;
    }
    setTableStatus(id, "booked");
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/call", HTTP_GET, [](AsyncWebServerRequest *request){
    int id = resolveTableIndex(request);
    if (id < 0) {
      request->send(400, "text/plain", "Invalid table");
      return;
    }
    String type = request->hasParam("type") ? request->getParam("type")->value() : "service";
    if (type == "barmen") {
      addDefaultOrder(id + 1, "Stoldan chaqiruv");
    }
    addLog("Chaqiruv: Stol " + String(id + 1) + " (" + type + ")");
    broadcastData();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/orders", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->send(200, "application/json", "{\"ok\":true}");
    },
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      (void)request;
      if (index != 0 || len != total) return;

      JsonDocument in;
      DeserializationError err = deserializeJson(in, data, len);
      if (err) return;

      String id = in["id"] | "";
      String status = in["status"] | "";
      int oi = findOrderIndexById(id);
      if (oi >= 0 && status.length() > 0) {
        orders[oi].status = status;
        addLog("Buyurtma holati yangilandi: " + id + " -> " + status);
        broadcastData();
      }
    }
  );

  server.addHandler(&ws);
  ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    (void)server;
    (void)client;
    (void)arg;
    (void)data;
    (void)len;
    if (type == WS_EVT_CONNECT) {
      broadcastData();
    }
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    Serial.print("404 Found: "); Serial.println(request->url());
    request->send(404, "text/plain", "Not Found (LittleFS)");
  });

  server.serveStatic("/", LittleFS, "/");
  server.begin();
  addLog("Master tizim tayyor (6 LEDs per table).");
  broadcastData();
}

void loop() {
  static uint32_t lastTick = 0;
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    for (int i = 0; i < NUM_TABLES; i++) {
      if (tables[i].active) {
        tables[i].elapsedSeconds++;
        long rate = (tables[i].status == "vip") ? HOURLY_RATE_VIP : HOURLY_RATE_NORMAL;
        tables[i].totalDue = (tables[i].elapsedSeconds * rate) / 3600;
      }
    }
    broadcastData();
  }

  static bool lastBtn[NUM_TABLES];
  static bool firstRun = true;
  if (firstRun) {
    for (int i = 0; i < NUM_TABLES; i++) lastBtn[i] = HIGH;
    firstRun = false;
  }
  
  static uint32_t lastDebounceMs[NUM_TABLES] = {0};
  for (int i = 0; i < NUM_TABLES; i++) {
    bool state = digitalRead(BUTTON_PINS[i]);
    if (state == LOW && lastBtn[i] == HIGH && (millis() - lastDebounceMs[i]) > 180) {
      if (tables[i].active) setTableStatus(i, "free");
      else setTableStatus(i, "busy");
      lastDebounceMs[i] = millis();
    }
    lastBtn[i] = state;
  }
}
