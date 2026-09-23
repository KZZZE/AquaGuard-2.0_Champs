#include <Arduino.h>
#include "esp_camera.h"
#include "img_converters.h"

#include <WiFi.h>
#include "esp_http_server.h"

#include <esp_heap_caps.h>

#include <TensorFlowLite_ESP32.h>

#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "person_detect_model_data.h"


// =====================================================
// WIFI
// =====================================================

const char* ssid = "superpower";
const char* password = "12121212";


// =====================================================
// AI THINKER ESP32-CAM + OV2640
// =====================================================

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1

#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22


// =====================================================
// PERSON DETECTION
// =====================================================

constexpr int kNumCols = 96;
constexpr int kNumRows = 96;
constexpr int kNumChannels = 1;

constexpr int kPersonIndex = 1;
constexpr int kNotAPersonIndex = 0;

const float PERSON_THRESHOLD = 0.60f;


// =====================================================
// TFLITE
// =====================================================

tflite::ErrorReporter* error_reporter = nullptr;

const tflite::Model* model = nullptr;

tflite::MicroInterpreter* interpreter = nullptr;

TfLiteTensor* input = nullptr;


// ESP32 non-S3: 81 KB seperti example TensorFlowLite_ESP32
constexpr int kTensorArenaSize = 81 * 1024;

uint8_t* tensor_arena = nullptr;


// =====================================================
// AI STATUS
// =====================================================

volatile bool humanDetected = false;

volatile float personScore = 0.0f;

volatile float noPersonScore = 0.0f;


// =====================================================
// CAMERA MUTEX
// =====================================================

SemaphoreHandle_t cameraMutex;


// =====================================================
// RGB BUFFER
//
// VGA = 640 x 480
// RGB888 = 640 x 480 x 3
//
// Dialokasikan di PSRAM.
// =====================================================

uint8_t* rgbBuffer = nullptr;

constexpr size_t RGB_BUFFER_SIZE =
  640UL * 480UL * 3UL;


// =====================================================
// HTTP SERVER
// =====================================================

httpd_handle_t camera_httpd = NULL;


// =====================================================
// STREAM
// =====================================================

#define PART_BOUNDARY "123456789000000000000987654321"

static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static const char* STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";

static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n\r\n";


// =====================================================
// WEB PAGE
// =====================================================

static const char INDEX_HTML[] PROGMEM = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta charset="UTF-8">

<meta name="viewport"
content="width=device-width, initial-scale=1">

<title>AquaGuard CCTV</title>

<style>

body {

  margin: 0;

  background: #111;

  color: white;

  font-family: Arial, sans-serif;

  text-align: center;

}

h1 {

  margin: 20px 0 15px 0;

}

.camera {

  width: 95%;

  max-width: 800px;

  border-radius: 10px;

}

.status {

  margin: 20px auto;

  padding: 15px;

  width: 90%;

  max-width: 700px;

  border-radius: 12px;

  background: #222;

}

.statusText {

  font-size: 24px;

  font-weight: bold;

}

.score {

  margin-top: 10px;

  font-size: 17px;

}

.detected {

  color: #ff4444;

}

.safe {

  color: #00dd88;

}

.info {

  margin-top: 10px;

  color: #aaa;

  font-size: 14px;

}

</style>

</head>


<body>

<h1>🌊 AQUAGUARD CCTV</h1>

<img
  class="camera"
  src="/stream"
>


<div class="status">

  <div
    id="statusText"
    class="statusText safe"
  >
    🟢 TIDAK ADA MANUSIA
  </div>


  <div
    id="score"
    class="score"
  >
    Person score: 0.00
  </div>


  <div class="info">
    ESP32-CAM • TensorFlow Lite
  </div>

</div>


<script>

function updateStatus() {

  fetch('/status')

  .then(response => response.json())

  .then(data => {

    const statusText =
      document.getElementById("statusText");

    const score =
      document.getElementById("score");


    if (data.human) {

      statusText.innerHTML =
        "🔴 MANUSIA TERDETEKSI";

      statusText.className =
        "statusText detected";

    }

    else {

      statusText.innerHTML =
        "🟢 TIDAK ADA MANUSIA";

      statusText.className =
        "statusText safe";

    }


    score.innerHTML =
      "Person score: " +
      data.person.toFixed(2);

  })

  .catch(error => {

    console.log(error);

  });

}


setInterval(updateStatus, 1000);

updateStatus();

</script>


</body>

</html>

)rawliteral";


// =====================================================
// INDEX HANDLER
// =====================================================

static esp_err_t index_handler(
  httpd_req_t *req
)
{

  httpd_resp_set_type(
    req,
    "text/html; charset=UTF-8"
  );

  return httpd_resp_send(
    req,
    INDEX_HTML,
    strlen(INDEX_HTML)
  );
}


// =====================================================
// STATUS HANDLER
// =====================================================

static esp_err_t status_handler(
  httpd_req_t *req
)
{

  char json[160];


  snprintf(

    json,
    sizeof(json),

    "{\"human\":%s,\"person\":%.3f,\"no_person\":%.3f}",

    humanDetected ? "true" : "false",

    personScore,

    noPersonScore

  );


  httpd_resp_set_type(
    req,
    "application/json"
  );


  return httpd_resp_send(
    req,
    json,
    strlen(json)
  );
}


// =====================================================
// STREAM HANDLER
// =====================================================

static esp_err_t stream_handler(
  httpd_req_t *req
)
{

  camera_fb_t *fb = NULL;

  esp_err_t res = ESP_OK;

  char part_buf[64];


  res = httpd_resp_set_type(
    req,
    STREAM_CONTENT_TYPE
  );


  if (res != ESP_OK) {

    return res;

  }


  while (true) {


    // -----------------------------------------------
    // Ambil frame
    // -----------------------------------------------

    if (
      xSemaphoreTake(
        cameraMutex,
        pdMS_TO_TICKS(1000)
      ) != pdTRUE
    ) {

      continue;

    }


    fb = esp_camera_fb_get();


    xSemaphoreGive(
      cameraMutex
    );


    if (!fb) {

      Serial.println(
        "Stream: capture gagal"
      );

      res = ESP_FAIL;

      break;

    }


    // -----------------------------------------------
    // Kirim JPEG
    // -----------------------------------------------

    size_t hlen = snprintf(

      part_buf,

      sizeof(part_buf),

      STREAM_PART,

      fb->len

    );


    res = httpd_resp_send_chunk(

      req,

      part_buf,

      hlen

    );


    if (res == ESP_OK) {

      res = httpd_resp_send_chunk(

        req,

        (const char*)fb->buf,

        fb->len

      );

    }


    if (res == ESP_OK) {

      res = httpd_resp_send_chunk(

        req,

        STREAM_BOUNDARY,

        strlen(STREAM_BOUNDARY)

      );

    }


    esp_camera_fb_return(fb);

    fb = NULL;


    if (res != ESP_OK) {

      break;

    }


    delay(1);

  }


  if (fb) {

    esp_camera_fb_return(fb);

  }


  return res;
}


// =====================================================
// START CAMERA SERVER
// =====================================================

void startCameraServer()
{

  httpd_config_t config =
    HTTPD_DEFAULT_CONFIG();


  config.server_port = 80;


  // -----------------------------------------------
  // ROOT
  // -----------------------------------------------

  httpd_uri_t index_uri = {};

  index_uri.uri = "/";

  index_uri.method = HTTP_GET;

  index_uri.handler = index_handler;

  index_uri.user_ctx = NULL;


  // -----------------------------------------------
  // STREAM
  // -----------------------------------------------

  httpd_uri_t stream_uri = {};

  stream_uri.uri = "/stream";

  stream_uri.method = HTTP_GET;

  stream_uri.handler = stream_handler;

  stream_uri.user_ctx = NULL;


  // -----------------------------------------------
  // STATUS
  // -----------------------------------------------

  httpd_uri_t status_uri = {};

  status_uri.uri = "/status";

  status_uri.method = HTTP_GET;

  status_uri.handler = status_handler;

  status_uri.user_ctx = NULL;


  // -----------------------------------------------
  // START SERVER
  // -----------------------------------------------

  esp_err_t err =

    httpd_start(

      &camera_httpd,

      &config

    );


  if (err != ESP_OK) {

    Serial.printf(

      "Web server gagal: 0x%x\n",

      err

    );

    return;

  }


  httpd_register_uri_handler(

    camera_httpd,

    &index_uri

  );


  httpd_register_uri_handler(

    camera_httpd,

    &stream_uri

  );


  httpd_register_uri_handler(

    camera_httpd,

    &status_uri

  );


  Serial.println(
    "Web server kamera aktif"
  );
}


// =====================================================
// CAMERA INIT
// =====================================================

bool initCamera()
{

  camera_config_t config = {};


  config.ledc_channel =
    LEDC_CHANNEL_0;

  config.ledc_timer =
    LEDC_TIMER_0;


  config.pin_d0 =
    Y2_GPIO_NUM;

  config.pin_d1 =
    Y3_GPIO_NUM;

  config.pin_d2 =
    Y4_GPIO_NUM;

  config.pin_d3 =
    Y5_GPIO_NUM;

  config.pin_d4 =
    Y6_GPIO_NUM;

  config.pin_d5 =
    Y7_GPIO_NUM;

  config.pin_d6 =
    Y8_GPIO_NUM;

  config.pin_d7 =
    Y9_GPIO_NUM;


  config.pin_xclk =
    XCLK_GPIO_NUM;

  config.pin_pclk =
    PCLK_GPIO_NUM;


  config.pin_vsync =
    VSYNC_GPIO_NUM;

  config.pin_href =
    HREF_GPIO_NUM;


  // Untuk Core 2.0.17
  config.pin_sscb_sda =
    SIOD_GPIO_NUM;

  config.pin_sscb_scl =
    SIOC_GPIO_NUM;


  config.pin_pwdn =
    PWDN_GPIO_NUM;

  config.pin_reset =
    RESET_GPIO_NUM;


  config.xclk_freq_hz =
    20000000;


  // =================================================
  // PENTING:
  // Kamera tetap JPEG seperti CCTV yang sudah pernah
  // berhasil kamu pakai.
  // =================================================

  config.pixel_format =
    PIXFORMAT_JPEG;


  if (psramFound()) {

    Serial.println(
      "PSRAM ditemukan"
    );


    config.frame_size =
      FRAMESIZE_VGA;


    config.jpeg_quality =
      10;


    config.fb_count =
      2;

  }

  else {

    Serial.println(
      "PSRAM tidak ditemukan"
    );


    config.frame_size =
      FRAMESIZE_QVGA;


    config.jpeg_quality =
      12;


    config.fb_count =
      1;

  }


  Serial.println(
    "Mulai init kamera..."
  );


  esp_err_t err =

    esp_camera_init(
      &config
    );


  if (err != ESP_OK) {

    Serial.printf(

      "Camera init gagal: 0x%x\n",

      err

    );

    return false;

  }


  Serial.println(
    "Kamera berhasil!"
  );


  return true;
}


// =====================================================
// TFLITE INIT
// =====================================================

bool initPersonDetection()
{

  Serial.println();
  Serial.println(
    "Memulai AI..."
  );


  // -----------------------------------------------
  // Error reporter
  // -----------------------------------------------

  static tflite::MicroErrorReporter
    micro_error_reporter;


  error_reporter =
    &micro_error_reporter;


  // -----------------------------------------------
  // Load model
  // -----------------------------------------------

  model =

    tflite::GetModel(

      g_person_detect_model_data

    );


  if (
    model->version()
    != TFLITE_SCHEMA_VERSION
  ) {

    Serial.println(
      "Versi model tidak cocok!"
    );

    return false;

  }


  // -----------------------------------------------
  // Tensor arena
  // -----------------------------------------------

  tensor_arena =

    (uint8_t*)heap_caps_malloc(

      kTensorArenaSize,

      MALLOC_CAP_INTERNAL |

      MALLOC_CAP_8BIT

    );


  if (
    tensor_arena == nullptr
  ) {

    Serial.println(
      "Tensor arena gagal!"
    );

    return false;

  }


  Serial.println(
    "Tensor arena berhasil!"
  );


  // -----------------------------------------------
  // Resolver
  // -----------------------------------------------

  static
  tflite::MicroMutableOpResolver<5>
    micro_op_resolver;


  micro_op_resolver.AddAveragePool2D();

  micro_op_resolver.AddConv2D();

  micro_op_resolver.AddDepthwiseConv2D();

  micro_op_resolver.AddReshape();

  micro_op_resolver.AddSoftmax();


  // -----------------------------------------------
  // Interpreter
  // -----------------------------------------------

  static tflite::MicroInterpreter
    static_interpreter(

      model,

      micro_op_resolver,

      tensor_arena,

      kTensorArenaSize,

      error_reporter

    );


  interpreter =
    &static_interpreter;


  // -----------------------------------------------
  // Allocate tensors
  // -----------------------------------------------

  TfLiteStatus allocate_status =

    interpreter->AllocateTensors();


  if (
    allocate_status != kTfLiteOk
  ) {

    Serial.println(
      "AllocateTensors gagal!"
    );

    return false;

  }


  // -----------------------------------------------
  // Input
  // -----------------------------------------------

  input =
    interpreter->input(0);


  Serial.println(
    "TensorFlow Lite siap!"
  );


  Serial.print(
    "Input bytes: "
  );


  Serial.println(
    input->bytes
  );


  // -----------------------------------------------
  // Cek input
  // -----------------------------------------------

  if (
    input->bytes
    !=
    kNumCols * kNumRows
  ) {

    Serial.println(
      "PERINGATAN: ukuran input model tidak 96x96!"
    );

  }


  return true;
}


// =====================================================
// INIT RGB BUFFER
// =====================================================

bool initRGBBuffer()
{

  if (!psramFound()) {

    Serial.println(
      "PSRAM diperlukan untuk RGB buffer!"
    );

    return false;

  }


  rgbBuffer =

    (uint8_t*)ps_malloc(
      RGB_BUFFER_SIZE
    );


  if (rgbBuffer == nullptr) {

    Serial.println(
      "RGB buffer gagal dialokasikan!"
    );

    return false;

  }


  Serial.println(
    "RGB buffer berhasil!"
  );


  return true;
}


// =====================================================
// CAPTURE FRAME UNTUK AI
// =====================================================

bool getAIImage()
{

  camera_fb_t *fb = NULL;


  // -----------------------------------------------
  // Lock kamera
  // -----------------------------------------------

  if (

    xSemaphoreTake(

      cameraMutex,

      pdMS_TO_TICKS(1500)

    ) != pdTRUE

  ) {

    Serial.println(
      "AI: gagal lock kamera"
    );

    return false;

  }


  // -----------------------------------------------
  // Capture
  // -----------------------------------------------

  fb =
    esp_camera_fb_get();


  if (!fb) {

    xSemaphoreGive(
      cameraMutex
    );


    Serial.println(
      "AI: capture gagal!"
    );


    return false;

  }


  // -----------------------------------------------
  // Pastikan JPEG
  // -----------------------------------------------

  if (
    fb->format
    !=
    PIXFORMAT_JPEG
  ) {

    esp_camera_fb_return(fb);

    xSemaphoreGive(
      cameraMutex
    );


    Serial.println(
      "AI: frame bukan JPEG!"
    );


    return false;

  }


  // -----------------------------------------------
  // JPEG -> RGB888
  // -----------------------------------------------

  bool converted =

    fmt2rgb888(

      fb->buf,

      fb->len,

      fb->format,

      rgbBuffer

    );


  if (!converted) {

    esp_camera_fb_return(fb);

    xSemaphoreGive(
      cameraMutex
    );


    Serial.println(
      "AI: JPEG -> RGB gagal!"
    );


    return false;

  }


  // Kita sudah selesai menggunakan framebuffer
  esp_camera_fb_return(fb);

  fb = NULL;


  // Kamera boleh dipakai stream lagi
  xSemaphoreGive(
    cameraMutex
  );


  // =================================================
  // RGB888
  //
  // Ambil bagian tengah frame.
  //
  // VGA = 640 x 480
  // Model = 96 x 96
  // =================================================

  const int sourceWidth =
    640;

  const int sourceHeight =
    480;


  const int cropX =
    (sourceWidth - 96) / 2;


  const int cropY =
    (sourceHeight - 96) / 2;


  // -----------------------------------------------
  // RGB -> grayscale -> int8
  // -----------------------------------------------

  for (
    int y = 0;
    y < 96;
    y++
  ) {

    for (
      int x = 0;
      x < 96;
      x++
    ) {


      int srcX =
        cropX + x;


      int srcY =
        cropY + y;


      int index =

        (
          srcY
          *
          sourceWidth
          +
          srcX
        )
        *
        3;


      uint8_t r =
        rgbBuffer[index];


      uint8_t g =
        rgbBuffer[index + 1];


      uint8_t b =
        rgbBuffer[index + 2];


      // Luminance / grayscale
      uint8_t gray =

        (
          (
            (uint16_t)r * 77
          )
          +
          (
            (uint16_t)g * 150
          )
          +
          (
            (uint16_t)b * 29
          )
        )
        >> 8;


      // uint8 -> int8
      input->data.int8[
        y * 96 + x
      ] =

        ((int)gray) - 128;

    }

  }


  return true;
}


// =====================================================
// RUN PERSON DETECTION
// =====================================================

void runPersonDetection()
{

  if (!getAIImage()) {

    return;

  }


  // -----------------------------------------------
  // Invoke model
  // -----------------------------------------------

  TfLiteStatus status =

    interpreter->Invoke();


  if (
    status != kTfLiteOk
  ) {

    Serial.println(
      "Invoke gagal!"
    );

    return;

  }


  // -----------------------------------------------
  // Output
  // -----------------------------------------------

  TfLiteTensor* output =

    interpreter->output(0);


  uint8_t person_raw =

    output->data.uint8[
      kPersonIndex
    ];


  uint8_t no_person_raw =

    output->data.uint8[
      kNotAPersonIndex
    ];


  float p =

    (
      person_raw
      -
      output->params.zero_point
    )
    *
    output->params.scale;


  float np =

    (
      no_person_raw
      -
      output->params.zero_point
    )
    *
    output->params.scale;


  // -----------------------------------------------
  // Simpan status
  // -----------------------------------------------

  personScore =
    p;


  noPersonScore =
    np;


  if (

    p >= PERSON_THRESHOLD
    &&
    p > np

  ) {

    humanDetected =
      true;

  }

  else {

    humanDetected =
      false;

  }


  // -----------------------------------------------
  // Serial monitor
  // -----------------------------------------------

  Serial.print(
    "Person score: "
  );

  Serial.print(
    p,
    3
  );


  Serial.print(
    " | No person: "
  );

  Serial.print(
    np,
    3
  );


  Serial.print(
    " | STATUS: "
  );


  if (humanDetected) {

    Serial.println(
      "MANUSIA TERDETEKSI"
    );

  }

  else {

    Serial.println(
      "TIDAK ADA MANUSIA"
    );

  }

}


// =====================================================
// SETUP
// =====================================================

void setup()
{

  Serial.begin(
    115200
  );


  Serial.setDebugOutput(
    false
  );


  Serial.println();


  Serial.println(
    "=============================="
  );


  Serial.println(
    "   AQUAGUARD CCTV + AI"
  );


  Serial.println(
    "=============================="
  );


  // =================================================
  // PSRAM
  // =================================================

  if (psramFound()) {

    Serial.println(
      "PSRAM ditemukan"
    );

  }

  else {

    Serial.println(
      "PSRAM TIDAK DITEMUKAN"
    );

  }


  // =================================================
  // CAMERA
  // =================================================

  if (!initCamera()) {

    Serial.println(
      "Kamera gagal diinisialisasi."
    );

    while (true) {

      delay(1000);

    }

  }


  // =================================================
  // CAMERA MUTEX
  // =================================================

  cameraMutex =
    xSemaphoreCreateMutex();


  if (cameraMutex == NULL) {

    Serial.println(
      "Camera mutex gagal!"
    );

    while (true) {

      delay(1000);

    }

  }


  // =================================================
  // RGB BUFFER
  // =================================================

  if (!initRGBBuffer()) {

    Serial.println(
      "RGB buffer gagal!"
    );

    while (true) {

      delay(1000);

    }

  }


  // =================================================
  // AI
  // =================================================

  if (!initPersonDetection()) {

    Serial.println(
      "AI gagal diinisialisasi!"
    );

    while (true) {

      delay(1000);

    }

  }


  // =================================================
  // WIFI
  // =================================================

  Serial.println();

  Serial.print(
    "Menghubungkan ke Wi-Fi: "
  );

  Serial.println(
    ssid
  );


  WiFi.mode(
    WIFI_STA
  );


  WiFi.begin(
    ssid,
    password
  );


  int retry = 0;


  while (

    WiFi.status()
    !=
    WL_CONNECTED
    &&
    retry < 30

  ) {

    delay(500);

    Serial.print(".");

    retry++;

  }


  Serial.println();


  if (
    WiFi.status()
    !=
    WL_CONNECTED
  ) {

    Serial.println(
      "Wi-Fi GAGAL TERHUBUNG!"
    );


    Serial.print(
      "Status Wi-Fi: "
    );


    Serial.println(
      WiFi.status()
    );


    while (true) {

      delay(1000);

    }

  }


  Serial.println(
    "Wi-Fi TERHUBUNG!"
  );


  Serial.print(
    "IP ESP32-CAM: "
  );


  Serial.println(
    WiFi.localIP()
  );


  Serial.print(
    "RSSI: "
  );


  Serial.print(
    WiFi.RSSI()
  );


  Serial.println(
    " dBm"
  );


  // =================================================
  // WEB SERVER
  // =================================================

  startCameraServer();


  Serial.println();


  Serial.println(
    "=============================="
  );


  Serial.println(
    "       CCTV + AI SIAP!"
  );


  Serial.println(
    "=============================="
  );


  Serial.print(
    "Buka HP: http://"
  );


  Serial.println(
    WiFi.localIP()
  );


  Serial.println();

}


// =====================================================
// LOOP
// =====================================================

void loop()
{

  static unsigned long
    lastDetection = 0;


  unsigned long now =
    millis();


  // AI setiap 2 detik
  if (
    now - lastDetection
    >=
    2000
  ) {

    lastDetection =
      now;


    runPersonDetection();

  }


  // Hindari watchdog
  delay(10);

}