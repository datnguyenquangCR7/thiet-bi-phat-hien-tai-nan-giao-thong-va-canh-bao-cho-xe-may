#include <Arduino.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);

bool isGpsConnected = false;
bool isSignalCaught = false;
unsigned long lastPrintTime = 0; 
unsigned long lastWarningTime = 0; // Biến dùng cho cảnh báo không dùng delay

void setup() {
  Serial.begin(115200);

  // Khởi tạo UART2 với RX=16, TX=17 cho ESP32
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

  Serial.println("=====================================");
  Serial.println("Hệ thống ESP32 & GPS NEO-7M khởi động");
  Serial.println("Đang chờ phản hồi từ module GPS...");
  Serial.println("=====================================");
}

void loop() {
  // Liên tục đọc dữ liệu không ngừng nghỉ
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // 1. Kiểm tra kết nối vật lý (Dữ liệu từ module đã vào ESP32 chưa)
  if (gps.charsProcessed() > 0 && !isGpsConnected) {
    Serial.println("✅ Đã kết nối thành công với module GPS NEO-7M!");
    isGpsConnected = true;
  }

  // 2. Kiểm tra tín hiệu vệ tinh (Đã có tọa độ hợp lệ chưa)
  if (gps.location.isValid()) {
    if (!isSignalCaught) {
        Serial.println("🛰️ GPS ĐÃ BẮT ĐƯỢC TÍN HIỆU VỆ TINH!");
        isSignalCaught = true;
    }
    
    // In toạ độ và link Google Maps mỗi 2 giây
    if (millis() - lastPrintTime > 2000) {
        float lat = gps.location.lat();
        float lng = gps.location.lng();

        Serial.print("Vĩ độ: "); Serial.print(lat, 6);
        Serial.print(" | Kinh độ: "); Serial.println(lng, 6);
        
        // --- CHÈN LINK GOOGLE MAPS CHUẨN ---
        Serial.print("📍 Link bản đồ: ");
        Serial.print("https://www.google.com/maps?q=");
        Serial.print(lat, 6);
        Serial.print(",");
        Serial.println(lng, 6); 
        Serial.println("-------------------------------------");
        // -------------------------------------

        lastPrintTime = millis();
    }
  } else {
     if (isSignalCaught) {
        Serial.println("⚠️ Mất tín hiệu vệ tinh, đang tìm kiếm lại...");
        isSignalCaught = false;
     }
  }

  // 3. Cảnh báo lỗi kết nối vật lý (Dùng millis thay cho delay)
  if (millis() > 5000 && gps.charsProcessed() < 10) {
    if (millis() - lastWarningTime > 2000) {
      Serial.println("❌ Lỗi: Không nhận được dữ liệu từ GPS. Hãy kiểm tra lại dây TX/RX!");
      lastWarningTime = millis(); // Cập nhật lại thời gian cảnh báo
    }
  }
}