#include <M5Unified.h>
#include <TinyGPS++.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>

// --- 設定 ---
static const uint32_t GPSBaud = 115200; 
const char* WIFI_SSID = "M5GPS_Logger_Pro"; 
const char* CSV_HEADER = "Date,Time,Latitude,Longitude,Altitude(m),Speed(km/h),Course(deg),HDOP";

// --- SDカード SPIピン設定 (Atom S3 + Atomic Mate) ---
#define S3_SPI_SCK  7
#define S3_SPI_MISO 8
#define S3_SPI_MOSI 6
#define S3_SPI_CS   5

// --- 状態管理 ---
enum DeviceMode { MODE_VIEW, MODE_MENU, MODE_WIFI };
DeviceMode currentMode = MODE_VIEW;

// メニュー項目
enum MenuOption { MENU_TOGGLE_REC, MENU_NEW_FILE, MENU_WIFI, MENU_EXIT };
int currentMenuIndex = 0;
const int MENU_ITEMS_COUNT = 4;
const char* MENU_LABELS[] = { "Start/Stop Log", "New Log File", "Wi-Fi Server", "Back to View" };

bool isLogging = false;       
String currentLogFileName = ""; 
bool handledLongPress = false; // ボタン長押し制御用フラグ

// --- オブジェクト ---
HardwareSerial gpsSerial(2); 
TinyGPSPlus gps;
WebServer server(80);

// --- プロトタイプ ---
void initSD();
String createNewLogFile();
void logData();
void drawViewScreen();
void drawMenuScreen();
void drawWifiScreen();
void handleWifiLoop();
float getSmartSpeed();
float getSmartAltitude();
float getSmartCourse();

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  // 画面・ピン設定
  if (M5.getBoard() == m5::board_t::board_M5AtomS3) {
    M5.Display.setBrightness(20); 
    M5.Display.setRotation(0); 
    M5.Display.setTextFont(0); 
    M5.Display.setTextSize(1);
    gpsSerial.begin(GPSBaud, SERIAL_8N1, 1, 2);
  } else {
    gpsSerial.begin(GPSBaud, SERIAL_8N1, 32, 26);
  }

  // SD初期化
  initSD();
  
  // 初期状態
  isLogging = false;
  currentLogFileName = ""; 
  
  Serial.println("System Ready.");
}

void loop() {
  M5.update();

  // GPS受信
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }

  // --- モード別処理 ---
  switch (currentMode) {
    
    // ■ 1. 通常表示モード (情報表示メイン)
    case MODE_VIEW:
      // ログ保存
      if (isLogging && currentLogFileName != "") {
        static int lastLoggedSecond = -1;
        // 3秒以内のデータなら有効とみなす
        if (gps.location.isValid() && gps.location.age() < 3000) {
          int currentSecond = gps.time.second();
          if (currentSecond % 10 == 0 && currentSecond != lastLoggedSecond) {
            logData();
            lastLoggedSecond = currentSecond;
          }
        }
      }

      // 画面更新 (0.5秒ごと: 時計の秒などをスムーズに)
      static unsigned long lastViewUpdate = 0;
      if (millis() - lastViewUpdate > 500) {
        drawViewScreen();
        lastViewUpdate = millis();
      }

      // 長押し(2秒)でメニューへ
      if (M5.BtnA.pressedFor(2000) && !handledLongPress) {
        handledLongPress = true; // 長押し処理済みとする
        currentMode = MODE_MENU;
        currentMenuIndex = 0; 
        drawMenuScreen();
      }
      
      // ボタンを離した時のリセット処理
      if (M5.BtnA.wasReleased()) {
        handledLongPress = false;
      }
      break;


    // ■ 2. メニューモード (操作性改善)
    case MODE_MENU:
      // ★改善点: ボタンを「離した瞬間」に遷移 (長押し済みでない場合のみ)
      if (M5.BtnA.wasReleased()) {
        if (!handledLongPress) {
          currentMenuIndex = (currentMenuIndex + 1) % MENU_ITEMS_COUNT;
          drawMenuScreen();
        }
        handledLongPress = false; // フラグリセット
      }

      // 長押し(2秒): 決定
      if (M5.BtnA.pressedFor(2000) && !handledLongPress) {
        handledLongPress = true; // 長押し検知
        
        // 実行フィードバック
        M5.Display.fillScreen(TFT_BLUE);
        delay(100);

        switch (currentMenuIndex) {
          case MENU_TOGGLE_REC: 
            if (isLogging) {
              isLogging = false; 
            } else {
              if (currentLogFileName == "") currentLogFileName = createNewLogFile();
              isLogging = true; 
            }
            currentMode = MODE_VIEW;
            break;

          case MENU_NEW_FILE: 
            currentLogFileName = createNewLogFile();
            isLogging = true;
            currentMode = MODE_VIEW;
            break;

          case MENU_WIFI: 
            isLogging = false; 
            currentMode = MODE_WIFI;
            // Wi-Fi起動
            M5.Display.fillScreen(TFT_BLACK);
            M5.Display.setCursor(0,0);
            M5.Display.setTextColor(TFT_WHITE);
            M5.Display.println("Starting WiFi...");
            WiFi.softAP(WIFI_SSID);
            server.begin();
            
            // ファイル一覧
            server.on("/", HTTP_GET, []() {
              String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Log List</title></head><body>";
              html += "<h2>SD Card Files</h2><ul>";
              File root = SD.open("/");
              if (root) {
                File file = root.openNextFile();
                while (file) {
                  String fileName = String(file.name());
                  if (fileName.endsWith(".csv") || fileName.endsWith(".CSV")) {
                    html += "<li><a href='/download?file=" + fileName + "'>" + fileName + "</a> (" + String(file.size()) + " B)</li>";
                  }
                  file = root.openNextFile();
                }
                root.close();
              }
              html += "</ul></body></html>";
              server.send(200, "text/html", html);
            });

            // ダウンロード
            server.on("/download", HTTP_GET, []() {
              String fileToDownload = server.arg("file");
              if (!fileToDownload.startsWith("/")) fileToDownload = "/" + fileToDownload;
              if (SD.exists(fileToDownload)) {
                File file = SD.open(fileToDownload, FILE_READ);
                server.streamFile(file, "text/csv");
                file.close();
              } else {
                server.send(404, "text/plain", "File Not Found");
              }
            });
            break;

          case MENU_EXIT: 
            currentMode = MODE_VIEW;
            break;
        }
        
        // 画面リフレッシュ
        if (currentMode == MODE_VIEW) drawViewScreen();
        if (currentMode == MODE_WIFI) drawWifiScreen();
      }
      break;


    // ■ 3. Wi-Fiモード
    case MODE_WIFI:
      server.handleClient();
      
      static unsigned long lastWifiUpdate = 0;
      if (millis() - lastWifiUpdate > 1000) {
        drawWifiScreen();
        lastWifiUpdate = millis();
      }

      if (M5.BtnA.wasPressed()) {
        WiFi.softAPdisconnect(true);
        currentMode = MODE_MENU;
        drawMenuScreen();
      }
      break;
  }
}


// --- 関数群 ---

void initSD() {
  if (M5.getBoard() == m5::board_t::board_M5AtomS3) {
    SPI.begin(S3_SPI_SCK, S3_SPI_MISO, S3_SPI_MOSI, S3_SPI_CS);
    if (!SD.begin(S3_SPI_CS, SPI, 15000000)) {
      M5.Display.fillScreen(TFT_RED);
      M5.Display.println("SD Error!");
      while(1);
    }
  } else {
    if (!SD.begin(4, SPI, 15000000)) {
       Serial.println("SD Error");
    }
  }
}

String createNewLogFile() {
  String fileName = "";
  if (gps.date.isValid() && gps.time.isValid()) {
    char timeBuffer[32];
    snprintf(timeBuffer, sizeof(timeBuffer), "/%04d%02d%02d_%02d%02d%02d.csv",
      gps.date.year(), gps.date.month(), gps.date.day(),
      gps.time.hour(), gps.time.minute(), gps.time.second());
    fileName = String(timeBuffer);
    if (SD.exists(fileName)) {
       delay(1000); 
       snprintf(timeBuffer, sizeof(timeBuffer), "/%04d%02d%02d_%02d%02d%02d_2.csv",
        gps.date.year(), gps.date.month(), gps.date.day(),
        gps.time.hour(), gps.time.minute(), gps.time.second());
       fileName = String(timeBuffer);
    }
  } else {
    int fileIndex = 1;
    while (true) {
      fileName = "/gps_seq_" + String(fileIndex) + ".csv";
      if (!SD.exists(fileName)) break; 
      fileIndex++;
    }
  }

  File file = SD.open(fileName, FILE_WRITE);
  if (file) {
    file.println(CSV_HEADER);
    file.close();
  }
  return fileName;
}

void logData() {
  File file = SD.open(currentLogFileName, FILE_APPEND);
  if (!file) return;

  char buffer[200]; 
  snprintf(buffer, sizeof(buffer), 
    "%04d-%02d-%02d,%02d:%02d:%02d,%.6f,%.6f,%.1f,%.1f,%.1f,%.2f\n",
    gps.date.year(), gps.date.month(), gps.date.day(),
    gps.time.hour(), gps.time.minute(), gps.time.second(),
    gps.location.lat(), gps.location.lng(),
    getSmartAltitude(), getSmartSpeed(), getSmartCourse(),
    gps.hdop.hdop() 
  );
  file.print(buffer);
  file.close();
}

// ★改良版: 通常画面 (情報豊富バージョン)
void drawViewScreen() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLACK);
  
  // --- 上部ステータスバー (REC状態 & 時刻) ---
  if (isLogging) {
    M5.Display.fillRect(0, 0, 128, 20, TFT_RED); // 赤背景
    M5.Display.setTextColor(TFT_WHITE, TFT_RED);
    M5.Display.setCursor(2, 2);
    M5.Display.print("REC ");
  } else {
    M5.Display.fillRect(0, 0, 128, 20, TFT_DARKGREY); // グレー背景
    M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    M5.Display.setCursor(2, 2);
    M5.Display.print("STOP ");
  }

  // 時刻表示 (HH:MM:SS)
  if (gps.time.isValid()) {
    M5.Display.printf("%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
  } else {
    M5.Display.print("--:--:--");
  }
  
  // --- メイン情報 ---
  M5.Display.setCursor(0, 25);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(1);
  
  // 緯度経度
  if (gps.location.isValid()) {
    M5.Display.printf("Lat:%.4f\nLng:%.4f\n", gps.location.lat(), gps.location.lng());
  } else {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.print("Search GPS...\n\n");
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  // 高度 & 方位 (★追加)
  M5.Display.printf("Alt:%.0fm  Crs:%.0f\n", getSmartAltitude(), getSmartCourse());
  
  // --- 下部グラフィカルエリア ---
  
  // 速度 (大文字)
  M5.Display.setCursor(0, 95);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  if (gps.speed.isValid()) {
     M5.Display.printf("%.0f", getSmartSpeed());
  } else {
     M5.Display.print("-");
  }
  M5.Display.setTextSize(1);
  M5.Display.setCursor(55, 110);
  M5.Display.print("km/h");

  // アンテナバー & 衛星数
  int sats = gps.satellites.value();
  int hdop = gps.hdop.value();
  
  // 衛星数表示
  M5.Display.setCursor(90, 95);
  M5.Display.printf("Sat:%d", sats);

  // バーの色計算
  uint16_t barColor = TFT_DARKGREY;
  int level = 0;
  if (gps.location.age() < 3000) { // データが古くない場合
    if(sats>15 && hdop<=100) level=3;
    else if(sats>5 && hdop<=100) level=2;
    else if(sats>2 && hdop<=500) level=1;
  }
  
  // バー描画 (右下)
  M5.Display.fillRect(108,123,4,4, (level>=0)?(level==0?TFT_RED:TFT_ORANGE):barColor);
  M5.Display.fillRect(113,119,4,8, (level>=1)?(level==1?TFT_ORANGE:TFT_YELLOW):barColor);
  M5.Display.fillRect(118,115,4,12,(level>=2)?(level==2?TFT_YELLOW:TFT_GREEN):barColor);
  M5.Display.fillRect(123,111,4,16,(level>=3)?TFT_GREEN:barColor);

  M5.Display.endWrite();
}

void drawMenuScreen() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_DARKGREY); 
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.println("=== MENU ===");
  M5.Display.println("");

  for (int i = 0; i < MENU_ITEMS_COUNT; i++) {
    if (i == currentMenuIndex) {
      M5.Display.setTextColor(TFT_BLACK, TFT_WHITE); 
      M5.Display.print(" > ");
    } else {
      M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
      M5.Display.print("   ");
    }
    M5.Display.println(MENU_LABELS[i]);
  }
  
  M5.Display.setTextColor(TFT_YELLOW, TFT_DARKGREY);
  M5.Display.println("");
  M5.Display.print("Status: ");
  M5.Display.println(isLogging ? "REC" : "STOP");
  M5.Display.println("");
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
  M5.Display.println("Short: Next");
  M5.Display.println("Long : Enter");

  M5.Display.endWrite();
}

void drawWifiScreen() {
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_BLUE);
  M5.Display.setCursor(0, 0);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLUE);
  M5.Display.println("Wi-Fi SERVER ON");
  M5.Display.println("");
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.printf("SSID: %s\n", WIFI_SSID);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.println("IP: 192.168.4.1");
  M5.Display.println("");
  M5.Display.println("Connect & Open");
  M5.Display.println("Browser to IP");
  M5.Display.println("");
  M5.Display.setTextColor(TFT_RED, TFT_WHITE);
  M5.Display.println("[Press Btn to Exit]");
  M5.Display.endWrite();
}

float getSmartSpeed() {
  if (gps.speed.age() > 3000) return 0.0;
  return gps.speed.kmph();
}
float getSmartAltitude() {
  if (gps.altitude.age() > 3000) return 0.0;
  return gps.altitude.meters();
}
float getSmartCourse() {
  if (gps.course.age() > 3000) return 0.0;
  return gps.course.deg();
}
void handleWifiLoop() {
  server.handleClient();
}
