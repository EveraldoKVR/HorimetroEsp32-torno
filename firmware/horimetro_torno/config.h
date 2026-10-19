#pragma once

// ============================================================
//  CONFIGURAÇÕES DO HORÍMETRO
// ============================================================

// --- Wi-Fi (configurado pelo WiFiManager, somente rede 2,4 GHz) ---
// conecte-se a ela pelo celular e escolha a rede da oficina na página que abrir.
#define PORTAL_AP_NAME       "Horimetro-Setup" //SSID para configuração da rede de Wi-Fi (parecido com a de um roteador)
#define PORTAL_AP_PASS       "horimetro123"   // Senha para acessar a configuração do Wi-Fi
#define PORTAL_TIMEOUT_S     180              // O portal fecha sozinho após este tempo (3 minutos)
#define PIN_CONFIG_BTN       0                // Botão BOOT da placa
#define CONFIG_BTN_HOLD_MS   5000             // Segure o botão por 5 s para trocar de rede Wi-Fi

// --- Firebase ---
#define FIREBASE_API_KEY     "###"  // API Key do projeto Firebase
#define FIREBASE_PROJECT_ID  "###"  // ID do projeto Firebase

// Definição do usuário e senha que o ESP32 usará para acessar o firebase
#define DEVICE_EMAIL     "###"  // Email do dispositivo
#define DEVICE_PASSWORD  "###"  // Senha do dispositivo

#define DEVICE_ID        "###"  // ID do dispositivo (gerado pelo Firebase)


//Definição dos pinos usados na placa para fazer as leituras
#define PIN_MOTOR_FWD    23   // sentido horário (ou único ponto de leitura)
#define PIN_MOTOR_REV    34   // sentido anti-horário (se não usar, não ligue nada)
#define PIN_LED          2    // LED da placa: aceso enquanto o motor está ligado

// --- Ajustes de detecção ---
// Com optoacoplador alimentado em corrente alternada, a saída pulsa a 100/120 Hz.
// O sinal é considerado "ativo" se houve pulso nesta janela:
#define ACTIVE_HOLD_MS       60
// Tempo que o sinal precisa ficar estável para confirmar partida / parada.
// Paradas menores que STOP_DEBOUNCE_MS (ex.: inversão de sentido) não encerram a sessão.
#define START_DEBOUNCE_MS    1000
#define STOP_DEBOUNCE_MS     2000
// Sessões menores que isso (segundos) são descartadas.
#define MIN_SESSION_S        5

// --- Comunicação ---

#define HEARTBEAT_MS         30000   // tempo do pulso de gravação (30s)
#define CHECKPOINT_MS        60000   // grava progresso da sessão na memória (protege contra queda de energia)
#define PENDING_FILE         "/pendentes.txt"
#define FW_VERSION           "1.0.0-firebase"
