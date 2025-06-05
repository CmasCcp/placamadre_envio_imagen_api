/*
 * @file    Unified_SIM7600_SD_Image_Manager.ino
 * @brief   ESP32 sketch for:
 *            - Receiving images via Serial2 and saving to SD
 *            - Sending images via SIM7600 HTTP POST (webhook)
 *            - Querying and displaying network info (fallback AT)
 *            - Updating RTC from server
 *            - Handling asynchronous modem events with TinyGSM debug
 * @author  Alejandro Rebolledo
 * @date    2025-05-18
 */


#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 2014  // Set RX buffer to 2Kb
#define SerialAT Serial1

#include <Arduino.h>
#include <StreamDebugger.h>
#include <TinyGsmClient.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>

// --- Serial port pins -----------------------------------------------------
#define MODEM_RX_PIN 17
#define MODEM_TX_PIN 16
#define FILE_RX_PIN 26
#define FILE_TX_PIN 27
// Configuración de tiempos
#define uS_TO_S_FACTOR 1000000ULL  // Factor de conversión de microsegundos a segundos
#define TIME_TO_SLEEP 30           // Tiempo de sleep en segundos
#define UART_BAUD 115200

// --- HardwareSerial instances --------------------------------------------

StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);

HardwareSerial fileSerial(2);  // Serial2 for image transfer

// --- Display & RTC --------------------------------------------------------
U8G2_SH1107_SEEED_128X128_F_HW_I2C display(U8G2_R0);
RTC_DS3231 rtc;

// --- GPRS & Webhook config -----------------------------------------------
const char *APN = "gigsky-02";
const char *GPRS_USER = "";
const char *GPRS_PASS = "";

const char *WEBHOOK_URL = "https://webhook.site/63829e7d-b9b6-487b-be1f-0dec5447b33a";
// const char *API_URL = "https://api-sensores.cmasccp.cl/agregarImagen";
const char *API_URL = "http://127.0.0.1:8084/agregarImagen";
const char *PHOTO_PATH = "/mariposa.JPG";

// --- Transfer settings ---------------------------------------------------
#define CHUNK_SIZE 1024
#define SD_CS_PIN 5
#define UART_BAUD 115200
const unsigned long networkInterval = 45000;  // 45 s
const unsigned long sendInterval = 600000;    // 10 min

// --- State variables ------------------------------------------------------
bool receiving = false;
File outFile;
String filename;
int fileSize = 0;
int bytesReceived = 0;

String networkOperator;
String networkTech;
String signalQuality;
String registrationStatus;

String httpReadData;
String lastPostedID;

unsigned long lastNetworkUpdate = 0;
unsigned long lastDataSend = 0;

// --- Forward declarations -----------------------------------------------
void setupHardware();
void loopNormalTasks();
void checkForFileHeader();
void processFileReception();
void handleSerialCommands();
void sendImageWebhook();
void sendImageApi();
void readModemResponses();
void executeATCommand(const String &cmd, unsigned long timeout);
void displayModemResponse(const String &cmd, const String &resp);
void testSIM();
bool connectToNetwork();
void getNetworkInfoFallback(String &op, String &tech, String &csq, String &reg);
void updateNetworkInfo();
void closeHttpSession();
void rtcUpdate();

// --- Setup ---------------------------------------------------------------
//void setup() {
//  Serial.begin(UART_BAUD);
//  setupHardware();
//  testSIM();
//  if (!connectToNetwork()) {
//    Serial.println("Network connect failed");
//  }
//  updateNetworkInfo();
//  lastNetworkUpdate = millis();
//  lastDataSend = millis();
//}
//
// --- Setup ---------------------------------------------------------------
//void setup() {
//  pinMode(4, OUTPUT);
//  pinMode(13, OUTPUT);
//  digitalWrite(4, HIGH);
//  digitalWrite(13, HIGH);
//
//  delay(100);
//
//  Serial.begin(UART_BAUD);
//  setupHardware();
//  testSIM();
//  if (!connectToNetwork()) {
//    Serial.println("Network connect failed");
//  }
//  updateNetworkInfo();
//  lastNetworkUpdate = millis();
//  lastDataSend = millis();
//}

// --- Setup ---------------------------------------------------------------
void setup() {
  pinMode(4, OUTPUT);
  pinMode(13, OUTPUT);
  digitalWrite(4, HIGH);
  digitalWrite(13, HIGH);

  delay(100);

  Serial.begin(UART_BAUD);
  setupHardware();
  testSIM();
  if (!connectToNetwork()) {
    Serial.println("Network connect failed");
  }
  updateNetworkInfo();
  lastNetworkUpdate = millis();
  lastDataSend = millis();
}

// --- Main loop -----------------------------------------------------------
void loop() {
  if (receiving) {
    processFileReception();
    return;
  }
  checkForFileHeader();
  readModemResponses();
  handleSerialCommands();
  loopNormalTasks();
}

// --- Hardware initialization --------------------------------------------
void setupHardware() {
  // USB-Serial and AT-Serial
  SerialAT.begin(UART_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  modem.restart();
  modem.init();
  // File-transfer serial
  fileSerial.begin(UART_BAUD, SERIAL_8N1, FILE_RX_PIN, FILE_TX_PIN);

  // OLED
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  display.drawStr(0, 24, "INIT");
  display.sendBuffer();

  // RTC
  if (!rtc.begin()) {
    Serial.println("RTC init failed");
    display.clearBuffer();
    display.drawStr(0, 24, "RTC FAIL");
    display.sendBuffer();
  }

  // SD card
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD init failed");
    display.clearBuffer();
    display.drawStr(0, 24, "SD FAIL");
    display.sendBuffer();
  }
}

// --- Detect incoming "filename|size" header -----------------------------
void checkForFileHeader() {
  if (!receiving && fileSerial.available()) {
    String header = fileSerial.readStringUntil('\n');
    header.trim();
    int sep = header.indexOf('|');
    if (sep > 0) {
      filename = header.substring(0, sep);
      fileSize = header.substring(sep + 1).toInt();
      bytesReceived = 0;
      outFile = SD.open("/" + filename, FILE_WRITE);
      if (outFile) {
        receiving = true;
        display.clearBuffer();
        display.drawStr(0, 24, "RECEIVING");
        display.sendBuffer();
        Serial.printf("Receiving '%s' (%d bytes)\n",
                      filename.c_str(), fileSize);
      } else {
        Serial.println("SD open failed");
      }
    }
  }
}

// --- Receive and save file chunks ---------------------------------------
void processFileReception() {
  while (fileSerial.available() && bytesReceived < fileSize) {
    uint8_t buf[CHUNK_SIZE];
    int toRead = min((int)sizeof(buf), fileSize - bytesReceived);
    int n = fileSerial.readBytes(buf, toRead);
    outFile.write(buf, n);
    bytesReceived += n;
    if ( bytesReceived % 1024 == 0) Serial.print(".");//mostrar en serial los modulos de datos
  }
  if (bytesReceived >= fileSize) {
    outFile.close();
    Serial.println("File saved");
    fileSerial.println("ACK");
    receiving = false;
    display.clearBuffer();
    display.drawStr(0, 24, "SAVED");
    display.sendBuffer();
  }
}

// --- Handle commands from USB serial ------------------------------------
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.equalsIgnoreCase("s")) {
    sendImageWebhook();
  } else if (cmd.equalsIgnoreCase("p")) {
    fileSerial.println("foto");
    Serial.println("Requested photo");
  } else if (cmd.equalsIgnoreCase("t")) {
    rtcUpdate();
  } else if (cmd.equalsIgnoreCase("sa")) {
    sendImageApi();
  }
}

// ------------- Send image via SIM7600 HTTP POST -----------------------------------
void sendImageWebhook() {
  File img = SD.open(PHOTO_PATH, FILE_READ);
  if (!img) {
    Serial.println("Image open failed");
    return;
  }
  size_t imgSize = img.size();
  Serial.printf("Image size: %u bytes\n", imgSize);

  // Close previous session
  modem.sendAT("+HTTPTERM");
  modem.waitResponse(2000);

  // Init HTTP
  modem.sendAT("+HTTPINIT");
  if (modem.waitResponse(5000) != 1) {
    Serial.println("HTTPINIT failed");
    img.close();
    return;
  }

  // Set parameters
  modem.sendAT("+HTTPPARA=\"CID\",1");
  modem.waitResponse(2000);
  modem.sendAT(String("+HTTPPARA=\"URL\",\"") + WEBHOOK_URL + "\"");
  modem.waitResponse(2000);
  modem.sendAT("+HTTPPARA=\"CONTENT\",\"image/jpeg\"");
  modem.waitResponse(2000);

  // Specify data length
  modem.sendAT(String("+HTTPDATA=") + imgSize + ",10000");
  if (modem.waitResponse(8000, ">") != 1 && modem.waitResponse(8000, "DOWNLOAD") != 1) {
    Serial.println("! No prompt, continuing");
  }

  // Stream image
  uint8_t buf[256];
  while (img.available()) {
    size_t n = img.read(buf, sizeof(buf));
    modem.stream.write(buf, n);
    delay(5);
  }
  img.close();
  Serial.println("Image data sent");

  // Wait OK
  modem.waitResponse(10000);

  // POST
  modem.sendAT("+HTTPACTION=1");

  // Response will be handled in readModemResponses()
}


// ------------- Codigo de enviar imagen a API ---------------

// void sendImageApi() {
//   File img = SD.open(PHOTO_PATH, FILE_READ);
//   if (!img) {
//     Serial.println("Image open failed");
//     return;
//   }

//   size_t imgSize = img.size();
//   Serial.printf("Image size to send API: %u bytes\n", imgSize);

//   // Close previous session
//   modem.sendAT("+HTTPTERM");
//   String response;
//   modem.waitResponse(2000, response);
//   Serial.println("Response to HTTPTERM: " + response);

//   // Init HTTP
//   modem.sendAT("+HTTPINIT");
//   modem.waitResponse(5000, response);
//   Serial.println("Response to HTTPINIT: " + response);
//   if (response.indexOf("OK") == -1) {
//     Serial.println("HTTPINIT failed");
//     img.close();
//     return;
//   }

//   // Set parameters
//   modem.sendAT("+HTTPPARA=\"CID\",1");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to CID: " + response);

//   modem.sendAT(String("+HTTPPARA=\"URL\",\"") + API_URL + "\"");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to URL: " + response);

//   modem.sendAT("+HTTPPARA=\"CONTENT\",\"image/jpeg\"");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to Content-Type: " + response);

//   // Specify data length
//   modem.sendAT(String("+HTTPDATA=") + imgSize + ",10000");
//   modem.waitResponse(8000, response);
//   Serial.println("Response to HTTPDATA: " + response);

//   // Stream image
//   uint8_t buf[256];
//   size_t bytesSent = 0;
//   while (img.available()) {
//     size_t n = img.read(buf, sizeof(buf));
//     modem.stream.write(buf, n);
//     bytesSent += n;
//     Serial.printf("Sent %u bytes\n", bytesSent);
//     delay(5);
//   }
//   img.close();
//   Serial.println("Image data sent");

//   // Wait OK
//   modem.waitResponse(10000, response);
//   Serial.println("Response after sending data: " + response);

//   // POST
//   modem.sendAT("+HTTPACTION=1");
//   modem.waitResponse(10000, response);
//   Serial.println("Response to HTTPACTION: " + response);
// }




// -------------  Depurando Codigo de enviar imagen a API ---------------

// void sendImageApi() {
//   File img = SD.open(PHOTO_PATH, FILE_READ);
//   if (!img) {
//     Serial.println("Image open failed");
//     return;
//   }

//   size_t imgSize = img.size();
//   Serial.printf("Image size to send API: %u bytes\n", imgSize);

//   // Cerrar sesión HTTP anterior, si existe
//   modem.sendAT("+HTTPTERM");
//   String response;
//   modem.waitResponse(2000, response);
//   Serial.println("Response to HTTPTERM: " + response);

//   // Iniciar HTTP
//   modem.sendAT("+HTTPINIT");
//   modem.waitResponse(5000, response);
//   Serial.println("Response to HTTPINIT: " + response);
//   if (response.indexOf("OK") == -1) {
//     Serial.println("HTTPINIT failed");
//     img.close();
//     return;
//   }

//   // Configuración de parámetros
//   modem.sendAT("+HTTPPARA=\"CID\",1");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to CID: " + response);

//   modem.sendAT(String("+HTTPPARA=\"URL\",\"") + API_URL + "\"");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to URL: " + response);

//   modem.sendAT("+HTTPPARA=\"CONTENT\",\"image/jpeg\"");  // Tipo de contenido para imagen JPEG
//   modem.waitResponse(2000, response);
//   Serial.println("Response to Content-Type: " + response);

//   // Añadir encabezados manualmente
//   modem.sendAT("POST /upload HTTP/1.1\r\n");  // Cambia la ruta de la API
//   modem.sendAT("Host: http://127.0.0.1:8084/agregarImagen\r\n");  // Reemplaza con el nombre del host real
//   modem.sendAT("Content-Type: image/jpeg\r\n");  // Content-Type de la imagen
//   modem.sendAT("Content-Length: " + String(imgSize) + "\r\n");  // Tamaño del contenido
//   modem.sendAT("Connection: keep-alive\r\n");  // Mantener la conexión abierta
//   modem.sendAT("\r\n");  // Línea en blanco que separa encabezados del cuerpo

//   // Especificar longitud de los datos
//   modem.sendAT("+HTTPDATA=" + String(imgSize) + ",10000");
//   modem.waitResponse(8000, response);
//   Serial.println("Response to HTTPDATA: " + response);

//   // Enviar la imagen en bloques
//   uint8_t buf[256];
//   size_t bytesSent = 0;
//   while (img.available()) {
//     size_t n = img.read(buf, sizeof(buf));
//     modem.stream.write(buf, n);  // Enviar los datos
//     bytesSent += n;
//     Serial.printf("Sent %u bytes\n", bytesSent);
//     delay(5);  // Retardo para evitar saturar el módem
//   }
//   img.close();
//   Serial.println("Image data sent");

//   // Esperar respuesta después de enviar los datos
//   modem.waitResponse(10000, response);
//   Serial.println("Response after sending data: " + response);

//   // Realizar el POST
//   modem.sendAT("+HTTPACTION=1");
//   modem.waitResponse(10000, response);
//   Serial.println("Response to HTTPACTION: " + response);

//   // Terminar la sesión HTTP
//   modem.sendAT("+HTTPTERM");
//   modem.waitResponse(2000, response);
//   Serial.println("Response to HTTPTERM (final): " + response);
// }


// -------------  Enviar POST vacio a API ---------------

void sendImageApi() {
  // No se abre ni lee ningún archivo, simplemente se hace un POST vacío
  Serial.println("Enviando POST vacío a la API");

  // Cerrar sesión HTTP anterior, si existe
  modem.sendAT("+HTTPTERM");
  String response;
  modem.waitResponse(2000, response);
  Serial.println("Response to HTTPTERM: " + response);

  // Inicializar HTTP
  modem.sendAT("+HTTPINIT");
  modem.waitResponse(5000, response);
  Serial.println("Response to HTTPINIT: " + response);
  if (response.indexOf("OK") == -1) {
    Serial.println("HTTPINIT failed");
    return;
  }

  // Configuración de parámetros para el POST
  modem.sendAT("+HTTPPARA=\"CID\",1");
  modem.waitResponse(2000, response);
  Serial.println("Response to CID: " + response);

  modem.sendAT(String("+HTTPPARA=\"URL\",\"") + API_URL + "\"");  // Usa la URL de tu API
  modem.waitResponse(2000, response);
  Serial.println("Response to URL: " + response);

  modem.sendAT("+HTTPPARA=\"CONTENT\",\"application/x-www-form-urlencoded\"");  // Tipo de contenido genérico
  modem.waitResponse(2000, response);
  Serial.println("Response to Content-Type: " + response);

  // Indicar que no se envían datos, es una solicitud POST vacía
  modem.sendAT("+HTTPDATA=0,10000");  // Enviamos 0 bytes
  modem.waitResponse(8000, response);
  Serial.println("Response to HTTPDATA: " + response);

  // Enviar el POST vacío
  modem.sendAT("+HTTPACTION=1");  // Acción POST
  modem.waitResponse(10000, response);
  Serial.println("Response to HTTPACTION: " + response);

  // Cerrar la sesión HTTP
  modem.sendAT("+HTTPTERM");
  modem.waitResponse(2000, response);
  Serial.println("Response to HTTPTERM (final): " + response);
}


// --- Process asynchronous modem events ----------------------------------
void readModemResponses() {
  while (modem.stream.available()) {
    String line = modem.stream.readStringUntil('\n');
    line.trim();

    if (line.startsWith("+HTTPACTION:")) {
      // parse status and length
      int c1 = line.indexOf(','), c2 = line.indexOf(',', c1 + 1);
      int status = line.substring(c1 + 1, c2).toInt();
      int length = line.substring(c2 + 1).toInt();
      Serial.printf("HTTPACTION status=%d length=%d\n", status, length);

      if (status == 200 && length > 0) {
        modem.sendAT(String("+HTTPREAD=0,") + length);
      }
    } else if (line.startsWith("+HTTPREAD:")) {
      int comma = line.indexOf(',');
      int length = (comma > 0) ? line.substring(comma + 1).toInt() : 0;
      httpReadData = "";
      unsigned long t0 = millis();
      while (millis() - t0 < 12000 && httpReadData.length() < length) {
        if (modem.stream.available())
          httpReadData += (char)modem.stream.read();
      }
      Serial.println("HTTPREAD data: " + httpReadData);
      // parse JSON or timestamp as before...
    } else if (line == "OK") {
      closeHttpSession();
    }
  }
}

// --- Execute AT command with display -------------------------------------
void executeATCommand(const String &cmd, unsigned long timeout) {
  while (modem.stream.available()) modem.stream.read();
  Serial.println("AT> " + cmd);
  displayModemResponse(cmd, "");
  modem.sendAT(cmd);
  String resp;
  modem.waitResponse(timeout, resp);
  Serial.println("AT< " + resp);
  displayModemResponse(cmd, resp);
}

// --- Display modem cmd/result on OLED -----------------------------------
void displayModemResponse(const String &cmd, const String &resp) {
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  display.setCursor(0, 12);
  display.print("->");
  display.print(cmd);
  display.setCursor(0, 24);
  display.print("<-");
  display.print(resp);
  display.sendBuffer();
}

// --- Basic SIM test ------------------------------------------------------
void testSIM() {
  executeATCommand("AT", 2000);
  executeATCommand("AT+CPIN?", 2000);
  executeATCommand("AT+CREG?", 2000);
  executeATCommand("AT+CGPADDR", 2000);
}

// --- Connect to network & GPRS -------------------------------------------
bool connectToNetwork() {
  for (int i = 0; i < 3; ++i) {
    Serial.println("Waiting for network...");
    if (modem.waitForNetwork() && modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
      Serial.println("GPRS connected");
      return true;
    }
    delay(5000);
  }
  return false;
}

// --- Fallback network info via AT ----------------------------------------
void getNetworkInfoFallback(String &op, String &tech, String &csq, String &reg) {
  modem.sendAT("AT+COPS?");
  String resp = modem.stream.readString();
  int q1 = resp.indexOf('"'), q2 = resp.indexOf('"', q1 + 1);
  op = (q1 >= 0 && q2 > q1) ? resp.substring(q1 + 1, q2) : "N/A";
  int lc = resp.lastIndexOf(',');
  int tn = (lc > 0) ? resp.substring(lc + 1).toInt() : -1;
  tech = (tn == 7) ? "LTE" : (tn == 2) ? "3G"
                           : (tn == 0) ? "GSM"
                                       : "N/A";

  modem.sendAT("AT+CSQ");
  resp = modem.stream.readString();
  int c = resp.indexOf(':'), c2 = resp.indexOf(',', c);
  csq = (c >= 0 && c2 > c) ? resp.substring(c + 1, c2) : "N/A";

  modem.sendAT("AT+CREG?");
  resp = modem.stream.readString();
  reg = (resp.indexOf("0,1") >= 0 || resp.indexOf("0,5") >= 0) ? "Registered" : "Not Reg";
}

// --- Update & display network info ---------------------------------------
void updateNetworkInfo() {
  getNetworkInfoFallback(networkOperator, networkTech, signalQuality, registrationStatus);
  display.clearBuffer();
  display.setFont(u8g2_font_ncenB08_tr);
  display.drawStr(0, 0, ("Op: " + networkOperator).c_str());
  display.drawStr(0, 16, ("Tec:" + networkTech).c_str());
  display.drawStr(0, 32, ("CSQ:" + signalQuality).c_str());
  display.drawStr(0, 48, ("Reg:" + registrationStatus).c_str());
  display.sendBuffer();
}

// --- Terminate HTTP session ----------------------------------------------
void closeHttpSession() {
  modem.sendAT("+HTTPTERM");
  modem.waitResponse(2000);
}

// --- Update RTC from server ----------------------------------------------
void rtcUpdate() {
  closeHttpSession();
  modem.sendAT("+HTTPINIT");
  if (modem.waitResponse(5000) != 1) return;
  modem.sendAT("+HTTPPARA=\"URL\",\"https://southamerica-west1-fic-aysen-412113.cloudfunctions.net/unixTime\"");
  modem.waitResponse(5000);
  executeATCommand("+HTTPACTION=0", 5000);
  // readModemResponses() will handle timestamp
  closeHttpSession();
}

// --- Periodic tasks & auto-request photo -------------------------------
void loopNormalTasks() {
  unsigned long now = millis();
  if (now - lastNetworkUpdate >= networkInterval) {
    lastNetworkUpdate = now;
    // updateNetworkInfo();
  }
  if (!receiving && now - lastDataSend >= sendInterval) {
    lastDataSend = now;
    fileSerial.println("foto");
    Serial.println("Auto request photo");
  }
}
