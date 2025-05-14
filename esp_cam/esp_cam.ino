/*
ESP32 with OV7670 camera (NO FIFO) streaming over WebSocket
Author: RawFish69
*/


#include <WiFi.h>
#include <Wire.h>
#include <OV7670.h>
#include <WebSocketsServer.h>

const char* AP_SSID     = "ESP32_CAM";
const char* AP_PASSWORD = ""; 
WiFiServer        httpServer(80);
WebSocketsServer  webSocket(81);

/*
  Wiring (camera → ESP32 Feather V2):
    PWDN      → unused
    RESET     → unused
    XCLK      → D4  (GPIO4)
    SDA (SIOD)→ D21 (GPIO21)
    SCL (SIOC)→ D22 (GPIO22)
    D7 (Y9)   → D27 (GPIO27)
    D6 (Y8)   → D26 (GPIO26)
    D5 (Y7)   → D25 (GPIO25)
    D4 (Y6)   → D33 (GPIO33)
    D3 (Y5)   → D32 (GPIO32)
    D2 (Y4)   → D14 (GPIO14) *input only*
    D1 (Y3)   → D34 (GPIO34) *input only*
    D0 (Y2)   → D39 (GPIO39) *input only*
    VSYNC     → D13 (GPIO13)
    HREF      → D12 (GPIO12)
    PCLK      → D5  (GPIO5)
*/
#define MODE   QQVGA   // 160×120
#define COLOR  RGB565

const camera_config_t cam_conf = {
  .D0 = 39,  .D1 = 34,  .D2 = 14,  .D3 = 32,
  .D4 = 33,  .D5 = 25,  .D6 = 26,  .D7 = 27,
  .XCLK         = 4,
  .PCLK         = 5,
  .VSYNC        = 13,
  .xclk_freq_hz = 10'000'000,
  .ledc_timer   = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0
};

OV7670    cam;
uint16_t* frameBuf = nullptr;
uint16_t  w = 160, h = 120;

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  Wire.begin(22, 20);   // SDA = GPIO22, SCL = GPIO20
  Wire.setClock(400000);

  Serial.println("[BOOT] Init camera...");
  if (cam.init(&cam_conf, MODE, COLOR) != ESP_OK) {
    Serial.println("[ERROR] cam.init failed!");
    while(true);
  }
  cam.setPCLK(2, DBLV_CLK_x4);
  cam.vflip(false);

  switch (MODE) {
    case QQVGA: w=160; h=120; break;
    case QVGA:  w=320; h=240; break;
    case QCIF:  w=176; h=144; break;
    case QQCIF: w=88;  h=72;  break;
  }
  Serial.printf("[INFO] %ux%u RGB565 stream\n", w, h);

  frameBuf = (uint16_t*)malloc(w * h * 2);
  if (!frameBuf) {
    Serial.println("[ERROR] malloc frameBuf");
    while(true);
  }

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[WIFI] AP IP = "); Serial.println(ip);
  httpServer.begin();
  webSocket.begin();
  webSocket.onEvent([](uint8_t num, WStype_t type, uint8_t * payload, size_t length){
    // not used
  });
}

void loop() {
  webSocket.loop();
  WiFiClient client = httpServer.available();
  if (client) {
    String req = client.readStringUntil('\r\n');
    while (client.available() && client.read()!='\n');
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println();
    client.println(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>OV7670 RAW Stream</title>
  <style>
    body { margin:0; overflow:hidden; background:#000; }
    #status {
      position:absolute; top:10px; left:10px;
      padding:4px 8px; background:rgba(0,0,0,0.6);
      color:#0f0; font-family:monospace; font-size:14px;
      z-index:100;
    }
    canvas {
      position:absolute; top:0; left:0;
      width:100vw; height:100vh;
      image-rendering: pixelated;
    }
  </style>
</head>
<body>
  <div id="status">Connecting...</div>
  <canvas id="cam" width="160" height="120"></canvas>
  <script>
    const W = 160, H = 120;
    const canvas = document.getElementById('cam');
    const ctx    = canvas.getContext('2d');
    const img    = ctx.createImageData(W, H);
    const status = document.getElementById('status');
    let frameCount = 0, lastTime = Date.now();

    function connect() {
      status.textContent = 'Connecting...';
      let ws = new WebSocket('ws://' + location.hostname + ':81');
      ws.binaryType = 'arraybuffer';

      ws.onopen = () => {
        status.textContent = 'Connected';
      };
      ws.onmessage = ev => {
        const data = new Uint16Array(ev.data);
        for (let i = 0; i < data.length; i++) {
          const px = data[i];
          img.data[4*i  ] = ((px>>11)&0x1F)*255/31;
          img.data[4*i+1] = ((px>>5 )&0x3F)*255/63;
          img.data[4*i+2] = ( px      &0x1F)*255/31;
          img.data[4*i+3] = 255;
        }
        ctx.putImageData(img, 0, 0);
        frameCount++;
        const now = Date.now();
        if (now - lastTime >= 1000) {
          status.textContent = 'FPS: ' + frameCount;
          frameCount = 0;
          lastTime = now;
        }
      };
      ws.onerror = () => {
        status.textContent = 'Error';
      };
      ws.onclose = () => {
        status.textContent = 'Disconnected, retrying...';
        setTimeout(connect, 1000);
      };
    }

    connect();
  </script>
</body>
</html>
    )rawliteral");
    client.stop();
  }
  for (uint16_t y = 0; y < h; y++) {
    uint16_t* line = cam.getLine(y + 1);
    memcpy(frameBuf + y*w, line, w*2);
  }
  webSocket.broadcastBIN((uint8_t*)frameBuf, w*h*2);
  delay(50); // this is optimistic, it will most likely not get 20 fps
}
