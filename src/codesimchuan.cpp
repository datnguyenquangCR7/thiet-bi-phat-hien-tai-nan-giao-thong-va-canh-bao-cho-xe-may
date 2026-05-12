#include <Arduino.h>

#define SIM_RX 16 // Cắm dây TX của mạch SIM vào chân 16 ESP32
#define SIM_TX 17 // Cắm dây RX của mạch SIM vào chân 17 ESP32

HardwareSerial simSerial(2);

// SỐ ĐIỆN THOẠI NHẬN TIN NHẮN & CUỘC GỌI
String phoneNumber = "0869381546"; 

// Hàm gửi lệnh nâng cao: Có kiểm tra xem phản hồi có đúng chữ mình cần không
bool sendATCommand(String command, String expected_response, int timeout) {
  simSerial.println(command);
  long int time = millis();
  String response = "";
  
  while ((time + timeout) > millis()) {
    while (simSerial.available()) {
      char c = simSerial.read();
      response += c;
      Serial.print(c); // In ra màn hình để theo dõi
    }
    // Nếu tìm thấy chữ mong muốn (vd: "OK") trong câu trả lời
    if (response.indexOf(expected_response) != -1) {
      return true; 
    }
  }
  return false; // Hết giờ mà không nhận được
}

void setup() {
  Serial.begin(115200);
  simSerial.begin(9600, SERIAL_8N1, SIM_RX, SIM_TX);
  
  Serial.println("\n====================================");
  Serial.println("   KHOI DONG HE THONG THONG MINH");
  Serial.println("====================================");
  Serial.println("Doi 15 giay de SIM vao mang 4G/GSM...");
  delay(15000); 

  // 1. Đánh thức module SIM
  Serial.println("\n>> Dong bo giao tiep voi SIM...");
  for(int i = 0; i < 3; i++) {
    simSerial.println("AT");
    delay(500);
  }

  // 2. Kiểm tra sống chết
  Serial.println("\n>> Kiem tra ket noi AT:");
  if (!sendATCommand("AT", "OK", 2000)) {
    Serial.println("\n-> [LOI] SIM khong tra loi! Kiem tra lai day TX/RX, GND hoac Nguon cham RST.");
    return; // Dừng chương trình ngay lập tức
  }

  // ================= PHẦN 1: NHẮN TIN SMS =================

  // 3. Cài đặt chế độ Text SMS
  Serial.println("\n>> Chuyen che do Text SMS (AT+CMGF=1):");
  if (!sendATCommand("AT+CMGF=1", "OK", 2000)) {
    Serial.println("\n-> [LOI] Khong the cai dat che do tin nhan.");
    return;
  }

  // 4. Nhập số điện thoại
  Serial.println("\n>> Thiet lap so dien thoai nhan SMS...");
  simSerial.print("AT+CMGS=\"");
  simSerial.print(phoneNumber);
  simSerial.println("\"");
  
  long int time = millis();
  bool readyToSend = false;
  String prompt = "";
  
  // Đợi tối đa 5 giây cho dấu >
  while ((time + 5000) > millis()) { 
    while (simSerial.available()) {
      char c = simSerial.read();
      prompt += c;
      Serial.print(c);
    }
    if (prompt.indexOf(">") != -1) {
      readyToSend = true;
      break;
    }
  }

  // 5. Gửi nội dung tin nhắn
  if (readyToSend) {
    Serial.println("\n\n>> SIM da mo cua. Dang gui tin nhan...");
    
    // NỘI DUNG TIN NHẮN TẠI ĐÂY
    simSerial.print("hello world"); 
    
    delay(100);
    simSerial.write(26); // Mã ASCII 26 (Ctrl+Z) để chốt và gửi đi
    
    Serial.println("\n>> Dang cho mang vien thong xac nhan SMS (co the mat vai giay)...");
    
    // Tối ưu hóa: Thoát vòng lặp sớm nếu nhận được "OK" (gửi thành công) hoặc "ERROR"
    long int sendTime = millis();
    String smsResponse = "";
    while ((sendTime + 15000) > millis()) { 
      while (simSerial.available()) {
        char c = simSerial.read();
        smsResponse += c;
        Serial.print(c);
      }
      if (smsResponse.indexOf("OK") != -1 || smsResponse.indexOf("ERROR") != -1) {
        break; // Thoát ngay khi có kết quả
      }
    }
    Serial.println("\n=== HOAN THANH NHAN TIN ===");
  } else {
    Serial.println("\n\n-> [LOI] Khong nhan duoc dau nhac '>'. Module SIM bi loi giua chung.");
  }

  delay(3000); // Nghỉ 3 giây trước khi chuyển sang gọi điện

  // ================= PHẦN 2: GỌI ĐIỆN THOẠI =================
  
  Serial.println("\n====================================");
  Serial.println(">> CHUAN BI THUC HIEN CUOC GOI...");
  
  String callCommand = "ATD" + phoneNumber + ";"; // Lưu ý: PHẢI có dấu chấm phẩy ';' ở cuối để gọi thoại
  simSerial.println(callCommand);
  
  Serial.print(">> Dang goi den so: ");
  Serial.println(phoneNumber);
  Serial.println(">> Chuong se reo trong 15 giay...");
  
  // Để chuông reo trong 15 giây (bạn có thể tăng giảm thời gian này)
  delay(15000); 

  // Tắt máy (Cúp máy)
  Serial.println("\n>> Ket thuc cuoc goi (ATH).");
  simSerial.println("ATH");
  
  Serial.println("\n=== HOAN THANH TOAN BO QUY TRINH ===");
}

void loop() {
  // Cho phép bạn gõ lệnh AT trực tiếp từ Serial Monitor để debug
  while (simSerial.available()) {
    Serial.write(simSerial.read());
  }
  while (Serial.available()) {
    simSerial.write(Serial.read());
  }
}
