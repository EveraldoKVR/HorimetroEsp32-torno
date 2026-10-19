# Horímetro digital IoT — versão 100% Firebase

Versão educacional do horímetro do torno Nardini Mascote MS175 que usa **somente o plano gratuito (Spark) do Firebase**, sem servidor próprio e sem Cloud Functions.

```
Motor ──► Optoacoplador ──► ESP32 ──HTTPS──► Firestore ◄──── Painel web (Firebase Hosting)
                             │   (REST + login por e-mail)      (leitura em tempo real)
                             └ autentica como "dispositivo"     └ login do grupo/professor
```

- **ESP32** faz login no Firebase Authentication e grava em `status/live` (estado ao vivo) e `sessions/{id}` (cada sessão).
- **Painel** (HTML + JavaScript, hospedado no Firebase Hosting) lê o Firestore em tempo real e calcula totais e alertas no navegador.
- **Firestore Rules** garantem que só o ESP32 grava sessões e só o grupo lê o painel e edita o plano de manutenção.

## Estrutura

```
horimetro-torno-firebase/
├── firmware/horimetro_torno/    ← Arduino IDE
│   ├── horimetro_torno.ino
│   └── config.h                 ← dados do Firebase + usuário do ESP32 + ajustes do portal Wi-Fi
├── public/                      ← painel (Firebase Hosting)
│   ├── index.html
│   ├── app.js
│   └── firebase-config.js       ← cole aqui a config do app web
├── firestore.rules              ← regras de segurança (edite os e-mails)
├── firebase.json
└── .firebaserc                  ← coloque o ID do seu projeto
```

## Passo a passo

### 1. Criar o projeto
1. Acesse https://console.firebase.google.com → **Adicionar projeto** (pode desativar o Google Analytics).
2. Anote o **ID do projeto** (ex.: `horimetro-torno-1a2b3`).

### 2. Authentication
1. **Build → Authentication → Vamos começar → E-mail/senha → Ativar**.
2. Aba **Usuários → Adicionar usuário**, crie:
   - `dispositivo@horimetro.com` (senha forte): usado só pelo ESP32.
   - Um usuário para o painel, ex.: `grupo@horimetro.com` (crie mais se quiser).

   Os e-mails podem ser fictícios, mas **precisam ser exatamente os mesmos** do `firestore.rules` e do `config.h`.

### 3. Firestore
**Build → Firestore Database → Criar banco de dados** → modo **produção** → região `southamerica-east1` (São Paulo).

### 4. Config do painel
1. **Configurações do projeto (engrenagem) → Geral → Seus apps → `</>` (Web)** → registre o app (não precisa marcar Hosting).
2. Copie os valores de `firebaseConfig` para `public/firebase-config.js`.

### 5. Regras de segurança
1. Em `firestore.rules`, ajuste os e-mails de `isDevice()` e `isDashboardUser()`.
2. Publique: copie o conteúdo na aba **Firestore → Regras → Publicar**, **ou** use a CLI (passo 6).

> Por que listar e-mails? A chave de API do Firebase é pública (vai no site) e qualquer pessoa conseguiria criar uma conta. As regras liberam apenas os e-mails listados.

### 6. Publicar o painel (Firebase Hosting)
```bash
npm install -g firebase-tools
firebase login
# edite .firebaserc com o ID do projeto
firebase deploy --only hosting,firestore:rules
```
O endereço será `https://SEU-PROJECT-ID.web.app`.

Para testar localmente antes: `npx serve public` e abra `http://localhost:3000` (o domínio `localhost` já é autorizado no Firebase Authentication).

### 7. Firmware do ESP32
1. Arduino IDE: pacote de placas **esp32** (Espressif) + bibliotecas **ArduinoJson 7.x** (Benoit Blanchon) e **WiFiManager** (tzapu).
2. Edite `config.h`: `FIREBASE_API_KEY`, `FIREBASE_PROJECT_ID`, `DEVICE_EMAIL` e `DEVICE_PASSWORD`. O Wi-Fi **não** fica mais no código.
3. Placa **ESP32 Dev Module** → Upload → Monitor Serial (115200).
4. **Configurar o Wi-Fi (primeira vez):** o Monitor Serial mostra `[wifi] Portal aberto` e o LED da placa pisca rápido. No celular, conecte-se à rede **Horimetro-Setup** (senha `horimetro123`, definida no `config.h`). A página de configuração abre sozinha (ou acesse `192.168.4.1`). Toque em **Configure WiFi**, escolha a rede da oficina (2,4 GHz), digite a senha e salve. Deve aparecer `[wifi] Conectado a "..."` e depois `[auth] dispositivo autenticado`.
5. Ao ligar o torno, confira no Firestore as coleções `status` e `sessions`.

**Como o Wi-Fi funciona agora**
- A rede escolhida fica salva na memória do ESP32 e é usada nas próximas partidas.
- Se a rede salva não for encontrada na partida (ex.: roteador desligado), o portal abre por 3 minutos e depois fecha sozinho. O horímetro **continua contando** e guarda as sessões na fila, enviando quando a rede voltar.
- **Trocar de rede:** segure o botão **BOOT** da placa por 5 s. O LED pisca e o portal abre de novo.
- O portal roda na tarefa de rede, então **não interrompe a contagem de horas**.

### 8. Primeiro acesso ao painel
Entre com o usuário do grupo. Na primeira vez o painel cria 4 tarefas de **exemplo** (8, 100, 500 e 1000 h). **São valores ilustrativos**: ajuste cada uma conforme o manual da Nardini MS175 (botão *Editar*).

## Problemas comuns com o Wi-Fi

| Sintoma | Causa provável |
|---|---|
| A rede `Horimetro-Setup` não aparece | Portal já fechou (3 min). Reinicie a placa ou segure BOOT por 5 s |
| Página de configuração não abre sozinha | Acesse `http://192.168.4.1` no navegador do celular |
| Não conecta após salvar | Rede de 5 GHz ou senha digitada errada: repita com BOOT por 5 s |
| Mudou a senha do roteador | Segure BOOT por 5 s e configure de novo |

## Ligação elétrica

**Atenção:** a leitura envolve a tensão do motor (127/220/380 V). A instalação deve ser feita ou revisada por eletricista/técnico qualificado, com a máquina desenergizada, e o ESP32 nunca deve ser ligado direto à rede.

## Limites do plano gratuito e custo de uso

| Recurso | Limite gratuito (Spark) | Uso deste projeto |
|---|---|---|
| Escritas no Firestore | 20.000/dia | até ~2.900/dia de status + 1 por sessão |
| Leituras no Firestore | 50.000/dia | 1 por atualização de status por painel aberto (~2.900/dia) + sessões lidas ao abrir |
| Armazenamento | 1 GiB | desprezível |

Observações:
- Os limites podem mudar; confira em https://firebase.google.com/pricing.
- O painel lê **todas** as sessões ao abrir (suficiente para uso educacional, com centenas ou poucos milhares de sessões).
- Se o limite diário for excedido, o Firebase bloqueia novas operações até o dia seguinte (no plano Spark não há cobrança).

## Diferenças em relação à versão com servidor (Node.js)

- Não há servidor nem banco SQLite: tudo roda no Firebase.
- Os cálculos (totais, hoje/semana/mês, status das tarefas) são feitos no navegador.
- A segurança depende das **regras do Firestore**; sem Cloud Functions, o ESP32 grava direto no banco com a própria conta.
- O ESP32 usa TLS sem validar o certificado (`setInsecure()`), aceitável para fins educacionais. Para produção, use `setCACert()`.
- Se o relógio do ESP32 não sincronizar (NTP), a sessão usa o horário do servidor menos a duração.

## Roteiro de testes

Use o roteiro (detecção, precisão, inversão de sentido, perda de rede, queda de energia, alertas e usabilidade). Para o teste de perda de rede, desligue o Wi-Fi durante uma sessão e confira no Firestore que o documento em `sessions` aparece quando a rede volta.
