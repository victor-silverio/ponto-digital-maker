#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// Botao de Cadastro (Botao FLASH do ESP ou botao externo no pino D3)
const int PINO_BOTAO   = 0;  // GPIO0 = D3
const int PINO_BUZZER  = 15; // GPIO15 = D8 (Puxado para GND pelo buzzer, ideal para o boot)

// LCD I2C (Endereco 0x27, 20 colunas, 4 linhas)
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Sensor de Digital: RX=D5(GPIO14), TX=D6(GPIO12)
SoftwareSerial mySerial(14, 12);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// Comunicacao com a Slave RFID: RX=D7(GPIO13), TX=D0(GPIO16)
SoftwareSerial slaveSerial(13, 16);

// Arquivos LittleFS
const char* ARQUIVO_PESSOAS   = "/pessoas.txt";
const char* ARQUIVO_STATUS    = "/status.txt";
const char* ARQUIVO_PENDENTES = "/pendentes.txt";
const char* ARQUIVO_CARTOES   = "/cartoes.txt";  // Mapa UID -> ID
const char* ARQUIVO_TEMP      = "/temp.txt";

// WiFi e Planilha (Configure com os dados da sua rede e do seu Google Apps Script)
const char* WIFI_SSID     = "SUA_REDE_WIFI";
const char* WIFI_PASSWORD = "SUA_SENHA_WIFI";
const char* WEBAPP_URL    = "https://script.google.com/macros/s/SEU_SCRIPT_ID_AQUI/exec"; // Veja instrucoes na pasta cloud/

bool atualizarTela = true;
unsigned long ultimaTentativaPendentes = 0;
const unsigned long INTERVALO_TENTATIVA_PENDENTES = 30000;
unsigned long ultimoRelogio = 0;
int ultimoDiaFechamento = -1;

// Maximo de tentativas de reenvio de um pendente antes de descartar
const int MAX_TENTATIVAS_PENDENTE = 3;

// Watchdog do sensor biometrico
int errosConsecutivosSensor = 0;
const int MAX_ERROS_SENSOR = 8;           // erros seguidos antes de reinicializar
unsigned long ultimaVerificacaoSensor = 0;
const unsigned long INTERVALO_SAUDE_SENSOR = 7200000UL; // verificacao preventiva a cada 2h

// Reconexao WiFi automatica
unsigned long ultimaTentativaWiFi = 0;
const unsigned long INTERVALO_TENTATIVA_WIFI = 30000UL; // tenta reconectar a cada 30s

// Sincronizacao NTP periodica (a cada 6h)
unsigned long ultimaSincNTP = 0;
const unsigned long INTERVALO_SINC_NTP = 21600000UL;

// Cooldown anti-duplo-registro (5s por ID)
int    ultimoIDRegistrado   = -1;
unsigned long ultimoRegistroMs = 0;
const unsigned long COOLDOWN_REGISTRO_MS = 5000UL;


// ====================================================================
// FUNCOES AUXILIARES DE FORMATACAO PARA O LCD 20x4
// ====================================================================

// Garante que a linha tenha exatamente 'largura' caracteres (trunca ou completa com espacos)
String formatarLinha(String s, int largura = 20) {
  if ((int)s.length() > largura) {
    return s.substring(0, largura);
  }
  while ((int)s.length() < largura) {
    s += ' ';
  }
  return s;
}

// Centraliza o texto na linha de 20 colunas
String formatarCentro(String s, int largura = 20) {
  if ((int)s.length() >= largura) {
    return s.substring(0, largura);
  }
  int espacos = largura - s.length();
  int esquerda = espacos / 2;
  int direita = espacos - esquerda;
  String res = "";
  for (int i = 0; i < esquerda; i++) res += ' ';
  res += s;
  for (int i = 0; i < direita; i++) res += ' ';
  return res;
}

// Formata data e hora para a linha superior do LCD 20x4
String getLinhaDataHora() {
  time_t agora = time(nullptr);
  struct tm *tmInfo = localtime(&agora);
  if (tmInfo->tm_year + 1900 < 2024) {
    return "--/--/----    --:-- ";
  }
  char buf[21];
  sprintf(buf, "%02d/%02d/%04d    %02d:%02d",
          tmInfo->tm_mday, tmInfo->tm_mon + 1, tmInfo->tm_year + 1900,
          tmInfo->tm_hour, tmInfo->tm_min);
  return String(buf);
}

// ====================================================================
// ESCREVE NO LCD PARANDO O SOFTWARESERIAL TEMPORARIAMENTE
// Suporta ate 4 linhas aproveitando todo o display 20x4.
// ====================================================================
void escreverLCD(String linha1, String linha2 = "", String linha3 = "", String linha4 = "") {
  static bool lcdInicializado = false;

  pinMode(12, OUTPUT);
  digitalWrite(12, HIGH);
  mySerial.end();
  delay(50);

  if (!lcdInicializado) {
    Wire.begin(4, 5);
    Wire.setClock(100000);
    delay(10);
    lcd.init();
    lcd.backlight();
    delay(10);
    lcdInicializado = true;
  }

  lcd.setCursor(0, 0); lcd.print(formatarLinha(linha1));
  lcd.setCursor(0, 1); lcd.print(formatarLinha(linha2));
  lcd.setCursor(0, 2); lcd.print(formatarLinha(linha3));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(linha4));

  mySerial.begin(57600);
  delay(100);
}

// ====================================================================
// REINICIALIZA O SENSOR BIOMETRICO (usado pelo watchdog)
// 1) Envia pacote de soft-reset (cmd 0x0E) ao DSP do sensor via UART.
// 2) Reinicia o SoftwareSerial do lado do ESP8266.
// 3) Verifica comunicacao com verifyPassword().
// ====================================================================

// Envia o pacote de soft-reset do protocolo FPM10A/DY50 diretamente
// pelo SoftwareSerial, sem depender da biblioteca Adafruit.
// Estrutura: EF01 FFFFFFFF 01 0003 0E 0012
//            header  addr  pkg_id  len  cmd  checksum
void enviarSoftResetSensor() {
  const uint8_t pacote[] = {
    0xEF, 0x01,              // cabecalho
    0xFF, 0xFF, 0xFF, 0xFF,  // endereco (broadcast)
    0x01,                    // tipo: comando
    0x00, 0x03,              // tamanho do dado: 3 bytes
    0x0E,                    // comando: Soft Reset
    0x00, 0x12               // checksum (0x01+0x00+0x03+0x0E = 0x12)
  };
  mySerial.begin(57600);
  delay(20);
  for (uint8_t i = 0; i < sizeof(pacote); i++) {
    mySerial.write(pacote[i]);
  }
  mySerial.flush();
  delay(200); // aguarda o DSP reiniciar (~100-200ms segundo datasheet)
}

// ====================================================================
// ========================= BUZZER ===================================
// ====================================================================

void bip(int duracao) {
  digitalWrite(PINO_BUZZER, HIGH);
  delay(duracao);
  digitalWrite(PINO_BUZZER, LOW);
}

void bipEntrada() {
  // 2 bips curtos: ENTRADA
  bip(100); delay(80); bip(100);
}

void bipSaida() {
  // 1 bip medio: SAIDA
  bip(220);
}

void bipNegado() {
  // 1 bip longo: ACESSO NEGADO
  bip(700);
}

void bipBoot() {
  // 3 bips rapidos: sistema pronto
  bip(70); delay(55); bip(70); delay(55); bip(70);
}

void reinicializarSensor() {
  Serial.println(F("\n[WATCHDOG] Reinicializando sensor biometrico..."));
  escreverLCD("  PONTO ELETRONICO  ", "Reiniciando sensor..", "Aguarde um momento..", "");

  // Tenta soft-reset via protocolo UART antes de tudo
  Serial.println(F("[WATCHDOG] Enviando soft-reset ao DSP do sensor..."));
  enviarSoftResetSensor();

  // Reinicia o SoftwareSerial do lado da ESP
  mySerial.end();
  delay(300);
  mySerial.begin(57600);
  finger.begin(57600);
  delay(300);

  if (finger.verifyPassword()) {
    Serial.println(F("[WATCHDOG] Sensor recuperado com sucesso!"));
    errosConsecutivosSensor = 0;
  } else {
    Serial.println(F("[WATCHDOG] ATENCAO: sensor nao respondeu apos reinicializacao!"));
  }

  ultimaVerificacaoSensor = millis();
  atualizarTela = true;
}

// ====================================================================

void setup() {
  Serial.begin(9600);
  slaveSerial.begin(9600); // Comunicacao com a Slave RFID
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // LED desligado
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_BUZZER, OUTPUT);
  digitalWrite(PINO_BUZZER, LOW);  // Buzzer desligado

  // Inicia I2C e LCD 20x4
  Wire.begin(4, 5);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("PONTO ELETRONICO"));
  lcd.setCursor(0, 1); lcd.print(formatarCentro("Inicializando..."));
  lcd.setCursor(0, 2); lcd.print(formatarCentro("MakerSpace UNIFEI"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

  iniciarArquivos();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("PONTO ELETRONICO"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Conectando WiFi..."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha(String("Rede: ") + WIFI_SSID));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Aguarde..."));
  conectarWiFi();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("PONTO ELETRONICO"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Sincronizando hora.."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Servidor NTP (BR)"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Aguarde..."));
  sincronizarHora();
  ultimaSincNTP = millis(); // inicia o temporizador de ressincronizacao NTP

  if (WiFi.status() == WL_CONNECTED) {
    processarPendentes();
  }

  // Inicia sensor biometrico
  finger.begin(57600);
  delay(500);

  if (finger.verifyPassword()) {
    Serial.println("Sensor biometrico OK!");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("PONTO ELETRONICO"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("Biometria: OK"));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("WiFi: OK"));
    lcd.setCursor(0, 3); lcd.print(formatarCentro("Sistema Pronto!"));
    delay(1500);
    bipBoot(); // 3 bips: sistema iniciado com sucesso
    imprimirMenuSerial();
  } else {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("PONTO ELETRONICO"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("ERRO NO SENSOR!"));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("Biometria nao resp."));
    lcd.setCursor(0, 3); lcd.print(formatarCentro("Verifique a fiacao"));
    Serial.println("ERRO: Sensor biometrico nao responde!");
    while (1) { delay(1); }
  }
}

void loop() {
  time_t agora = time(nullptr);
  struct tm *tmStruct = localtime(&agora);

  // 0. FECHAMENTO AUTOMATICO AS 23:00
  if (tmStruct->tm_year + 1900 > 2023 && tmStruct->tm_hour >= 23) {
    if (ultimoDiaFechamento != tmStruct->tm_mday) {
      Serial.println("\n[SISTEMA] Iniciando fechamento automatico das 23h...");
      escreverLCD("  FECHAMENTO 23H    ", "Encerrando turnos...", "Aguarde envio...", "");
      forcarSaidaAutomatica();
      ultimoDiaFechamento = tmStruct->tm_mday;
      atualizarTela = true;
    }
  }

  // 1. REENVIO PERIODICO DE PENDENTES
  if (millis() - ultimaTentativaPendentes > INTERVALO_TENTATIVA_PENDENTES) {
    ultimaTentativaPendentes = millis();
    if (WiFi.status() == WL_CONNECTED && temPendentes()) {
      processarPendentes();
      atualizarTela = true;
    }
  }

  // 1b. RECONEXAO WIFI AUTOMATICA
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - ultimaTentativaWiFi > INTERVALO_TENTATIVA_WIFI) {
      ultimaTentativaWiFi = millis();
      Serial.println(F("[WiFi] Conexao perdida. Tentando reconectar..."));
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      int t = 0;
      while (WiFi.status() != WL_CONNECTED && t < 20) { delay(500); t++; }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("[WiFi] Reconectado!"));
        atualizarTela = true;
      } else {
        Serial.println(F("[WiFi] Falha na reconexao. Tentara novamente."));
      }
    }
  }

  // 1c. SINCRONIZACAO NTP PERIODICA (a cada 6h)
  if (ultimaSincNTP > 0 && millis() - ultimaSincNTP > INTERVALO_SINC_NTP) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(F("[NTP] Ressincronizando hora..."));
      sincronizarHora();
      ultimaSincNTP = millis();
    }
  }

  // 2. VERIFICACAO PREVENTIVA DO SENSOR (a cada 2h)
  if (millis() - ultimaVerificacaoSensor > INTERVALO_SAUDE_SENSOR) {
    Serial.println(F("[WATCHDOG] Verificacao periodica do sensor..."));
    if (!finger.verifyPassword()) {
      Serial.println(F("[WATCHDOG] Sensor nao respondeu. Reinicializando..."));
      reinicializarSensor();
    } else {
      Serial.println(F("[WATCHDOG] Sensor OK."));
      ultimaVerificacaoSensor = millis();
    }
  }


  // 3. BOTAO FISICO DE CADASTRO RAPIDO
  if (digitalRead(PINO_BOTAO) == LOW) {
    delay(50); // debounce
    if (digitalRead(PINO_BOTAO) == LOW) {
      cadastrarDigitalBotao();
      atualizarTela = true;
      // Espera soltar o botao
      while (digitalRead(PINO_BOTAO) == LOW) { delay(10); }
    }
  }

  // 4. COMANDOS VIA SERIAL DO PC
  if (Serial.available()) {
    char opcao = Serial.read();
    delay(50);
    while (Serial.available()) { Serial.read(); }

    if (opcao == '1') cadastrarDigital();
    else if (opcao == '2') recadastrarDigital();
    else if (opcao == '3') deletarDigital();
    else if (opcao == '4') limparBancoDeDados();
    else if (opcao == '5') listarDigitais();
    else if (opcao == '6') listarNomesArquivo();
    else if (opcao == '7') editarNome();
    else if (opcao == '8') verPendentes();
    else if (opcao == '9') associarCartaoRFID();
    else if (opcao == 'a' || opcao == 'A') listarCartoesCadastrados();
    else if (opcao == 'b' || opcao == 'B') restaurarBackupNomes();

    imprimirMenuSerial();
    atualizarTela = true;
  }
  // 5. MODO PONTO (Tela de Espera 20x4)
  else {
    if (atualizarTela) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(formatarCentro(getLinhaDataHora()));
      lcd.setCursor(0, 1);
      lcd.print(formatarCentro("PONTO ELETRONICO"));
      lcd.setCursor(0, 2);
      lcd.print(formatarCentro("Aproxime o dedo"));
      lcd.setCursor(0, 3);
      if (temPendentes()) {
        int qtd = contarPendentes();
        lcd.print(formatarCentro("* Pendentes: " + String(qtd)));
      } else if (WiFi.status() == WL_CONNECTED) {
        lcd.print(formatarCentro("WiFi: Conectado (OK)"));
      } else {
        lcd.print(formatarCentro("WiFi: Desconectado!"));
      }
      atualizarTela = false;
      ultimoRelogio = millis();
    }

    // Atualiza periodicamente o relogio e o status sem piscar a tela
    if (millis() - ultimoRelogio > 5000) {
      ultimoRelogio = millis();
      lcd.setCursor(0, 0);
      lcd.print(formatarCentro(getLinhaDataHora()));
      lcd.setCursor(0, 3);
      if (temPendentes()) {
        int qtd = contarPendentes();
        lcd.print(formatarCentro("* Pendentes: " + String(qtd)));
      } else if (WiFi.status() == WL_CONNECTED) {
        lcd.print(formatarCentro("WiFi: Conectado (OK)"));
      } else {
        lcd.print(formatarCentro("WiFi: Desconectado!"));
      }
    }

    // 5a. LEITURA RFID (Slave)
    if (slaveSerial.available()) {
      String msg = slaveSerial.readStringUntil('\n');
      msg.trim();
      if (msg.length() > 0) {
        Serial.print(F("[SLAVE->MASTER] ")); Serial.println(msg);
        if (msg.startsWith("CARD:")) {
          String uidLido = msg.substring(5);
          processarPontoRFID(uidLido);
        }
      }
    }

    // 5b. LEITURA BIOMETRICA
    lerDigital();
  }

  delay(50);
}

// ====================================================================
// ================= FUNCOES DO PONTO ELETRONICO ======================
// ====================================================================

void lerDigital() {
  uint8_t p = finger.getImage();

  // Sem dedo no sensor: situacao normal, zera contador de erros
  if (p == FINGERPRINT_NOFINGER) {
    errosConsecutivosSensor = 0;
    return;
  }

  // Erro de comunicacao real (nao e' "sem dedo")
  if (p != FINGERPRINT_OK) {
    errosConsecutivosSensor++;
    Serial.print(F("[SENSOR] Erro de comunicacao #"));
    Serial.print(errosConsecutivosSensor);
    Serial.print(F(" (cod "));
    Serial.print(p);
    Serial.println(F(")"));
    if (errosConsecutivosSensor >= MAX_ERROS_SENSOR) {
      reinicializarSensor();
    }
    return;
  }

  // Imagem capturada com sucesso
  errosConsecutivosSensor = 0;
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println(F("\n[SENSOR] Lendo digital..."));

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  p = finger.fingerFastSearch();

  if (p != FINGERPRINT_OK) {
    Serial.println(F("[SENSOR] Dedo NAO encontrado! (Acesso Negado)"));
    bipNegado();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("ACESSO NEGADO!"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("Digital nao cadast."));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("Tente novamente..."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
    delay(2000);
    digitalWrite(LED_BUILTIN, HIGH);
    atualizarTela = true;
    return;
  }

  int id = finger.fingerID;

  // Cooldown: ignora se o mesmo ID foi registrado nos ultimos 5s
  if (id == ultimoIDRegistrado && millis() - ultimoRegistroMs < COOLDOWN_REGISTRO_MS) {
    Serial.println(F("[SENSOR] Cooldown ativo - registro ignorado."));
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    return;
  }
  ultimoIDRegistrado = id;
  ultimoRegistroMs   = millis();

  String nomeUsuario = buscarNome(id);

  Serial.print(F("[SENSOR] SUCESSO! ID: ")); Serial.print(id);
  Serial.print(F(" | Nome: ")); Serial.println(nomeUsuario);

  String statusAtual = buscarStatus(id);
  String novoStatus = (statusAtual == "E") ? "S" : "E";
  salvarStatus(id, novoStatus);

  // Bip de acordo com o tipo de registro
  if (novoStatus == "E") { bipEntrada(); } else { bipSaida(); }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarLinha("Ola, " + nomeUsuario));
  lcd.setCursor(0, 1);
  if (novoStatus == "E") {
    lcd.print(formatarCentro(">>> ENTRADA <<<"));
  } else {
    lcd.print(formatarCentro(">>>  SAIDA  <<<"));
  }
  lcd.setCursor(0, 2); lcd.print(formatarCentro(getDataHora()));
  lcd.setCursor(0, 3); lcd.print(formatarCentro("ID #" + String(id) + " - Registrado!"));

  String dataHora = getDataHora();
  enviarParaPlanilha(dataHora, id, nomeUsuario, novoStatus);

  delay(3000);
  digitalWrite(LED_BUILTIN, HIGH);
  atualizarTela = true;
}

// ====================================================================
// =================== FUNCOES RFID (SLAVE + BANCO) ===================
// ====================================================================

int buscarIdPorCartao(String uid) {
  File f = LittleFS.open(ARQUIVO_CARTOES, "r");
  if (!f) return -1;
  int idEncontrado = -1;
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;
    int pos = linha.indexOf(';');
    if (pos == -1) continue;
    if (linha.substring(0, pos).equalsIgnoreCase(uid)) {
      idEncontrado = linha.substring(pos + 1).toInt();
      break;
    }
  }
  f.close();
  return idEncontrado;
}

void salvarCartao(String uid, int id) {
  File origem = LittleFS.open(ARQUIVO_CARTOES, "r");
  File temp   = LittleFS.open(ARQUIVO_TEMP, "w");
  if (!temp) { if (origem) origem.close(); return; }
  if (origem) {
    while (origem.available()) {
      String linha = origem.readStringUntil('\n');
      linha.trim();
      if (linha.length() == 0) continue;
      int pos = linha.indexOf(';');
      if (pos == -1) continue;
      if (!linha.substring(0, pos).equalsIgnoreCase(uid)) temp.println(linha);
    }
    origem.close();
  }
  temp.print(uid); temp.print(';'); temp.println(id);
  temp.close();
  LittleFS.remove(ARQUIVO_CARTOES);
  LittleFS.rename(ARQUIVO_TEMP, ARQUIVO_CARTOES);
}

void listarCartoesCadastrados() {
  File f = LittleFS.open(ARQUIVO_CARTOES, "r");
  if (!f) { Serial.println("Nenhum cartao cadastrado."); return; }
  Serial.println("\n--- CARTOES RFID CADASTRADOS ---");
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;
    int pos = linha.indexOf(';');
    if (pos == -1) continue;
    String uid = linha.substring(0, pos);
    int id = linha.substring(pos + 1).toInt();
    Serial.print("Tag UID: "); Serial.print(uid);
    Serial.print(" -> ID #"); Serial.print(id);
    Serial.print(" ("); Serial.print(buscarNome(id)); Serial.println(")");
  }
  Serial.println("--------------------------------");
  f.close();
}

void processarPontoRFID(String uid) {
  int id = buscarIdPorCartao(uid);

  if (id == -1) {
    Serial.print(F("[RFID] Tag nao cadastrada: ")); Serial.println(uid);
    Serial.println(F("[MASTER->SLAVE] Enviando: DENIED"));
    slaveSerial.println("DENIED");
    bipNegado();

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("ACESSO NEGADO!"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("Cartao nao cadast."));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("Tente novamente..."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
    delay(2500);
    atualizarTela = true;
    return;
  }

  // Cooldown: ignora se o mesmo ID foi registrado nos ultimos 5s
  // (enviamos OK antes para o slave dar feedback imediato)
  if (id == ultimoIDRegistrado && millis() - ultimoRegistroMs < COOLDOWN_REGISTRO_MS) {
    Serial.println(F("[RFID] Cooldown ativo - registro ignorado."));
    slaveSerial.println("OK");
    return;
  }
  ultimoIDRegistrado = id;
  ultimoRegistroMs   = millis();

  String nomeUsuario = buscarNome(id);

  Serial.print(F("[RFID] SUCESSO! Tag: ")); Serial.print(uid);
  Serial.print(F(" | ID: ")); Serial.print(id);
  Serial.print(F(" | Nome: ")); Serial.println(nomeUsuario);

  String statusAtual = buscarStatus(id);
  String novoStatus = (statusAtual == "E") ? "S" : "E";
  salvarStatus(id, novoStatus);

  // Notifica a Slave com entrada ou saida (para o LED RGB)
  // e aciona o buzzer correspondente no Master
  if (novoStatus == "E") {
    Serial.println(F("[MASTER->SLAVE] Enviando: OK_E"));
    slaveSerial.println("OK_E");
    bipEntrada();
  } else {
    Serial.println(F("[MASTER->SLAVE] Enviando: OK_S"));
    slaveSerial.println("OK_S");
    bipSaida();
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarLinha("Ola, " + nomeUsuario));
  lcd.setCursor(0, 1);
  if (novoStatus == "E") {
    lcd.print(formatarCentro(">>> ENTRADA <<<"));
  } else {
    lcd.print(formatarCentro(">>>  SAIDA  <<<"));
  }
  lcd.setCursor(0, 2); lcd.print(formatarCentro(getDataHora()));
  lcd.setCursor(0, 3); lcd.print(formatarCentro("[RFID] Registrado!"));

  String dataHora = getDataHora();
  enviarParaPlanilha(dataHora, id, nomeUsuario, novoStatus);

  delay(2500);
  atualizarTela = true;
}

void associarCartaoRFID() {
  Serial.println(F("\n--- ASSOCIAR CARTAO RFID A UMA PESSOA ---"));
  Serial.println(F("Digite o ID da pessoa (1 a 127):"));

  int id = readnumber();
  if (id == 0) return;

  String nome = buscarNome(id);
  Serial.print(F("Associando cartao para: ID #")); Serial.print(id);
  Serial.print(F(" - ")); Serial.println(nome);
  Serial.println(F("Aproxime o cartao no leitor RFID (ou digite o UID no teclado)..."));

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRAR RFID"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id) + " - " + nome));
  lcd.setCursor(0, 2); lcd.print(formatarCentro("Aproxime o cartao"));
  lcd.setCursor(0, 3); lcd.print(formatarCentro("ou digite UID no PC"));

  String uidRecebido = "";
  unsigned long inicio = millis();
  while (millis() - inicio < 15000) {
    if (slaveSerial.available()) {
      String m = slaveSerial.readStringUntil('\n');
      m.trim();
      if (m.startsWith("CARD:")) {
        uidRecebido = m.substring(5);
        slaveSerial.println("OK");
        break;
      }
    }
    if (Serial.available()) {
      uidRecebido = Serial.readStringUntil('\n');
      uidRecebido.trim();
      uidRecebido.toUpperCase();
      break;
    }
    delay(50);
  }

  if (uidRecebido.length() > 0) {
    salvarCartao(uidRecebido, id);
    Serial.print(F("SUCESSO! Cartao [")); Serial.print(uidRecebido);
    Serial.print(F("] associado ao ID #")); Serial.println(id);

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("RFID GRAVADO!"));
    lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id) + " - " + nome));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("UID: " + uidRecebido));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Salvo com sucesso!"));
    delay(2500);
  } else {
    Serial.println(F("Tempo esgotado. Operacao cancelada."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRAR RFID"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("Tempo esgotado!"));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("Operacao cancelada."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
    delay(2000);
  }
  atualizarTela = true;
}

// ====================================================================
// ================= CADASTRO PELO BOTAO (Sem PC) =====================
// ====================================================================

void cadastrarDigitalBotao() {
  Serial.println("\n[BOTAO] Iniciando cadastro sem PC...");
  escreverLCD("  MODO CADASTRO   ", "Buscando vaga livre.", "Aguarde um momento..", "");

  int idLivre = -1;
  for (int i = 1; i <= 127; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) {
      idLivre = i;
      break;
    }
  }

  if (idLivre == -1) {
    escreverLCD("  MODO CADASTRO   ", "Erro: Memoria cheia!", "Limite: 127 digitais", "");
    delay(2500);
    return;
  }

  escreverLCD(" CADASTRO RAPIDO  ", "ID #" + String(idLivre) + " livre!", "Coloque o dedo...", "1a Leitura");

  int p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { escreverLCD(" CADASTRO RAPIDO  ", "Erro na 1a captura!", "Cancelado.", ""); delay(2000); return; }

  escreverLCD(" CADASTRO RAPIDO  ", "1a Leitura OK!", "Tire o dedo agora...", "");
  delay(2000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); }

  escreverLCD(" CADASTRO RAPIDO  ", "2a Leitura:", "Coloque O MESMO dedo", "novamente...");
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { escreverLCD(" CADASTRO RAPIDO  ", "Erro na 2a captura!", "Cancelado.", ""); delay(2000); return; }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) { escreverLCD(" CADASTRO RAPIDO  ", "Digitais divergentes", "Nao bateram!", "Tente novamente."); delay(2500); return; }

  p = finger.storeModel(idLivre);
  if (p == FINGERPRINT_OK) {
    String nomeTemp = "User " + String(idLivre);
    salvarNome(idLivre, nomeTemp);
    salvarStatus(idLivre, "S");

    escreverLCD("CADASTRO CONCLUIDO", "ID #" + String(idLivre) + " Gravado!", "Nome: " + nomeTemp, "Salvo com sucesso!");
    Serial.println("[BOTAO] Cadastro do ID " + String(idLivre) + " finalizado com sucesso!");
    delay(2500);
  } else {
    escreverLCD(" CADASTRO RAPIDO  ", "Erro ao gravar!", "Falha de memoria.", "");
    delay(2500);
  }
}

// ====================================================================
// ==================== FECHAMENTO AUTOMATICO =========================
// ====================================================================

void forcarSaidaAutomatica() {
  File f = LittleFS.open(ARQUIVO_STATUS, "r");
  if (!f) return;

  File temp = LittleFS.open("/statustemp.txt", "w");
  if (!temp) { f.close(); return; }

  String dataHora = getDataHora();
  int fechados = 0;

  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    int pos = linha.indexOf(';');
    if (pos == -1) continue;

    int id = linha.substring(0, pos).toInt();
    String status = linha.substring(pos + 1);

    if (status == "E") {
      String nome = buscarNome(id);
      Serial.print(" -> Fechando ponto de: "); Serial.println(nome);
      enviarParaPlanilha(dataHora, id, nome, "S");
      status = "S";
      fechados++;
      delay(500);
    }

    temp.print(id);
    temp.print(';');
    temp.println(status);
  }

  f.close();
  temp.close();

  LittleFS.remove(ARQUIVO_STATUS);
  LittleFS.rename("/statustemp.txt", ARQUIVO_STATUS);

  Serial.print("Fechamento concluido. ");
  Serial.print(fechados);
  Serial.println(" pessoa(s) deslogada(s).");
}

// ====================================================================
// =================== FUNCOES ADMINISTRATIVAS ========================
// ====================================================================

void imprimirMenuSerial() {
  Serial.println(F("\n--- MENU ADMINISTRATIVO ---"));
  Serial.println(F("1 - CADASTRAR nova pessoa (ID automatico)"));
  Serial.println(F("2 - RECADASTRAR digital de alguem (mesma pessoa, nova digital)"));
  Serial.println(F("3 - APAGAR UMA digital especifica"));
  Serial.println(F("4 - APAGAR TODAS as digitais"));
  Serial.println(F("5 - LISTAR digitais cadastradas"));
  Serial.println(F("6 - LISTAR arquivo de nomes (bruto)"));
  Serial.println(F("7 - EDITAR/CORRIGIR nome de um ID"));
  Serial.println(F("8 - VER/REENVIAR pendentes agora"));
  Serial.println(F("9 - CADASTRAR/ASSOCIAR cartao RFID a um ID"));
  Serial.println(F("A - LISTAR cartoes RFID cadastrados"));
  Serial.println(F("B - RESTAURAR nomes de backup (29 integrantes)"));
  Serial.println(F("Digite a opcao:"));
}

void cadastrarDigital() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("MODO CADASTRO"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Buscando vaga livre."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Aguarde..."));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

  Serial.println(F("\n--- MODO CADASTRO (ID AUTOMATICO) ---"));
  Serial.println(F("Buscando proximo ID disponivel..."));

  // Encontra o primeiro slot livre no sensor
  int id = -1;
  for (int i = 1; i <= 127; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) {
      id = i;
      break;
    }
  }

  if (id == -1) {
    Serial.println(F("ERRO: Memoria do sensor cheia! (127/127)"));
    lcd.setCursor(0, 1); lcd.print(formatarLinha("Erro: Memoria cheia!"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Limite: 127 digitais"));
    delay(2500);
    return;
  }

  Serial.print(F("ID atribuido automaticamente: #")); Serial.println(id);
  lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id) + " disponivel"));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Coloque o dedo no   "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("sensor biometrico..."));
  delay(1200);

  int p = -1;
  Serial.println(F("Coloque o dedo no sensor..."));
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRO ID #" + String(id)));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("1a Leitura:"));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Coloque o dedo..."));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println(F("Erro ao ler imagem."));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Erro na 1a leitura!"));
    delay(2000);
    return;
  }

  Serial.println(F("Tire o dedo do sensor."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("1a Leitura OK!"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Tire o dedo agora..."));
  delay(2000);

  p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); }

  p = -1;
  Serial.println(F("Coloque O MESMO DEDO novamente..."));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("2a Leitura:"));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Coloque O MESMO dedo"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("novamente..."));

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println(F("Erro ao ler imagem."));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Erro na 2a leitura!"));
    delay(2000);
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println(F("As digitais nao combinam!"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Digitais divergentes"));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Nao bateram!"));
    delay(2500);
    return;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println(F("SUCESSO! Digital gravada."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRO GRAVADO"));
    lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id) + " registrado!"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Digite o nome no PC:"));
    lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

    String nome = lerNomeViaSerial();
    if (nome.length() == 0) {
      Serial.println(F("Cadastro cancelado (timeout)."));
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRO"));
      lcd.setCursor(0, 1); lcd.print(formatarCentro("Cancelado!"));
      lcd.setCursor(0, 2); lcd.print(formatarCentro("Timeout de 60s"));
      lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
      delay(2000);
      atualizarTela = true;
      return;
    }
    salvarNome(id, nome);
    salvarStatus(id, "S");

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("CADASTRO SUCESSO"));
    lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id)));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Nome: " + nome));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Salvo com sucesso!"));
    delay(2000);
  } else {
    Serial.println(F("Erro ao gravar no sensor."));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Erro ao gravar!"));
    delay(2000);
  }
}

// --------------------------------------------------------------------

void recadastrarDigital() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("RECADASTRO DIGITAL"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Aguardando ID via   "));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Monitor Serial...   "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

  Serial.println(F("\n--- RECADASTRAR DIGITAL ---"));
  Serial.println(F("Digite o ID (1 a 127) da pessoa a recadastrar:"));

  int id = readnumber();
  if (id == 0) return;

  String nomeAtual = buscarNome(id);
  if (nomeAtual == "Desconhecido") {
    Serial.print(F("AVISO: ID #")); Serial.print(id);
    Serial.println(F(" nao tem nome cadastrado. Continuar mesmo assim? (S para sim)"));
    delay(100);
    while (Serial.available()) { Serial.read(); }
    unsigned long t0 = millis();
    while (!Serial.available()) {
      if (millis() - t0 > 30000) { Serial.println(F("Timeout. Cancelado.")); return; }
      delay(10);
    }
    String resp = Serial.readStringUntil('\n');
    resp.trim();
    if (resp != "S" && resp != "s") {
      Serial.println(F("Cancelado."));
      return;
    }
  }

  Serial.print(F("Recadastrando: ID #")); Serial.print(id);
  Serial.print(F(" | ")); Serial.println(nomeAtual);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("RECADASTRO ID #" + String(id)));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Nome: " + nomeAtual));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Coloque o dedo no   "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("sensor biometrico..."));
  delay(1500);

  int p = -1;
  Serial.println(F("Coloque o dedo no sensor..."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("1a Leitura:         "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Coloque o dedo...   "));

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println(F("Erro ao ler imagem."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Erro na 1a leitura! "));
    delay(2000);
    return;
  }

  Serial.println(F("Tire o dedo do sensor."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("1a Leitura OK!      "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Tire o dedo agora!  "));
  delay(2000);

  p = 0;
  while (p != FINGERPRINT_NOFINGER) { p = finger.getImage(); }

  p = -1;
  Serial.println(F("Coloque O MESMO DEDO novamente..."));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("2a Leitura:         "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Dedo novamente...   "));

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) delay(100);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println(F("Erro ao ler imagem."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Erro na 2a leitura! "));
    delay(2000);
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println(F("As digitais nao combinam!"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Digitais divergentes"));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Nao bateram!        "));
    delay(2000);
    return;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.print(F("SUCESSO! Digital de '")); Serial.print(nomeAtual);
    Serial.println(F("' recadastrada. Nome e status mantidos."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("RECADASTRO OK!"));
    lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id)));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Nome: " + nomeAtual));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Digital atualizada! "));
    delay(2000);
  } else {
    Serial.println(F("Erro ao gravar no sensor."));
    lcd.setCursor(0, 3); lcd.print(formatarLinha("Erro ao gravar!     "));
    delay(2000);
  }
}


void deletarDigital() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("MODO EXCLUSAO"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Digite ID no Serial "));
  lcd.setCursor(0, 2); lcd.print(formatarLinha(""));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
  Serial.println("\n--- MODO DELETAR ---");
  Serial.println("Digite o ID (1 a 127) que voce quer apagar:");

  uint8_t id = readnumber();
  if (id == 0) return;

  uint8_t p = finger.deleteModel(id);

  if (p == FINGERPRINT_OK) {
    Serial.println("SUCESSO! Digital apagada.");
    lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id) + " apagado!"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Removido com sucesso"));
    apagarNome(id);
    apagarStatus(id);
  } else {
    Serial.println("Nao foi possivel apagar. ID vazio?");
    lcd.setCursor(0, 1); lcd.print(formatarLinha("Falha ao apagar ID"));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("ID #" + String(id) + " vazio?"));
  }
  delay(2000);
}

void limparBancoDeDados() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("LIMPEZA TOTAL"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Confirmar no Serial:"));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Digite 'S' no PC... "));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));

  Serial.println("\n--- LIMPANDO TODO O SENSOR ---");
  Serial.println("Digite 'S' para confirmar ou qualquer tecla para cancelar.");

  delay(100);
  while (Serial.available()) { Serial.read(); }
  unsigned long t0 = millis();
  while (!Serial.available()) {
    if (millis() - t0 > 30000) {
      Serial.println(F("Timeout. Operacao cancelada."));
      lcd.setCursor(0, 1); lcd.print(formatarLinha("Timeout - cancelado."));
      delay(2000);
      atualizarTela = true;
      return;
    }
    delay(10);
  }

  String resposta = Serial.readStringUntil('\n');
  resposta.trim();

  if (resposta == "S" || resposta == "s") {
    uint8_t p = finger.emptyDatabase();
    if (p == FINGERPRINT_OK) {
      Serial.println("SUCESSO! Memoria limpa.");
      lcd.setCursor(0, 1); lcd.print(formatarLinha("Memoria formatada!  "));
      lcd.setCursor(0, 2); lcd.print(formatarLinha("Arquivos zerados.   "));
      lcd.setCursor(0, 3); lcd.print(formatarLinha("Sucesso total!      "));

      LittleFS.remove(ARQUIVO_PESSOAS);
      File f1 = LittleFS.open(ARQUIVO_PESSOAS, "w");
      if (f1) f1.close();

      LittleFS.remove(ARQUIVO_STATUS);
      File f2 = LittleFS.open(ARQUIVO_STATUS, "w");
      if (f2) f2.close();

      LittleFS.remove(ARQUIVO_PENDENTES);
      File f3 = LittleFS.open(ARQUIVO_PENDENTES, "w");
      if (f3) f3.close();

      LittleFS.remove(ARQUIVO_CARTOES);
      File f4 = LittleFS.open(ARQUIVO_CARTOES, "w");
      if (f4) f4.close();
    }
  } else {
    Serial.println("Operacao cancelada.");
    lcd.setCursor(0, 1); lcd.print(formatarLinha("Operacao cancelada. "));
    lcd.setCursor(0, 2); lcd.print(formatarLinha("Nada foi alterado.  "));
  }
  delay(2000);
}

void listarDigitais() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("CONSULTA DIGITAIS"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Listando IDs...     "));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Veja no Serial do PC"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
  Serial.println("\n--- CONSULTANDO DIGITAIS ---");

  finger.getTemplateCount();
  Serial.print("Total salvas: "); Serial.println(finger.templateCount);
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Total salvas: " + String(finger.templateCount)));

  for (int i = 1; i <= 127; i++) {
    uint8_t p = finger.loadModel(i);
    if (p == FINGERPRINT_OK) {
      Serial.print("-> ID #"); Serial.print(i);
      Serial.print(" - "); Serial.print(buscarNome(i));
      Serial.print(" ["); Serial.print(buscarStatus(i) == "E" ? "Dentro" : "Fora"); Serial.println("]");
    }
  }
  Serial.println("----------------------------");
  delay(2000);
}

void editarNome() {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("EDITAR NOME"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("Digite ID no Serial "));
  lcd.setCursor(0, 2); lcd.print(formatarLinha(""));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
  Serial.println("\n--- EDITAR NOME ---");
  Serial.println("Digite o ID (1 a 127) cujo nome deseja editar:");

  int id = readnumber();
  if (id == 0) return;

  String nomeAtual = buscarNome(id);
  Serial.print("Nome atual para o ID ");
  Serial.print(id); Serial.print(": ");
  Serial.println(nomeAtual);

  lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id)));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Atual: " + nomeAtual));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Digite novo no PC..."));

  String novoNome = lerNomeViaSerial();
  if (novoNome.length() == 0) {
    Serial.println(F("Edicao cancelada (timeout)."));
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(formatarCentro("EDICAO DE NOME"));
    lcd.setCursor(0, 1); lcd.print(formatarCentro("Cancelada!"));
    lcd.setCursor(0, 2); lcd.print(formatarCentro("Timeout de 60s"));
    lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
    delay(2000);
    atualizarTela = true;
    return;
  }
  salvarNome(id, novoNome);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("NOME ATUALIZADO"));
  lcd.setCursor(0, 1); lcd.print(formatarLinha("ID #" + String(id)));
  lcd.setCursor(0, 2); lcd.print(formatarLinha("Novo: " + novoNome));
  lcd.setCursor(0, 3); lcd.print(formatarLinha("Salvo com sucesso!  "));
  delay(2000);
}

void listarNomesArquivo() {
  File f = LittleFS.open(ARQUIVO_PESSOAS, "r");
  if (!f) { Serial.println("Nao foi possivel abrir."); return; }
  Serial.println("\n--- ARQUIVO /pessoas.txt ---");
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() > 0) Serial.println(linha);
  }
  Serial.println("----------------------------");
  f.close();
}

void restaurarBackupNomes() {
  Serial.println(F("\n--- RESTAURANDO BACKUP DE NOMES ---"));
  File f = LittleFS.open(ARQUIVO_PESSOAS, "w");
  if (!f) {
    Serial.println(F("Erro ao abrir /pessoas.txt para gravacao!"));
    return;
  }
  f.println("1;Integrante 01");
  f.println("2;Integrante 02");
  f.println("3;Integrante 03");
  f.println("4;Integrante 04");
  f.println("5;Integrante 05");
  f.println("6;Integrante 06");
  f.println("7;Integrante 07");
  f.println("8;Integrante 08");
  f.println("9;Integrante 09");
  f.println("10;Integrante 10");
  f.println("11;Integrante 11");
  f.println("12;Integrante 12");
  f.println("13;Integrante 13");
  f.println("14;Integrante 14");
  f.println("15;Integrante 15");
  f.println("16;Integrante 16");
  f.println("17;Integrante 17");
  f.println("18;Integrante 18");
  f.println("19;Integrante 19");
  f.println("20;Integrante 20");
  f.println("21;Integrante 21");
  f.println("22;Integrante 22");
  f.println("23;Integrante 23");
  f.println("24;Integrante 24");
  f.println("25;Integrante 25");
  f.println("26;Integrante 26");
  f.println("27;Integrante 27");
  f.println("28;Integrante 28");
  f.println("29;Integrante 29");
  f.close();

  // Restaura status padrao (S) para todos
  File fs = LittleFS.open(ARQUIVO_STATUS, "w");
  if (fs) {
    for (int i = 1; i <= 29; i++) {
      fs.print(i); fs.println(";S");
    }
    fs.close();
  }

  Serial.println(F("SUCESSO! Todos os 29 nomes e status foram restaurados!"));
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(formatarCentro("BACKUP RESTAURADO"));
  lcd.setCursor(0, 1); lcd.print(formatarCentro("29 nomes gravados!"));
  lcd.setCursor(0, 2); lcd.print(formatarCentro("LittleFS OK"));
  lcd.setCursor(0, 3); lcd.print(formatarLinha(""));
  delay(2000);
  listarNomesArquivo();
}

void verPendentes() {
  Serial.println("\n--- REGISTROS PENDENTES ---");

  File f = LittleFS.open(ARQUIVO_PENDENTES, "r");
  if (!f) {
    Serial.println("Nao foi possivel abrir o arquivo de pendentes.");
    return;
  }

  int total = 0;
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() > 0) {
      Serial.println(linha);
      total++;
    }
  }
  f.close();

  Serial.print("Total de pendentes: ");
  Serial.println(total);

  if (total > 0) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Tentando reenviar agora...");
      processarPendentes();
    } else {
      Serial.println("Sem WiFi no momento.");
    }
  }
  Serial.println("----------------------------");
}

uint8_t readnumber(void) {
  uint8_t num = 0;
  unsigned long inicio = millis();
  const unsigned long TIMEOUT_MS = 60000UL; // 60 segundos
  while (num == 0) {
    if (millis() - inicio > TIMEOUT_MS) {
      Serial.println(F("[TIMEOUT] Nenhuma entrada recebida. Operacao cancelada."));
      return 0;
    }
    if (Serial.available()) {
      num = Serial.parseInt();
      delay(50);
      while (Serial.available()) { Serial.read(); }
    }
    delay(10);
  }
  return num;
}

// ====================================================================
// =============== FUNCOES DE ARQUIVO (LittleFS) ======================
// ====================================================================

void iniciarArquivos() {
  if (!LittleFS.begin()) {
    Serial.println("Erro ao montar o LittleFS! Formatando...");
    LittleFS.format();
    LittleFS.begin();
  }

  if (!LittleFS.exists(ARQUIVO_PESSOAS)) {
    File f = LittleFS.open(ARQUIVO_PESSOAS, "w");
    if (f) f.close();
  }
  if (!LittleFS.exists(ARQUIVO_STATUS)) {
    File f = LittleFS.open(ARQUIVO_STATUS, "w");
    if (f) f.close();
  }
  if (!LittleFS.exists(ARQUIVO_PENDENTES)) {
    File f = LittleFS.open(ARQUIVO_PENDENTES, "w");
    if (f) f.close();
  }
  if (!LittleFS.exists(ARQUIVO_CARTOES)) {
    File f = LittleFS.open(ARQUIVO_CARTOES, "w");
    if (f) f.close();
  }
}

String lerNomeViaSerial() {
  Serial.println("Digite o NOME da pessoa e pressione Enter:");
  delay(100);
  while (Serial.available()) { Serial.read(); }
  unsigned long inicio = millis();
  const unsigned long TIMEOUT_MS = 60000UL; // 60 segundos
  while (!Serial.available()) {
    if (millis() - inicio > TIMEOUT_MS) {
      Serial.println(F("[TIMEOUT] Nenhum nome digitado. Operacao cancelada."));
      return "";
    }
    delay(10);
  }
  String nome = Serial.readStringUntil('\n');
  nome.trim();
  while (Serial.available()) { Serial.read(); }
  return nome;
}

String buscarNome(int id) {
  File f = LittleFS.open(ARQUIVO_PESSOAS, "r");
  if (!f) return "Desconhecido";
  String resultado = "Desconhecido";
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;
    int pos = linha.indexOf(';');
    if (pos == -1) continue;
    if (linha.substring(0, pos).toInt() == id) {
      resultado = linha.substring(pos + 1);
      break;
    }
  }
  f.close();
  return resultado;
}

void salvarNome(int id, String nome) {
  File origem = LittleFS.open(ARQUIVO_PESSOAS, "r");
  File temp   = LittleFS.open(ARQUIVO_TEMP, "w");
  if (!temp) { if (origem) origem.close(); return; }
  if (origem) {
    while (origem.available()) {
      String linha = origem.readStringUntil('\n');
      linha.trim();
      if (linha.length() == 0) continue;
      int pos = linha.indexOf(';');
      if (pos == -1) continue;
      if (linha.substring(0, pos).toInt() != id) temp.println(linha);
    }
    origem.close();
  }
  temp.print(id); temp.print(';'); temp.println(nome);
  temp.close();
  LittleFS.remove(ARQUIVO_PESSOAS);
  LittleFS.rename(ARQUIVO_TEMP, ARQUIVO_PESSOAS);
}

void apagarNome(int id) {
  File origem = LittleFS.open(ARQUIVO_PESSOAS, "r");
  File temp   = LittleFS.open(ARQUIVO_TEMP, "w");
  if (!temp) { if (origem) origem.close(); return; }
  if (origem) {
    while (origem.available()) {
      String linha = origem.readStringUntil('\n');
      linha.trim();
      if (linha.length() == 0) continue;
      int pos = linha.indexOf(';');
      if (pos == -1) continue;
      if (linha.substring(0, pos).toInt() != id) temp.println(linha);
    }
    origem.close();
  }
  temp.close();
  LittleFS.remove(ARQUIVO_PESSOAS);
  LittleFS.rename(ARQUIVO_TEMP, ARQUIVO_PESSOAS);
}

// ====================================================================
// ============== FUNCOES DE STATUS ==================================
// ====================================================================

String buscarStatus(int id) {
  File f = LittleFS.open(ARQUIVO_STATUS, "r");
  if (!f) return "S";
  String resultado = "S";
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;
    int pos = linha.indexOf(';');
    if (pos == -1) continue;
    if (linha.substring(0, pos).toInt() == id) {
      resultado = linha.substring(pos + 1);
      break;
    }
  }
  f.close();
  return resultado;
}

void salvarStatus(int id, String status) {
  File origem = LittleFS.open(ARQUIVO_STATUS, "r");
  File temp   = LittleFS.open("/statustemp.txt", "w");
  if (!temp) { if (origem) origem.close(); return; }
  if (origem) {
    while (origem.available()) {
      String linha = origem.readStringUntil('\n');
      linha.trim();
      if (linha.length() == 0) continue;
      int pos = linha.indexOf(';');
      if (pos == -1) continue;
      if (linha.substring(0, pos).toInt() != id) temp.println(linha);
    }
    origem.close();
  }
  temp.print(id); temp.print(';'); temp.println(status);
  temp.close();
  LittleFS.remove(ARQUIVO_STATUS);
  LittleFS.rename("/statustemp.txt", ARQUIVO_STATUS);
}

void apagarStatus(int id) {
  File origem = LittleFS.open(ARQUIVO_STATUS, "r");
  File temp   = LittleFS.open("/statustemp.txt", "w");
  if (!temp) { if (origem) origem.close(); return; }
  if (origem) {
    while (origem.available()) {
      String linha = origem.readStringUntil('\n');
      linha.trim();
      if (linha.length() == 0) continue;
      int pos = linha.indexOf(';');
      if (pos == -1) continue;
      if (linha.substring(0, pos).toInt() != id) temp.println(linha);
    }
    origem.close();
  }
  temp.close();
  LittleFS.remove(ARQUIVO_STATUS);
  LittleFS.rename("/statustemp.txt", ARQUIVO_STATUS);
}

// ====================================================================
// ============ FUNCOES DE PENDENTES =================================
// ====================================================================

bool temPendentes() {
  if (!LittleFS.exists(ARQUIVO_PENDENTES)) return false;
  File f = LittleFS.open(ARQUIVO_PENDENTES, "r");
  if (!f) return false;
  bool r = f.size() > 0;
  f.close();
  return r;
}

int contarPendentes() {
  if (!LittleFS.exists(ARQUIVO_PENDENTES)) return 0;
  File f = LittleFS.open(ARQUIVO_PENDENTES, "r");
  if (!f) return 0;
  int count = 0;
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() > 0) count++;
  }
  f.close();
  return count;
}

// Formato do pendente: dataHora;id;nome;status;tentativas
void salvarPendente(String dataHora, int id, String nome, String status) {
  File f = LittleFS.open(ARQUIVO_PENDENTES, "a");
  if (!f) return;
  f.print(dataHora); f.print(';');
  f.print(id);       f.print(';');
  f.print(nome);     f.print(';');
  f.print(status);   f.print(';');
  f.println(1); // primeira tentativa
  f.close();
}

void processarPendentes() {
  if (!temPendentes()) return;
  File origem = LittleFS.open(ARQUIVO_PENDENTES, "r");
  if (!origem) return;
  File restantes = LittleFS.open("/ptemp.txt", "w");
  if (!restantes) { origem.close(); return; }

  int enviados = 0, falharam = 0, descartados = 0;
  while (origem.available()) {
    String linha = origem.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    // Parsear: dataHora;id;nome;status;tentativas
    int p1 = linha.indexOf(';');
    if (p1 == -1) continue;
    int p2 = linha.indexOf(';', p1 + 1);
    if (p2 == -1) continue;
    int p3 = linha.indexOf(';', p2 + 1);
    if (p3 == -1) continue;
    int p4 = linha.indexOf(';', p3 + 1);

    String dh, nm, st;
    int id;
    int tentativas;

    if (p4 == -1) {
      // Formato sem contador de tentativas (compatibilidade com dados antigos)
      dh = linha.substring(0, p1);
      id = linha.substring(p1 + 1, p2).toInt();
      nm = linha.substring(p2 + 1, p3);
      st = linha.substring(p3 + 1);
      tentativas = 1;
    } else {
      // Formato com contador de tentativas
      dh = linha.substring(0, p1);
      id = linha.substring(p1 + 1, p2).toInt();
      nm = linha.substring(p2 + 1, p3);
      st = linha.substring(p3 + 1, p4);
      tentativas = linha.substring(p4 + 1).toInt();
    }

    if (enviarHTTP(dh, id, nm, st)) {
      enviados++;
    } else {
      tentativas++;
      if (tentativas > MAX_TENTATIVAS_PENDENTE) {
        // Passou do limite. Descarta para nao ficar preso para sempre.
        Serial.print("[PENDENTES] Descartado apos "); Serial.print(MAX_TENTATIVAS_PENDENTE);
        Serial.print(" tentativas: "); Serial.println(nm);
        descartados++;
      } else {
        // Salva de volta com o contador atualizado
        restantes.print(dh); restantes.print(';');
        restantes.print(id); restantes.print(';');
        restantes.print(nm); restantes.print(';');
        restantes.print(st); restantes.print(';');
        restantes.println(tentativas);
        falharam++;
      }
    }
  }
  origem.close();
  restantes.close();
  LittleFS.remove(ARQUIVO_PENDENTES);
  LittleFS.rename("/ptemp.txt", ARQUIVO_PENDENTES);

  if (enviados > 0) { Serial.print(enviados); Serial.println(" pendente(s) enviado(s)."); }
  if (falharam > 0) { Serial.print(falharam); Serial.println(" pendente(s) na fila."); }
  if (descartados > 0) { Serial.print(descartados); Serial.println(" pendente(s) descartado(s) (limite de tentativas)."); }
}

// ====================================================================
// ================= FUNCOES DE REDE =================================
// ====================================================================

void conectarWiFi() {
  Serial.print("Conectando ao WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado!"); Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha no WiFi.");
  }
}

void sincronizarHora() {
  configTime(-3 * 3600, 0, "pool.ntp.org", "a.st1.ntp.br");
  Serial.print("Sincronizando hora");
  time_t agora = time(nullptr);
  int t = 0;
  while (agora < 100000 && t < 40) { delay(500); Serial.print("."); agora = time(nullptr); t++; }
  Serial.println("\nHora sincronizada.");
}

String getDataHora() {
  time_t agora = time(nullptr);
  struct tm *tmInfo = localtime(&agora);
  char buf[24];
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
          tmInfo->tm_mday, tmInfo->tm_mon + 1, tmInfo->tm_year + 1900,
          tmInfo->tm_hour, tmInfo->tm_min, tmInfo->tm_sec);
  return String(buf);
}

String getHoraCurta() {
  time_t agora = time(nullptr);
  struct tm *tmInfo = localtime(&agora);
  char buf[8];
  sprintf(buf, "%02d:%02d", tmInfo->tm_hour, tmInfo->tm_min);
  return String(buf);
}

String urlEncode(String str) {
  String enc = "";
  char buf[4];
  for (unsigned int i = 0; i < str.length(); i++) {
    unsigned char c = (unsigned char)str.charAt(i);
    if (isalnum(c)) enc += (char)c;
    else { sprintf(buf, "%%%02X", c); enc += buf; }
  }
  return enc;
}

bool enviarHTTP(String dataHora, int id, String nome, String status) {
  if (WiFi.status() != WL_CONNECTED) return false;
  String sp = status;
  if (status == "E") sp = "Entrada";
  else if (status == "S") sp = "Saida";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String(WEBAPP_URL) + "?dataHora=" + urlEncode(dataHora)
             + "&id=" + String(id)
             + "&nome=" + urlEncode(nome) + "&status=" + urlEncode(sp);

  bool ok = false;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (http.begin(client, url)) {
    int cod = http.GET();
    Serial.print("[HTTP] Codigo: "); Serial.println(cod);

    // Se o Google respondeu QUALQUER coisa (codigo >= 0), os dados chegaram.
    // Codigos negativos (-1, -11, etc.) significam que NEM CONECTOU.
    if (cod >= 0) {
      ok = true;
    }
    http.end();
  }
  return ok;
}

void enviarParaPlanilha(String dataHora, int id, String nome, String status) {
  if (WiFi.status() != WL_CONNECTED) {
    salvarPendente(dataHora, id, nome, status);
    return;
  }
  if (!enviarHTTP(dataHora, id, nome, status)) {
    salvarPendente(dataHora, id, nome, status);
  }
}
