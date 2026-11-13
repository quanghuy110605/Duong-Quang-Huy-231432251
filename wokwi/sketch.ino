#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================
// CẤU HÌNH PHẦN CỨNG & CHÂN KẾT NỐI (ESP32 - KHỚP DIAGRAM.JSON)
// ============================================================
#define MQ2_PIN     34  // Chân Analog ADC1_CH6 đọc cảm biến khí gas MQ-2 (gas1:AOUT)
#define TEMP_PIN    4   // Chân 1-Wire đọc cảm biến nhiệt độ DS18B20 (temp1:DQ)
#define FAN_PIN     18  // LED Xanh lá (led2 qua r1) - Đại diện cho Quạt thông gió
#define PUMP_PIN    19  // LED Đỏ (led1 qua r2) - Đại diện cho Máy bơm nước
#define BUZZER_PIN  5   // Còi cảnh báo (bz1)

// Ngưỡng cảnh báo mặc định
#define GAS_THRESHOLD_PCT   50.0f   // Ngưỡng nồng độ khí gas (%) -> Bật Quạt (GPIO 18)
#define TEMP_THRESHOLD_C    45.0f   // Ngưỡng nhiệt độ (°C) -> Bật Máy bơm (GPIO 19)

// ============================================================
// CẤU HÌNH MẠNG WI-FI & MQTT BROKER
// ============================================================
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

const char* MQTT_BROKER   = "broker.emqx.io";
const int   MQTT_PORT     = 1883;
const char* TOPIC_DATA    = "lab3/fire_alarm/data";
const char* TOPIC_CONTROL = "lab3/fire_alarm/control";

// ============================================================
// CẤU TRÚC DỮ LIỆU & ĐỐI TƯỢNG FREERTOS
// ============================================================
typedef struct {
  int rawGas;           // Giá trị ADC thô (0 - 4095)
  float gasPercent;     // Nồng độ khí gas quy đổi (%)
  int gasPPM;           // Nồng độ khí gas ước lượng (PPM)
  float temperature;    // Nhiệt độ (°C) từ DS18B20
  uint32_t timestamp;   // Thời điểm đọc (ms)
} SensorData_t;

// Hàng đợi (Queue) giao tiếp giữa các Task
QueueHandle_t dataQueue = NULL;   // TaskReadSensor -> TaskTransmitData
QueueHandle_t dataAlert = NULL;   // TaskReadSensor -> TaskAlertControl

// Cờ hiệu (Mutex Semaphore) bảo vệ tài nguyên dùng chung
SemaphoreHandle_t xSerialMutex  = NULL;
SemaphoreHandle_t xControlMutex = NULL;

// Biến trạng thái điều khiển và cơ cấu chấp hành
volatile bool manualMode       = false;
volatile bool manualFanState   = false;
volatile bool manualPumpState  = false;
volatile bool currentFanState  = false;
volatile bool currentPumpState = false;
volatile bool currentGasAlert  = false;
volatile bool currentTempAlert = false;

// Đối tượng cảm biến DS18B20 & mạng
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ============================================================
// HÀM HỖ TRỢ IN LOG AN TOÀN VỚI MUTEX
// ============================================================
void safePrint(const String& msg) {
  if (xSerialMutex != NULL && xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    Serial.println(msg);
    xSemaphoreGive(xSerialMutex);
  }
}

// ============================================================
// CALLBACK NHẬN LỆNH ĐIỀU KHIỂN TỪ MQTT BROKER (WEB -> ESP32)
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  safePrint("[MQTT RX] Topic: " + String(topic) + " | Payload: " + message);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    safePrint("[MQTT ERROR] Loi giai ma JSON lenh dieu khien!");
    return;
  }

  if (xSemaphoreTake(xControlMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    if (doc.containsKey("mode")) {
      String modeStr = doc["mode"].as<String>();
      modeStr.toLowerCase();
      manualMode = (modeStr == "manual");
    }
    if (doc.containsKey("fan")) {
      manualFanState = doc["fan"].as<bool>();
      if (!doc.containsKey("mode")) {
        manualMode = true;
      }
    }
    if (doc.containsKey("pump")) {
      manualPumpState = doc["pump"].as<bool>();
      if (!doc.containsKey("mode")) {
        manualMode = true;
      }
    }
    xSemaphoreGive(xControlMutex);
  }
}

// ============================================================
// TASK 1: TASK ĐỌC DATA (CHẠY TRÊN CORE 1)
// Đọc MQ2 (GPIO 34) + DS18B20 (GPIO 4), in Serial và đẩy vào dataQueue & dataAlert
// ============================================================
void TaskReadSensor(void* pvParameters) {
  (void)pvParameters;
  safePrint("[TaskReadSensor] Khoi chay tren Core " + String(xPortGetCoreID()));

  for (;;) {
    // 1. Đọc giá trị cảm biến khí gas MQ-2 (ADC 12-bit: 0 - 4095)
    int rawGas = analogRead(MQ2_PIN);
    float gasPct = (rawGas / 4095.0f) * 100.0f;
    int gasPpm = map(rawGas, 0, 4095, 0, 10000);

    // 2. Đọc nhiệt độ từ cảm biến DS18B20 (GPIO 4)
    tempSensor.requestTemperatures();
    float temp = tempSensor.getTempCByIndex(0);

    if (temp == DEVICE_DISCONNECTED_C || isnan(temp)) {
      safePrint("[TaskReadSensor] Canh bao: Khong doc duoc DS18B20, giu gia tri mac dinh 25.0 C.");
      temp = 25.0f;
    }

    // Đóng gói dữ liệu cảm biến
    SensorData_t sensorData;
    sensorData.rawGas      = rawGas;
    sensorData.gasPercent  = gasPct;
    sensorData.gasPPM      = gasPpm;
    sensorData.temperature = temp;
    sensorData.timestamp   = millis();

    // 3. Hiển thị dữ liệu qua giao tiếp Serial (bảo vệ bởi Mutex)
    if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      Serial.println("--------------------------------------------------");
      Serial.printf("[CORE %d | TaskReadSensor] Gas ADC: %d | Gas: %.1f%% (%d ppm) | Nhiet do (DS18B20): %.1f C\n",
                    xPortGetCoreID(), sensorData.rawGas, sensorData.gasPercent,
                    sensorData.gasPPM, sensorData.temperature);
      xSemaphoreGive(xSerialMutex);
    }

    // 4. Gửi dữ liệu đầy đủ vào hàng đợi "dataQueue" và "dataAlert"
    if (xQueueSend(dataQueue, &sensorData, pdMS_TO_TICKS(100)) != pdPASS) {
      SensorData_t dummy;
      xQueueReceive(dataQueue, &dummy, 0);
      xQueueSend(dataQueue, &sensorData, 0);
    }

    if (xQueueSend(dataAlert, &sensorData, pdMS_TO_TICKS(100)) != pdPASS) {
      SensorData_t dummy;
      xQueueReceive(dataAlert, &dummy, 0);
      xQueueSend(dataAlert, &sensorData, 0);
    }

    // Chu kỳ đọc cảm biến: 2 giây
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ============================================================
// TASK 2: TASK CẢNH BÁO (CHẠY TRÊN CORE 1)
// Đọc dữ liệu từ dataAlert và điều khiển Quạt (GPIO 18), Máy bơm (GPIO 19), Còi (GPIO 5)
// ============================================================
void TaskAlertControl(void* pvParameters) {
  (void)pvParameters;
  safePrint("[TaskAlertControl] Khoi chay tren Core " + String(xPortGetCoreID()));

  SensorData_t latestData = {0, 0.0f, 0, 25.0f, 0};
  bool hasData = false;

  for (;;) {
    SensorData_t incomingData;
    if (xQueueReceive(dataAlert, &incomingData, pdMS_TO_TICKS(200)) == pdPASS) {
      latestData = incomingData;
      hasData = true;
    }

    if (!hasData) {
      continue;
    }

    bool gasOver  = (latestData.gasPercent >= GAS_THRESHOLD_PCT);
    bool tempOver = (latestData.temperature >= TEMP_THRESHOLD_C);

    bool targetFan  = false;
    bool targetPump = false;
    bool isManual   = false;

    if (xSemaphoreTake(xControlMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      isManual = manualMode;
      if (isManual) {
        // Chế độ Manual: Điều khiển Quạt/Máy bơm theo nút nhấn từ Web
        targetFan  = manualFanState;
        targetPump = manualPumpState;
      } else {
        // Chế độ Auto: Quạt bật khi Gas vượt ngưỡng, Máy bơm bật khi Nhiệt độ vượt ngưỡng
        targetFan  = gasOver;
        targetPump = tempOver;
      }

      currentFanState  = targetFan;
      currentPumpState = targetPump;
      currentGasAlert  = gasOver;
      currentTempAlert = tempOver;
      xSemaphoreGive(xControlMutex);
    }

    // Điều khiển LED đại diện Quạt (GPIO 18) & Máy bơm (GPIO 19)
    digitalWrite(FAN_PIN, targetFan ? HIGH : LOW);
    digitalWrite(PUMP_PIN, targetPump ? HIGH : LOW);

    // Kích hoạt còi cảnh báo (GPIO 5) khi khí gas hoặc nhiệt độ vượt ngưỡng
    if (gasOver || tempOver) {
      tone(BUZZER_PIN, 1500);
    } else {
      noTone(BUZZER_PIN);
    }

    static bool prevFan = false, prevPump = false, prevMode = false;
    static uint32_t lastLogTs = 0;
    if (latestData.timestamp != lastLogTs || targetFan != prevFan || targetPump != prevPump || isManual != prevMode) {
      lastLogTs = latestData.timestamp;
      prevFan   = targetFan;
      prevPump  = targetPump;
      prevMode  = isManual;

      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.printf("[CORE %d | TaskAlertControl] Mode: %s | Quat (LED Xanh GPIO18): %s | May bom (LED Do GPIO19): %s | Canh bao: %s\n",
                      xPortGetCoreID(),
                      isManual ? "MANUAL" : "AUTO",
                      targetFan ? "BAT" : "TAT",
                      targetPump ? "BAT" : "TAT",
                      (gasOver || tempOver) ? "NGUY HIEM!" : "An toan");
        xSemaphoreGive(xSerialMutex);
      }
    }
  }
}

// ============================================================
// HÀM KẾT NỐI WI-FI & MQTT TRONG TASK TRUYỀN DATA (CORE 0)
// ============================================================
void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  safePrint("[TaskTransmitData] Dang ket noi Wi-Fi: " + String(WIFI_SSID) + "...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() == WL_CONNECTED) {
    safePrint("[TaskTransmitData] Da ket noi Wi-Fi! IP: " + WiFi.localIP().toString());
  } else {
    safePrint("[TaskTransmitData] Ket noi Wi-Fi that bai, se thu lai...");
  }
}

void ensureMQTTConnected() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  String clientId = "ESP32_FireAlarm_" + String((uint32_t)ESP.getEfuseMac(), HEX);
  safePrint("[TaskTransmitData] Dang ket noi MQTT Broker: " + String(MQTT_BROKER) + "...");

  if (mqttClient.connect(clientId.c_str())) {
    safePrint("[TaskTransmitData] Da ket noi MQTT Broker thanh cong!");
    mqttClient.subscribe(TOPIC_CONTROL);
    safePrint("[TaskTransmitData] Da dang ky lang nghe topic: " + String(TOPIC_CONTROL));
  } else {
    safePrint("[TaskTransmitData] Ket noi MQTT that bai, rc=" + String(mqttClient.state()));
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ============================================================
// TASK 3: TASK TRUYỀN DATA (CHẠY TRÊN CORE 0)
// Kết nối Wi-Fi & MQTT, đọc dữ liệu từ dataQueue và gửi lên Broker
// ============================================================
void TaskTransmitData(void* pvParameters) {
  (void)pvParameters;
  safePrint("[TaskTransmitData] Khoi chay tren Core " + String(xPortGetCoreID()));

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  SensorData_t txData;

  for (;;) {
    ensureWiFiConnected();
    if (WiFi.status() == WL_CONNECTED) {
      ensureMQTTConnected();
      mqttClient.loop();
    }

    if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
      if (xQueueReceive(dataQueue, &txData, pdMS_TO_TICKS(100)) == pdPASS) {
        bool fanState, pumpState, isManual, gasAlert, tempAlert;

        if (xSemaphoreTake(xControlMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          fanState  = currentFanState;
          pumpState = currentPumpState;
          isManual  = manualMode;
          gasAlert  = currentGasAlert;
          tempAlert = currentTempAlert;
          xSemaphoreGive(xControlMutex);
        }

        StaticJsonDocument<320> doc;
        doc["gas"]        = round(txData.gasPercent * 10.0) / 10.0;
        doc["gas_ppm"]    = txData.gasPPM;
        doc["raw_gas"]    = txData.rawGas;
        doc["temp"]       = round(txData.temperature * 10.0) / 10.0;
        doc["fan"]        = fanState;
        doc["pump"]       = pumpState;
        doc["mode"]       = isManual ? "manual" : "auto";
        doc["gas_alert"]  = gasAlert;
        doc["temp_alert"] = tempAlert;

        char jsonBuffer[320];
        serializeJson(doc, jsonBuffer);

        if (mqttClient.publish(TOPIC_DATA, jsonBuffer)) {
          safePrint("[CORE " + String(xPortGetCoreID()) + " | TaskTransmitData] MQTT TX -> " + String(jsonBuffer));
        } else {
          safePrint("[TaskTransmitData] Loi gui du lieu len MQTT Broker!");
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================
// HÀM SETUP: KHỞI TẠO PHẦN CỨNG, QUEUE, SEMAPHORE VÀ TASK
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HE THONG BAO CHAY GIA DINH ESP32 + FREERTOS + MQTT ===");

  pinMode(MQ2_PIN, INPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(FAN_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW);
  noTone(BUZZER_PIN);

  // Khởi tạo cảm biến nhiệt độ DS18B20
  tempSensor.begin();

  // Tạo Mutex Semaphore
  xSerialMutex  = xSemaphoreCreateMutex();
  xControlMutex = xSemaphoreCreateMutex();

  // Tạo 2 Hàng đợi (Queue) chứa tối đa 10 gói SensorData_t
  dataQueue = xQueueCreate(10, sizeof(SensorData_t));
  dataAlert = xQueueCreate(10, sizeof(SensorData_t));

  if (dataQueue == NULL || dataAlert == NULL || xSerialMutex == NULL || xControlMutex == NULL) {
    Serial.println("[FATAL] Khong the tao Queue hoac Semaphore!");
    while (true) {
      delay(1000);
    }
  }

  // Khởi tạo 3 Task theo đúng phân bổ Core của Đề 3
  xTaskCreatePinnedToCore(
    TaskTransmitData,   // Hàm thực thi
    "Task truyền data", // Tên Task
    8192,               // Stack cho WiFi/MQTT/JSON
    NULL,
    2,
    NULL,
    0                   // Ghim vào Core 0
  );

  xTaskCreatePinnedToCore(
    TaskReadSensor,     // Hàm thực thi
    "Task đọc data",    // Tên Task
    4096,
    NULL,
    2,
    NULL,
    1                   // Ghim vào Core 1
  );

  xTaskCreatePinnedToCore(
    TaskAlertControl,   // Hàm thực thi
    "Task cảnh báo",    // Tên Task
    4096,
    NULL,
    3,                  // Độ ưu tiên cao nhất để phản ứng cảnh báo tức thời
    NULL,
    1                   // Ghim vào Core 1
  );
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}

