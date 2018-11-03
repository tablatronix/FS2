// Extended from ArduCAM Mini demo (C)2017 Lee      (Espressif 8266 to Wemos made by Lee)
// Project Website:  https://fasarek.de             ArduCAM Web:   http://www.ArduCAM.com
//  ______ _____ ___                                https://github.com/martinberlin/FS2 
// |  ____/ ____|__ \ 
// | |__ | (___    ) |
// |  __| \___ \  / / 
// | |    ____) |/ /_ 
// |_|   |_____/|____|     WiFi instant Camera
          
// PIN Definition for the ESP-32
// CS   16  Conflicts with OLED ?
// MOSI 23
// MISO 19
// SCK  5
// SDA  21
// SCL  22
#include "FS.h"
#include "SPIFFS.h"
#include <EEPROM.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <ArduCAM.h>
#include <SPI.h>
#include "Button2.h";
#include <ArduinoJson.h>    // Any version > 5.13.3 gave me an error on swap function
#include "FS2_functions.h"; // Helper functions
#include <WebServer.h>
#include <U8x8lib.h>        // OLED display
U8X8_SSD1306_128X64_NONAME_SW_I2C u8x8(/* clock=*/ 15, /* data=*/ 4, /* reset=*/ 16);
// CONFIGURATION
// Switch ArduCAM model to indicated ID. Ex.OV2640 = 5
byte cameraModelId = 5;                        // OV2640:5 |  OV5642:3   5MP  !IMPORTANT Nothing runs if model is not matched
bool saveInSpiffs = true;                      // Whether to save the jpg also in SPIFFS
const char* configModeAP = "CAM-autoconnect";  // Default config mode Access point
char* localDomain        = "cam";              // mDNS: cam.local
byte  CS = 17;                                 // set GPIO16 as the slave select
// NOTE: Select a Chip Select (CS) that does not conflict with any other connected SPI
#include "memorysaver.h"  // Uncomment the camera model you use
// NOTE:     ArduCAM owners please also make sure to choose your camera module in the ../libraries/ArduCAM/memorysaver.h
// ATTENTION NodeMCU: For NodeMCU 1.0 ESP-12E it only worked using Tools->CPU Frequency: 160 Mhz

// INTERNAL GLOBALS
// When timelapse is on will capture picture every N seconds
boolean captureTimeLapse;
boolean isStreaming = false;
static unsigned long lastTimeLapse;
unsigned long timelapseMillis;
// Flag for saving data in config.json
bool shouldSaveConfig = false;

byte D3 = 11;
byte D4 = 12;
byte D8 = 13;
// Outputs / Inputs (Shutter button D3)
Button2 buttonShutter = Button2(D3);
const int ledStatus = D4;
const int ledStatusTimelapse = D8;

WiFiManager wm;
WiFiClient client;

// Makes a div id="m" containing response message to dissapear after 6 seconds
String javascriptFadeMessage = "<script>setTimeout(function(){document.getElementById('m').innerHTML='';},6000);</script>";
String message;

// Note if saving to SPIFFS bufferSize needs to be 256, otherwise won't save correctly
static const size_t bufferSize = 256;
static uint8_t buffer[bufferSize] = {0xFF};

// UPLOAD Settings
String start_request = "";
String boundary = "_cam_";
String end_request = "\n--"+boundary+"--\n";
 
uint8_t temp = 0, temp_last = 0;
int i = 0;
bool is_header = false;
bool resetWifiSettings;

WebServer server(80);

// Automatic switch between 2MP and 5MP models
ArduCAM myCAM(cameraModelId, CS);

// jpeg_size_id Setting to set the camara Width/Height resolution. Smallest is default if no string match is done by config
uint8_t jpeg_size_id = 1;

// Definition for WiFi defaults
char timelapse[4] = "60";
char upload_host[120] = "api.slosarek.eu";
char upload_path[240] = "/your/upload.php";
char slave_cam_ip[16] = "";
char jpeg_size[10]  = "1600x1200";

// SPIFFS and memory to save photos
File fsFile;
String webTemplate = "";
bool onlineMode = true;
struct config_t
{
    byte photoCount = 1;
    bool resetWifiSettings;
    bool saveParamCallback;
} memory;

/**
 * Generic message printer. Modify this if you want to send this messages elsewhere (Display)
 */
void printMessage(String message, bool newline = true, bool displayClear = false) {
  if (displayClear) {
    u8x8.clear();
  }
  u8x8.print(message);
  if (newline) {
    u8x8.print("\n");
    Serial.println(message);
  } else {
    Serial.print(message);
  }
  return;
}

void setup() {
  u8x8.begin();
  u8x8.setFont(u8x8_font_chroma48medium8_r);
  String cameraModel; 
  if (cameraModelId == 5) {
    // Please select the hardware platform for your camera module in the ../libraries/ArduCAM/memorysaver.h file
    cameraModel = "OV2640";
  }
  if (cameraModelId == 3) {
    cameraModel = "OV5642";
  }
  EEPROM.begin(12);
  Serial.begin(115200);
  // Find out what are this PINS on ESP32
  //Serial.println(MOSI);
  //Serial.println(MISO);
  //Serial.println(SCK);
  //Serial.println(SDA);

  // Define outputs. This are also ledStatus signals (Red: no WiFI, B: Timelapse, G: Arducam Chip select)
  pinMode(CS, OUTPUT);
  pinMode(ledStatus, OUTPUT);
  pinMode(ledStatusTimelapse, OUTPUT);
  
  // Read memory struct from EEPROM
  EEPROM_readAnything(0, memory);
  printMessage("FS2 CAMERA");
  printMessage(" ______ _____");
  printMessage("|  ____/ ____|");
  printMessage("| |__ | (___  ");
  printMessage("|  __| \\___ \\ ");
  printMessage("| |    ____) |");
  printMessage("|_|   |_____/ 2");

  // Read configuration from FS json
  if (SPIFFS.begin()) {
    //printMessage("SPIFFS mount OK");

   if (SPIFFS.exists("/config.json")) {
      File configFile = SPIFFS.open("/config.json", FILE_READ);
      if (configFile) {
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {          
          strcpy(timelapse, json["timelapse"]);
          strcpy(upload_host, json["upload_host"]);
          strcpy(upload_path, json["upload_path"]);
          strcpy(slave_cam_ip, json["slave_cam_ip"]);
          strcpy(jpeg_size, json["jpeg_size"]);

        } else {
          printMessage("ERR load config");
        }
        configFile.close();
      }
    }
  } else {
    printMessage("ERR FS failed");
    printMessage("FFS Formatted?");
  }
//end read

  std::vector<const char *> menu = {"wifi", "sep", "param","sep", "info", "restart"};
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_html("<h2>Camera configuration</h2><p>After saving camera will restart and try to login to last WiFi connection</p>"); 
  WiFiManagerParameter param_timelapse("timelapse", "Timelapse in secs", timelapse,4);
  WiFiManagerParameter param_slave_cam_ip("slave_cam_ip", "Slave cam ip/ping", slave_cam_ip,16);
  WiFiManagerParameter param_upload_host("upload_host", "API host for upload", upload_host,120);
  WiFiManagerParameter param_upload_path("upload_path", "Path to API endoint", upload_path,240);
  // Variable depending on the camera model
  WiFiManagerParameter param_jpeg_size("jpeg_size", "Select JPG Size: 640x480 1024x768 1280x1024 1600x1200 (2 & 5mp) / 2048x1536 2592x1944 (only 5mp)", jpeg_size, 10);
 
 if (onlineMode) {
  //printMessage("> Camera is on ONLINE Mode");
  // This is triggered on next restart after click in RESET WIFI AND EDIT CONFIGURATION
  if (memory.resetWifiSettings) {
    wm.resetSettings();
  }
  wm.setMenu(menu);
  // Add the defined parameters to wm
  wm.addParameter(&custom_html);
  wm.addParameter(&param_timelapse);
  wm.addParameter(&param_slave_cam_ip);
  wm.addParameter(&param_upload_host);
  wm.addParameter(&param_upload_path);
  wm.addParameter(&param_jpeg_size);
  
  wm.setMinimumSignalQuality(20);
  // Callbacks configuration
  wm.setSaveParamsCallback(saveParamCallback);
  wm.setBreakAfterConfig(true); // Without this saveConfigCallback does not get fired
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setAPCallback(configModeCallback);
  wm.setDebugOutput(false);
  // If saveParamCallback is called then on next restart trigger config portal to update camera params
  if (memory.saveParamCallback) {
    // Let's do this just one time: Restarting again should connect to previous WiFi
    memory.saveParamCallback = false;
    EEPROM_writeAnything(0, memory);
    wm.startConfigPortal(configModeAP);
  } else {
    wm.autoConnect(configModeAP);
  }
 } else {
   printMessage("OFFLINE Mode");
 }

  // Read updated parameters
  strcpy(timelapse, param_timelapse.getValue());
  strcpy(slave_cam_ip, param_slave_cam_ip.getValue());
  strcpy(upload_host, param_upload_host.getValue());
  strcpy(upload_path, param_upload_path.getValue());
  strcpy(jpeg_size, param_jpeg_size.getValue());

  if (shouldSaveConfig) {
    printMessage("Save config.json", true, true);
   
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["timelapse"] = timelapse;
    json["slave_cam_ip"] = slave_cam_ip;
    json["upload_host"] = upload_host;
    json["upload_path"] = upload_path;
    json["jpeg_size"] = jpeg_size;
    Serial.println("timelapse:"+String(timelapse));
    Serial.println("slave_cam_ip:"+String(slave_cam_ip));
    Serial.println("upload_host:"+String(upload_host));
    Serial.println("upload_path:"+String(upload_path));
    Serial.println("jpeg_size:"+String(jpeg_size));
    
    File configFile = SPIFFS.open("/config.json", FILE_WRITE);
    if (!configFile) {
      printMessage("ERR config file");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
  }

  // Convert timelapse(char) to timelapseInt and then to  milliseconds
  int timelapseInt = atoi(timelapse);
  timelapseMillis = (timelapseInt) * 1000;

  // Button events
  buttonShutter.setReleasedHandler(shutterReleased); // Takes picture
  buttonShutter.setLongClickHandler(shutterLongClick); // Starts timelapse
  uint8_t vid, pid;
  uint8_t temp;
#if defined(__SAM3X8E__)
  Wire1.begin();
#else
  Wire.begin();
#endif

  // initialize SPI:
  SPI.begin();
  SPI.setFrequency(4000000); //4MHz

  //Check if the ArduCAM SPI bus is OK
  myCAM.write_reg(ARDUCHIP_TEST1, 0x55);
  temp = myCAM.read_reg(ARDUCHIP_TEST1);
  if (temp != 0x55) {
    printMessage("ERR SPI :Check");
    printMessage("ArduCam wiring");
    while (1);
  }
// TODO Refactor this ugly casting to string just because c adds the 0 operator at end of chars
  if (cameraModel == "OV2640") {
    if (String(jpeg_size) == "640x480") {
      jpeg_size_id = 4;
      }
    if (String(jpeg_size) == "800x600") {
      jpeg_size_id = 5;
      }
    if (String(jpeg_size) == "1024x768") {
      jpeg_size_id = 6;
      }
    if (String(jpeg_size) == "1280x1024") {
      jpeg_size_id = 7;
      }
    if (String(jpeg_size) == "1600x1200") {
      jpeg_size_id = 8;
      }

  // Check if the camera module type is OV2640
  myCAM.wrSensorReg8_8(0xff, 0x01);
  myCAM.rdSensorReg8_8(10, &vid);
  myCAM.rdSensorReg8_8(11, &pid);

  if ((vid != 0x26 ) && (( pid != 0x41 ) || ( pid != 0x42 ))) {
    printMessage("ERR conn OV2640");
  } else {
    printMessage("CAMERA READY\n\n", true, true);
    myCAM.set_format(JPEG);
    myCAM.InitCAM();
    myCAM.OV2640_set_JPEG_size(jpeg_size_id); 
  }
}

  if (cameraModel == "OV5642") {
    if (String(jpeg_size) == "640x480") {
      jpeg_size_id = 1;
    }
    if (String(jpeg_size) == "1024x768") {
      jpeg_size_id = 2;
    }
    if (String(jpeg_size) == "1280x1024") {
      jpeg_size_id = 3;
    }
    if (String(jpeg_size) == "1600x1200") {
      jpeg_size_id = 4;
    } 
    if (String(jpeg_size) == "2048x1536") {
      jpeg_size_id = 5;
    } 
    if (String(jpeg_size) == "2592x1944") {
      jpeg_size_id = 6;
    }
    
    temp=SPI.transfer(0x00);
    myCAM.clear_bit(6, GPIO_PWDN_MASK); //disable low power
    //Check if the camera module type is OV5642
    myCAM.wrSensorReg16_8(0xff, 0x01);
    myCAM.rdSensorReg16_8(12298, &vid);
    myCAM.rdSensorReg16_8(12299, &pid);

   if((vid != 0x56) || (pid != 0x42)) {
     printMessage("ERR conn OV5642");
     
   } else {
     printMessage("OK > OV5642");
     myCAM.set_format(JPEG);
     myCAM.InitCAM();
     // ARDUCHIP_TIM, VSYNC_LEVEL_MASK
     myCAM.write_reg(3, 2);   //VSYNC is active HIGH
     myCAM.OV5642_set_JPEG_size(jpeg_size_id);
   }
  }

  printMessage("size: "+String(jpeg_size)+"\n");
  printMessage("Counter: "+String(memory.photoCount)+"\n");

  myCAM.clear_fifo_flag();

   // Set up mDNS responder:
  // - first argument is the domain name, in FS2 the fully-qualified domain name is "cam.local"
  // - second argument is the IP address to advertise
  if (onlineMode) {
    if (!MDNS.begin(localDomain)) {
      while(1) { 
        delay(500);
      }
    }
    // Add service to MDNS-SD
    MDNS.addService("http", "tcp", 80);
    
    printMessage("http://"+String(localDomain)+".local");
    // ROUTES
    server.on("/capture", HTTP_GET, serverCapture);
    server.on("/stream", HTTP_GET, serverStream);
    server.on("/timelapse/start", HTTP_GET, serverStartTimelapse);
    server.on("/timelapse/stop", HTTP_GET, serverStopTimelapse);
    server.on("/fs/list", HTTP_GET, serverListFiles);
    server.on("/fs/download", HTTP_GET, serverDownloadFile);
    server.on("/fs/delete", HTTP_GET, serverDeleteFile);
    server.on("/wifi/reset", HTTP_GET, serverResetWifiSettings);
    server.on("/camera/settings", HTTP_GET, serverCameraParams);
    
    server.onNotFound(handleWebServerRoot);
    server.begin();
  }
  lastTimeLapse = millis() + timelapseMillis;  // Initialize timelapse
  }

void start_capture() {
  myCAM.clear_fifo_flag();
  myCAM.start_capture();
}


String camCapture(ArduCAM myCAM) {
   // Check if available bytes in SPIFFS
  uint32_t bytesAvailableSpiffs = SPIFFS.totalBytes()-SPIFFS.usedBytes();
  uint32_t len  = myCAM.read_fifo_length();

  printMessage("photoCount: "+String(memory.photoCount));
  printMessage(String(bytesAvailableSpiffs/1024)+ " Kb avail");
  if (len*2 > bytesAvailableSpiffs) {
    memory.photoCount = 1;
    printMessage("count reseted 1");
  }
  long full_length;
  
  if (len == 0) //0 kb
  {
    message = "ERR read memory";
    printMessage(message);
    return message;
  }

  myCAM.CS_LOW();
  myCAM.set_fifo_burst();
  if (cameraModelId == 5) {
    SPI.transfer(0xFF);
  }
  if (client.connect(upload_host, 80) || onlineMode) { 
    if (onlineMode) {
      while(client.available()) {
        String line = client.readStringUntil('\r');
       }  // Empty wifi receive bufffer
    }
  start_request = start_request + 
  "\n--"+boundary+"\n" + 
  "Content-Disposition: form-data; name=\"upload\"; filename=\"CAM.JPG\"\n" + 
  "Content-Transfer-Encoding: binary\n\n";
  
   full_length = start_request.length() + len + end_request.length();
   printMessage(String(full_length/1024)+ " Kb sent");
   printMessage(String(upload_host));
    client.println("POST "+String(upload_path)+" HTTP/1.1");
    client.println("Host: "+String(upload_host));
    client.println("Content-Type: multipart/form-data; boundary="+boundary);
    client.print("Content-Length: "); client.println(full_length);
    client.println();
    client.print(start_request);
  if (saveInSpiffs) {
    fsFile = SPIFFS.open("/"+String(memory.photoCount)+".jpg", "w");
  }
  // Read image data from Arducam mini and send away to internet
  static uint8_t buffer[bufferSize] = {0xFF};
  while (len) {
      size_t will_copy = (len < bufferSize) ? len : bufferSize;
      
      SPI.transferBytes(&buffer[0], &buffer[0], will_copy);
      //We won't break the WiFi upload if client disconnects since this is also for SPIFFS upload
      if (client.connected()) {
         client.write(&buffer[0], will_copy);
      }
      if (fsFile && saveInSpiffs) {
        fsFile.write(&buffer[0], will_copy);
      }
      len -= will_copy;
  }

  if (fsFile && saveInSpiffs) {
    fsFile.close();
  }

  memory.photoCount++;
  EEPROM_writeAnything(0, memory);
  
  client.println(end_request);
  myCAM.CS_HIGH(); 

  bool   skip_headers = true;
  String rx_line;
  String response;
  
  // Read all the lines of the reply from server and print them to Serial
    int timeout = millis() + 5000;
  while (client.available() == 0) {
    if (timeout - millis() < 0) {
      message = "> Client Timeout waiting for reply after sending JPG (5 sec. timeout reached)";
      printMessage("Client timeout");
      client.stop();
      return message;
    }
  }
  while(client.available()) {
    rx_line = client.readStringUntil('\r');
    Serial.println( rx_line );
    if (rx_line.length() <= 1) { // a blank line denotes end of headers
        skip_headers = false;
      }
      // Collect http response
     if (!skip_headers) {
            response += rx_line;
     }
  }
  response.trim();
  
  //Serial.println( response );
  client.stop();
  return response;
  } else {
    message = "ERROR: Could not connect to "+String(upload_host);
    printMessage("Conn failed to");
    printMessage(String(upload_host));
    return message;
  }
}

void serverCapture() {
  digitalWrite(ledStatus, HIGH);
  
  isStreaming = false;
  start_capture();
  printMessage("CAPTURING", true, true);

  int total_time = 0;
  total_time = millis();
  while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK));
  total_time = millis() - total_time;
  printMessage("DONE "+String(total_time));
  

  if (slave_cam_ip != "" && onlineMode) {
    printMessage("PING to\n"+String(slave_cam_ip));
    shutterPing();
  }
  total_time = 0;
  total_time = millis();
  String imageUrl = camCapture(myCAM);
  total_time = millis() - total_time;
  
  digitalWrite(ledStatus, LOW);
  if (onlineMode) {
    //printMessage("Image url: "+imageUrl);
    server.send(200, "text/html", "<div id='m'><small>"+imageUrl+
              "</small><br><img src='"+imageUrl+"' width='400'></div>"+ javascriptFadeMessage);
  }
}

void serverStream() {
  printMessage("STREAMING");
  isStreaming = true;
  WiFiClient client = server.client();

  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (isStreaming) {
    if (onlineMode) { 
      server.handleClient(); 
    }
    start_capture();
    while (!myCAM.get_bit(ARDUCHIP_TRIG, CAP_DONE_MASK));
    size_t len = myCAM.read_fifo_length();

    if (len == 0) //0 kb
    {
      continue;
    }
    myCAM.CS_LOW();
    myCAM.set_fifo_burst();
    if (!client.connected()) {
      client.stop(); is_header = false; break;
    }
    response = "--frame\r\n";
    response += "Content-Type: image/jpeg\r\n\r\n";
    server.sendContent(response);
    
    while ( len-- )
    {
      temp_last = temp;
      temp =  SPI.transfer(0x00);

      //Read JPEG data from FIFO
      if ( (temp == 0xD9) && (temp_last == 0xFF) ) //If find the end ,break while,
      {
        buffer[i++] = temp;  //save the last  0XD9
        //Write the remain bytes in the buffer
        myCAM.CS_HIGH();;
        if (!client.connected()) {
          client.stop(); is_header = false; break;
        }
        client.write(&buffer[0], i);
        is_header = false;
        i = 0;
      }
      if (is_header == true)
      {
        //Write image data to buffer if not full
        if (i < bufferSize)
          buffer[i++] = temp;
        else
        {
          //Write bufferSize bytes image data to file
          myCAM.CS_HIGH();
          if (!client.connected()) {
            client.stop(); is_header = false; break;
          }
          client.write(&buffer[0], bufferSize);
          i = 0;
          buffer[i++] = temp;
          myCAM.CS_LOW();
          myCAM.set_fifo_burst();
        }
      }
      else if ((temp == 0xD8) & (temp_last == 0xFF))
      {
        is_header = true;
        buffer[i++] = temp_last;
        buffer[i++] = temp;
      }
    }
    if (!client.connected()) {
      client.stop(); is_header = false; break;
    }
  }
}

/**
 * This is the home page and also the page that comes when a 404 is generated
 */
void handleWebServerRoot() {
  String fileName = "/ux.html";
  
  if (SPIFFS.exists(fileName)) {
    File file = SPIFFS.open(fileName, "r");
    server.streamFile(file, getContentType(fileName));
    file.close();
  } else {
    message = "ERROR: handleWebServerRoot could not read "+fileName;
    printMessage(message);
    server.send(200, "text/html", message);
    return;
  }
  
  server.send(200, "text/html");
}

void configModeCallback(WiFiManager *myWiFiManager) {
  digitalWrite(ledStatus, HIGH);
  message = "CAM can't get online. Entering config mode. Please connect to access point "+String(configModeAP);
  printMessage("CAM offline");
  printMessage("connect to ");
  printMessage(String(configModeAP));
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

void saveConfigCallback() {
  memory.resetWifiSettings = false;
  EEPROM_writeAnything(0, memory);
  shouldSaveConfig = true;
  printMessage("Saving config");
}

void saveParamCallback(){
  shouldSaveConfig = true;
  delay(100);
  wm.stopConfigPortal();
  Serial.println("[CALLBACK] saveParamCallback fired -> should save config is TRUE");
}

void shutterPing() {
  // Attempt to read settings.slave_cam_ip and ping a second camera
  if (slave_cam_ip == "") return;
  
  if (!client.connect(slave_cam_ip, 80)) {
    printMessage("Ping failed");
    return;
  }
    // This will send the request to the server
  client.print(String("GET ") + "/capture HTTP/1.1\r\n" +
               "Host: " + slave_cam_ip + "\r\n" + 
               "Connection: close\r\n\r\n");
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 100) {
      printMessage("> Pinged target "+String(slave_cam_ip));
      client.stop();
      return;
    }
  }
}

void serverStartTimelapse() {
    digitalWrite(ledStatusTimelapse, HIGH);
    printMessage("> Long click: TIMELAPSE enabled");
    captureTimeLapse = true;
    lastTimeLapse = millis() + timelapseMillis;
    server.send(200, "text/html", "<div id='m'>Start Timelapse</div>"+ javascriptFadeMessage);
}

void serverStopTimelapse() {
    digitalWrite(ledStatusTimelapse, LOW);
    printMessage("> TIMELAPSE disabled");
    captureTimeLapse = false;
    server.send(200, "text/html", "<div id='m'>Stop Timelapse</div>"+ javascriptFadeMessage);
}

void serverResetWifiSettings() {
    printMessage("> Resseting WiFi settings, please connect to "+String(configModeAP)+" and setup a new connection");
    Serial.println("resetWifiSettings flag is saved on EEPROM");
    memory.resetWifiSettings = true;
    memory.photoCount = 1;
    EEPROM_writeAnything(0, memory);
    server.send(200, "text/html", "<div id='m'><h5>Restarting please connect to "+String(configModeAP)+"</h5>WiFi credentials will be deleted and camera will start in configuration mode.</div>"+ javascriptFadeMessage);
    delay(500);
    ESP.restart();
}

void serverCameraParams() {
    printMessage("> Restarting. Connect to "+String(configModeAP)+" and click SETUP to update camera configuration");
    memory.saveParamCallback = true;
    EEPROM_writeAnything(0, memory);
    server.send(200, "text/html", "<div id='m'><h5>Restarting please connect to "+String(configModeAP)+"</h5>Edit camera configuration using <b>Setup</b></div>"+ javascriptFadeMessage);
    delay(500);
    ESP.restart();
}

void serverListFiles() {
  
  String fileName = "/template.html";
  webTemplate = "";
  printMessage("Listing files on SPIFFS:");
  
  if (SPIFFS.exists(fileName)) {
    File file = SPIFFS.open(fileName, "r");
    while (file.available() != 0) {  
      webTemplate += file.readStringUntil('\n');  
    }
    file.close();
  } else {
    Serial.println("Could not read "+fileName+" from SPIFFS");
    server.send(200, "text/html", "Could not read "+fileName+" from SPIFFS");
    return;
  }
  
  String body = "<table class='table'>";
  body += "<tr><th>File</th><th>Size</th><th>Del</th></tr>";
  //'Dir' was not declared in this scope
//  Dir dir = SPIFFS.openDir("/");
    File root = SPIFFS.open("/");
    if(!root){
        Serial.println("- failed to open directory");
        return;
}
  String fileUnit;
  unsigned int fileSize;
  char fileChar[32];
  
    File file = root.openNextFile();
    
    while(file){
      String fileName = file.name();
      fileName.toCharArray(fileChar, 32);
      if (!isServerListable(fileChar)) {
        // Move the pointer to avoid endless loop
        file = root.openNextFile();
        continue;
      }
      
      if (file.size()<1024) {
          fileUnit = " bytes";
          fileSize = file.size();
          } else {
            fileUnit = " Kb";
            fileSize = file.size()/1024;
          }
      fileName.remove(0,1);
      body += "<tr><td><a href='/fs/download?f="+fileName+"'>";
      body += fileName+"</a></td>";
      body += "<td>"+ String(fileSize)+fileUnit +"</td>";
      body += "<td>";
      if (isServerDeleteable(fileName)) {
        body += "<a class='btn-sm btn-danger' href='/fs/delete?f="+fileName+"'>x</a>";
      }
      body += "</td>";
      body += "</tr>";
      printMessage(fileName+ " "+ String(fileSize)+fileUnit);
      file = root.openNextFile();
  }
    
  body += "</table>";
  body += "<br>Total KB: "+String(SPIFFS.totalBytes()/1024)+" Kb";
  body += "<br>Used KB: "+String(SPIFFS.usedBytes()/1024)+" Kb";
  body += "<br>Avail KB: <b>"+String((SPIFFS.totalBytes()-SPIFFS.usedBytes())/1024)+" Kb</b><br>";

  webTemplate.replace("{{localDomain}}", localDomain);
  webTemplate.replace("{{home}}", "Camera UI");
  webTemplate.replace("{{body}}", body);
  
  server.send(200, "text/html", webTemplate);
}

void serverDownloadFile() {
  if (server.args() > 0 ) { 
    if (server.hasArg("f")) {
      String filename = server.arg(0);
      File download = SPIFFS.open("/"+filename, "r");
      if (download) {
        server.sendHeader("Content-Type", "text/text");
        server.sendHeader("Content-Disposition", "attachment; filename="+filename);
        server.sendHeader("Connection", "close");
        server.streamFile(download, "application/octet-stream");
        download.close();
      } else {
        server.send(404, "text/html", "file: "+ filename +" not found.");
      }
    } else {
      server.send(404, "text/html", "f parameter not received by GET.");
    }
  } else {
    server.send(404, "text/html", "No server parameters received.");
  }
}

void serverDeleteFile() {
  if (server.args() > 0 ) { 
    if (server.hasArg("f")) {
      String filename = server.arg(0);
      if(isServerDeleteable(filename)) {
         SPIFFS.remove("/"+filename);
         printMessage("> Deleting "+ filename);
      }
      server.sendHeader("Location", "/fs/list", true);
      server.send (302, "text/plain", "");
    } else {
      server.send(404, "text/html", "f parameter not received by GET.");
    }
  }
}

bool isServerDeleteable(String filename) {
  if (filename == "config.json"
    ||filename == "template.html"
    ||filename == "ux.html")
  {
    return false;
  } 
  return true;
}
  
bool isServerListable(char* filename) {
  int8_t len = strlen(filename);
  bool result;
  if (  strstr(strlwr(filename + (len - 4)), ".jpg")
     || strstr(strlwr(filename + (len - 5)), ".json")
    ) {
    result = true;
  } else {
    result = false;
  }
  return result;
}

// Button events
void shutterReleased(Button2& btn) {
    digitalWrite(ledStatusTimelapse, LOW);
    captureTimeLapse = false;
    serverCapture();
}
void shutterLongClick(Button2& btn) {
    digitalWrite(ledStatusTimelapse, HIGH);
    captureTimeLapse = true;
    lastTimeLapse = millis() + timelapseMillis;
}

void loop() {
   if (onlineMode) { 
    server.handleClient(); 
   }
  buttonShutter.loop();
  
  if (captureTimeLapse && millis() >= lastTimeLapse) {
    lastTimeLapse += timelapseMillis;
    serverCapture();
    printMessage("> Timelapse captured");
  }
}
