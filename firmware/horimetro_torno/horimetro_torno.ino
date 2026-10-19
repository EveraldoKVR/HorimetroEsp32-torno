/*
 * Horímetro digital para torno mecânico (Nardini Mascote MS175) — ESP32
 * Projeto Integrador 4 UNIVESP
 *
 * Funcionamento:
 *  1. Lê se o motor do torno está energizado, em qualquer sentido.
 *  2. Conta o tempo de cada "sessão" de uso (partida -> parada).
 *  3. Acumula o total em memória não volátil (NVS), sobrevivendo a quedas de energia.
 *  4. Grava direto no Firebase Firestore (API REST, plano gratuito Spark):
 *       - status/live          -> estado ao vivo, a cada HEARTBEAT_MS
 *       - sessions/{uid}       -> cada sessão concluída (ID único evita duplicidade)
 *     O ESP32 se autentica com e-mail/senha (Firebase Authentication) e as regras do
 *     Firestore só permitem que esse usuário grave nessas coleções.
 *     Se estiver sem Wi-Fi, as sessões ficam numa fila em arquivo (LittleFS) e são enviadas depois.
 *  5. Wi-Fi: na primeira vez (ou se a rede salva não for encontrada na partida) o ESP32 abre a rede
 *     Horimetro-Setup. É só conectar nela pelo celular e escolha a rede da oficina. 
 */

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>      
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <time.h>
#include "config.h"

// ---------------- Estado do sensor (usado só no loop(), núcleo 1) ----------------
Preferences prefs;
bool     motorOn          = false;   // estado confirmado (após debounce)
bool     candidate        = false;   // há uma mudança pendente de confirmação
uint32_t candidateSince   = 0;
uint32_t lastActiveMs     = 0;
uint32_t sessionStartMs   = 0;
uint32_t sessionStartEpoch = 0;      // 0 = relógio ainda não sincronizado
uint32_t lastCheckpointMs = 0;
uint32_t totalSeconds     = 0;       // total já fechado (sessões concluídas)

// ---------------- Compartilhado com a tarefa de rede ----------------
volatile bool     g_running        = false;
volatile uint32_t g_currentSessionS = 0;
volatile uint32_t g_totalS         = 0;   // total fechado
volatile bool     g_pending        = false;
volatile bool     g_heartbeatNow   = false;
volatile bool     g_openPortal     = false;   // pedido (botão BOOT) para abrir o portal de Wi-Fi
volatile bool     g_portalActive   = false;   // portal aberto (pisca o LED)
SemaphoreHandle_t fsMutex;

// ============================================================
//  Utilidades
// ============================================================
uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (uint32_t)t : 0;   // 0 se o NTP ainda não sincronizou
}

// ============================================================
//  Fila de sessões pendentes (arquivo no LittleFS)
// ============================================================
void enqueueSession(uint32_t startEpoch, uint32_t durationS) {
  char uid[20];
  snprintf(uid, sizeof(uid), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());

  xSemaphoreTake(fsMutex, portMAX_DELAY);
  File f = LittleFS.open(PENDING_FILE, FILE_APPEND);
  if (f) {
    f.printf("%s,%lu,%lu\n", uid, (unsigned long)startEpoch, (unsigned long)durationS);
    f.close();
  }
  xSemaphoreGive(fsMutex);
  g_pending = true;
}

std::vector<String> readPendingLines() {
  std::vector<String> lines;
  File f = LittleFS.open(PENDING_FILE, FILE_READ);
  if (!f) return lines;
  while (f.available() && lines.size() < 500) {
    String l = f.readStringUntil('\n');
    l.trim();
    if (l.length()) lines.push_back(l);
  }
  f.close();
  return lines;
}

// ============================================================
//  Rede
// ============================================================
// Token de autenticação (usado somente pela tarefa de rede)
String   g_idToken;
bool     g_haveToken     = false;
uint32_t g_tokenExpireMs = 0;

String docPath(const String& rel) {
  return String("projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents/" + rel;
}

// Login do dispositivo (Firebase Authentication, REST).

bool signIn() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" FIREBASE_API_KEY);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["email"] = DEVICE_EMAIL;
  req["password"] = DEVICE_PASSWORD;
  req["returnSecureToken"] = true;
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[auth] falha no login (HTTP %d). Confira API key, e-mail e senha.\n", code);
    http.end();
    return false;
  }
  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, http.getString());
  http.end();
  if (err) return false;

  g_idToken = resp["idToken"].as<String>();
  int expiresIn = atoi(resp["expiresIn"] | "3600");
  g_tokenExpireMs = millis() + (uint32_t)(expiresIn > 120 ? expiresIn - 120 : expiresIn) * 1000UL;
  g_haveToken = true;
  Serial.println("[auth] dispositivo autenticado");
  return true;
}

// Envia um lote de escritas ao Firestore. Retorna o código HTTP (200 = ok), ou <0 em falha de rede/login.
int firestoreCommit(JsonDocument& body) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (!g_haveToken || (int32_t)(millis() - g_tokenExpireMs) >= 0) {
    if (!signIn()) return -2;
  }

  String payload;
  serializeJson(body, payload);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID +
                     "/databases/(default)/documents:commit");
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + g_idToken);
  int code = http.POST(payload);
  if (code != 200 && code > 0) {
    String resp = http.getString();
    Serial.printf("[firestore] HTTP %d: %s\n", code, resp.substring(0, 200).c_str());
  }
  http.end();
  if (code == 401) g_haveToken = false;   // token expirado/inválido: refaz o login na próxima
  return code;
}

void sendHeartbeat() {
  JsonDocument body;
  JsonObject w = body["writes"].add<JsonObject>();
  JsonObject upd = w["update"].to<JsonObject>();
  upd["name"] = docPath("status/live");
  JsonObject f = upd["fields"].to<JsonObject>();
  f["device_id"]["stringValue"] = DEVICE_ID;
  f["running"]["booleanValue"]  = (bool)g_running;
  f["current_s"]["integerValue"] = String((uint32_t)g_currentSessionS);
  f["total_s"]["integerValue"]   = String((uint32_t)g_totalS + (uint32_t)g_currentSessionS);
  f["rssi"]["integerValue"]      = String(WiFi.RSSI());
  f["uptime_s"]["integerValue"]  = String((uint32_t)(millis() / 1000));
  f["fw"]["stringValue"]         = FW_VERSION;
  JsonObject tr = w["updateTransforms"].add<JsonObject>();
  tr["fieldPath"] = "updated_at";
  tr["setToServerValue"] = "REQUEST_TIME";   // horário do servidor (não depende do NTP do ESP32)
  firestoreCommit(body);
}

void flushPending() {
  if (!g_pending) return;

  xSemaphoreTake(fsMutex, portMAX_DELAY);
  std::vector<String> lines = readPendingLines();
  xSemaphoreGive(fsMutex);

  if (lines.empty()) { g_pending = false; return; }

  size_t done = 0;   // quantas linhas (do início) foram resolvidas
  for (; done < lines.size(); done++) {
    const String& l = lines[done];
    int c1 = l.indexOf(',');
    int c2 = l.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) continue;           // linha corrompida: descarta

    String uid = l.substring(0, c1);
    JsonDocument body;
    JsonObject w = body["writes"].add<JsonObject>();
    JsonObject upd = w["update"].to<JsonObject>();
    upd["name"] = docPath("sessions/" + uid);      // ID = uid -> reenvio apenas sobrescreve (sem duplicar)
    JsonObject f = upd["fields"].to<JsonObject>();
    f["device_id"]["stringValue"]  = DEVICE_ID;
    f["start_ts"]["integerValue"]  = String(l.substring(c1 + 1, c2).toInt());
    f["duration_s"]["integerValue"] = String(l.substring(c2 + 1).toInt());
    JsonObject tr = w["updateTransforms"].add<JsonObject>();
    tr["fieldPath"] = "created_at";
    tr["setToServerValue"] = "REQUEST_TIME";

    int code = firestoreCommit(body);
    if (code >= 200 && code < 300) continue;  // enviada
    if (code == 400) continue;                // rejeitada como inválida: descarta (evita fila travada)
    break;                                    // sem rede / erro temporário: tenta depois
  }

  if (done == 0) return;

  // Remove do arquivo apenas as linhas já resolvidas (o loop pode ter acrescentado novas).
  xSemaphoreTake(fsMutex, portMAX_DELAY);
  std::vector<String> current = readPendingLines();
  File f = LittleFS.open(PENDING_FILE, FILE_WRITE);
  if (f) {
    for (size_t i = done; i < current.size(); i++) f.println(current[i]);
    f.close();
    g_pending = (current.size() > done);
  }
  xSemaphoreGive(fsMutex);
}

// Conecta ao Wi-Fi usando o WiFiManager.
//  - forcePortal = false: tenta a rede salva; se não conseguir (ou se não houver), abre o portal.
//  - forcePortal = true : abre o portal direto (usado pelo botão BOOT, para trocar de rede).
void connectWifi(bool forcePortal) {
  WiFiManager wm;
  wm.setConnectTimeout(20);                    // segundos tentando a rede salva
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S); // portal fecha sozinho depois desse tempo
  wm.setAPCallback([](WiFiManager*) {
    g_portalActive = true;
    Serial.printf("[wifi] Portal aberto. Conecte-se a rede \"%s\" e acesse 192.168.4.1\n", PORTAL_AP_NAME);
  });

  bool ok = forcePortal ? wm.startConfigPortal(PORTAL_AP_NAME, PORTAL_AP_PASS)
                        : wm.autoConnect(PORTAL_AP_NAME, PORTAL_AP_PASS);
  g_portalActive = false;
  WiFi.mode(WIFI_STA);

  if (ok) Serial.printf("[wifi] Conectado a \"%s\" (%s)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  else    Serial.println("[wifi] Sem Wi-Fi: o horimetro segue contando e envia quando a rede voltar");
}

void networkTask(void*) {
  uint32_t lastHb = 0, lastWifiTry = 0, lastFlush = 0;

  connectWifi(false);

  for (;;) {
    uint32_t now = millis();

    if (g_openPortal) {                         // botão BOOT segurado por 5 s
      g_openPortal = false;
      connectWifi(true);
      g_heartbeatNow = true;
    }

    if (WiFi.status() != WL_CONNECTED) {
      // Queda de rede: tenta reconectar com a rede já salva (não abre o portal)
      if (now - lastWifiTry > 15000) {
        lastWifiTry = now;
        WiFi.reconnect();
      }
    } else {
      if (g_heartbeatNow || now - lastHb >= HEARTBEAT_MS) {
        g_heartbeatNow = false;
        lastHb = now;
        sendHeartbeat();
      }
      if (g_pending && now - lastFlush >= 3000) {
        lastFlush = now;
        flushPending();
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ============================================================
//  Sessões de uso
// ============================================================
void onMotorStart(uint32_t edgeMs) {
  sessionStartMs = edgeMs;
  uint32_t ep = nowEpoch();
  sessionStartEpoch = ep ? ep - (millis() - edgeMs) / 1000 : 0;
  lastCheckpointMs = millis();

  prefs.putBool("inS", true);
  prefs.putUInt("sStart", sessionStartEpoch);
  prefs.putUInt("sDur", 0);

  g_currentSessionS = 0;
  g_running = true;
  g_heartbeatNow = true;
  Serial.println("[motor] LIGADO");
}

void onMotorStop(uint32_t edgeMs) {
  uint32_t dur = (edgeMs - sessionStartMs) / 1000;
  g_running = false;
  g_currentSessionS = 0;
  prefs.putBool("inS", false);

  if (dur >= MIN_SESSION_S) {
    totalSeconds += dur;
    g_totalS = totalSeconds;
    prefs.putUInt("total", totalSeconds);
    enqueueSession(sessionStartEpoch, dur);
    Serial.printf("[motor] DESLIGADO — sessão de %lu s (total %.2f h)\n",
                  (unsigned long)dur, totalSeconds / 3600.0);
  } else {
    Serial.println("[motor] DESLIGADO — sessão curta descartada");
  }
  g_heartbeatNow = true;
}

// Lê o sinal e aplica janela de atividade (compatível com entrada em corrente alternada).
bool readMotorSignal() {
  uint32_t now = millis();
  if (digitalRead(PIN_MOTOR_FWD) == LOW || digitalRead(PIN_MOTOR_REV) == LOW) lastActiveMs = now;
  return (uint32_t)(now - lastActiveMs) < ACTIVE_HOLD_MS;
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_MOTOR_FWD, INPUT_PULLUP);
  pinMode(PIN_MOTOR_REV, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_CONFIG_BTN, INPUT_PULLUP);
  lastActiveMs = millis() - ACTIVE_HOLD_MS - 1;

  fsMutex = xSemaphoreCreateMutex();
  if (!LittleFS.begin(true)) Serial.println("Falha ao montar LittleFS");

  prefs.begin("horimetro", false);
  totalSeconds = prefs.getUInt("total", 0);

  // Recupera sessão interrompida por queda de energia
  if (prefs.getBool("inS", false)) {
    uint32_t dur = prefs.getUInt("sDur", 0);
    uint32_t st  = prefs.getUInt("sStart", 0);
    prefs.putBool("inS", false);
    if (dur >= MIN_SESSION_S) {
      totalSeconds += dur;
      prefs.putUInt("total", totalSeconds);
      enqueueSession(st, dur);
      Serial.printf("Sessão recuperada após reinício: %lu s\n", (unsigned long)dur);
    }
  }
  g_totalS = totalSeconds;
  g_pending = !readPendingLines().empty();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");   // Brasília (UTC-3)

  xTaskCreatePinnedToCore(networkTask, "rede", 20480, nullptr, 1, nullptr, 0);

  Serial.printf("Horímetro iniciado. Total acumulado: %.2f h\n", totalSeconds / 3600.0);
}

void loop() {
  uint32_t now = millis();
  bool raw = readMotorSignal();

  if (raw != motorOn) {
    if (!candidate) {
      candidate = true;
      candidateSince = now;
    } else if (now - candidateSince >= (raw ? START_DEBOUNCE_MS : STOP_DEBOUNCE_MS)) {
      motorOn = raw;
      candidate = false;
      if (motorOn) onMotorStart(candidateSince);
      else         onMotorStop(candidateSince);
    }
  } else {
    candidate = false;
  }

  if (motorOn) {
    g_currentSessionS = (now - sessionStartMs) / 1000;
    if (now - lastCheckpointMs >= CHECKPOINT_MS) {
      lastCheckpointMs = now;
      prefs.putUInt("sDur", g_currentSessionS);
    }
  }

  // Botão BOOT segurado por CONFIG_BTN_HOLD_MS -> abre o portal de configuração do Wi-Fi
  static uint32_t btnSince = 0;
  static bool btnFired = false;
  if (digitalRead(PIN_CONFIG_BTN) == LOW) {
    if (btnSince == 0) btnSince = now;
    if (!btnFired && !g_portalActive && now - btnSince >= CONFIG_BTN_HOLD_MS) {
      btnFired = true;
      g_openPortal = true;
      Serial.println("[wifi] Botao segurado: abrindo portal de configuracao");
    }
  } else {
    btnSince = 0;
    btnFired = false;
  }

  // LED: aceso com o motor ligado; pisca rápido enquanto o portal de Wi-Fi estiver aberto
  if (g_portalActive) digitalWrite(PIN_LED, ((now / 250) & 1) ? HIGH : LOW);
  else                digitalWrite(PIN_LED, motorOn ? HIGH : LOW);
  delay(1);
}
