/**
 * FIRMWARE SLAVE RFID
 * NodeMCU ESP8266 + PN532 V3 (SPI) + LED RGB
 *
 * Biblioteca necessária: "Adafruit PN532"
 *   -> Sketch > Gerenciar Bibliotecas > buscar "Adafruit PN532"
 *
 * =============================================
 *  CHAVES DIP DO PN532 V3 (Modo SPI):
 *    SW1 = OFF (0)
 *    SW2 = OFF (0)
 *
 *  FIAÇÃO (Slave NodeMCU + PN532):
 *    PN532 VCC   -> VIN ou VV (5V)
 *    PN532 GND   -> GND
 *    PN532 SCK   -> D5 (GPIO14)
 *    PN532 MISO  -> D6 (GPIO12)
 *    PN532 MOSI  -> D7 (GPIO13)
 *    PN532 SS    -> D4 (GPIO4)
 *
 *  LED RGB (catodo comum):
 *    R (vermelho) -> [220Ω] -> D1 (GPIO5)
 *    G (verde)    -> [220Ω] -> D3 (GPIO0)
 *    B (azul)     -> [47Ω]  -> D8 (GPIO15) + [10kΩ] D8->GND (boot pulldown)
 *    Catodo (-)   -> GND
 *
 *  COMUNICAÇÃO COM A MASTER (UART):
 *    SLAVE TX (GPIO1) -> MASTER D7 (GPIO13 - SoftwareSerial RX)
 *    SLAVE RX (GPIO3) -> MASTER D0 (GPIO16 - SoftwareSerial TX)
 *    SLAVE GND        -> MASTER GND (terra comum obrigatório)
 *
 * =============================================
 *  PROTOCOLO:
 *    Slave -> Master: "CARD:<UID_HEX>\n"
 *
 *    Master -> Slave:
 *      "OK_E\n"   -> Entrada registrada  -> Verde 2x pisca rapido
 *      "OK_S\n"   -> Saida registrada    -> Verde 1x pisca lento
 *      "OK\n"     -> Associacao/generico -> Verde 1x pisca
 *      "DENIED\n" -> Nao cadastrado      -> Vermelho longo
 * =============================================
 */

#include <SPI.h>
#include <Adafruit_PN532.h>

// PN532 SS/CS: D4 (GPIO4)
#define PN532_SS  4

// LED RGB conforme sua ligacao fisica na protoboard:
#define LED_R  15  // D8 = GPIO15 (Fio Vermelho)
#define LED_G  16  // D0 = GPIO16 (Fio Verde)
#define LED_B  5   // D1 = GPIO5  (Fio Azul)

// Inicializa o PN532 via Hardware SPI nativo do ESP8266
Adafruit_PN532 nfc(PN532_SS);

// Cooldown para evitar leituras duplicadas da mesma tag
String ultimoUID = "";
unsigned long ultimaLeitura = 0;
const unsigned long COOLDOWN_MS = 1500;

// Controle do pulso azul de espera (non-blocking)
unsigned long ultimoPulsoAzul = 0;
bool estadoAzul = false;
const unsigned long INTERVALO_PULSO_AZUL = 1200; // ms entre liga/desliga

// ====================================================================
// Funcoes de cor do LED RGB
// ====================================================================

void setRGB(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
}

void apagar() { setRGB(false, false, false); }

void piscaVerde(int vezes, int duracao = 120) {
  for (int i = 0; i < vezes; i++) {
    setRGB(false, true, false); delay(duracao);
    apagar();                    delay(90);
  }
}

void piscaVermelho(int duracao = 700) {
  setRGB(true, false, false); delay(duracao);
  apagar();
}

void piscaBranco(int duracao = 50) {
  setRGB(true, true, true); delay(duracao);
  apagar();
}

void piscaAzulBoot(int vezes = 3) {
  for (int i = 0; i < vezes; i++) {
    setRGB(false, false, true); delay(80);
    apagar();                    delay(80);
  }
}

void piscaVermelhoAlerta() {
  // Pisca vermelho rapidamente em loop infinito (erro critico)
  while (1) {
    setRGB(true, false, false); delay(150);
    apagar();                    delay(150);
  }
}

// ====================================================================
void setup() {
  // Serial nativa: comunicacao com a Master (TX=GPIO1, RX=GPIO3)
  Serial.begin(9600);

  // LED RGB
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  apagar();

  // LED onboard (nao usado para feedback, apenas desligado)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // Azul ligado durante inicializacao
  setRGB(false, false, true);

  nfc.begin();

  uint32_t versao = nfc.getFirmwareVersion();
  if (!versao) {
    Serial.println("ERRO:PN532_NAO_ENCONTRADO");
    piscaVermelhoAlerta(); // nao retorna
  }

  // Configura para leitura Mifare/NTAG/ISO14443A
  nfc.SAMConfig();

  // 3 piscadas azuis: inicializacao OK
  piscaAzulBoot(3);

  // Anuncia para a Master que o leitor e a Slave estao online
  Serial.println(F("SLAVE_ONLINE:PN532_OK"));

  // Entra em modo espera: azul suave piscando no loop
  estadoAzul = true;
  setRGB(false, false, true);
  ultimoPulsoAzul = millis();
}

// ====================================================================
void loop() {

  // 1. PULSO AZUL DE ESPERA (non-blocking)
  if (millis() - ultimoPulsoAzul > INTERVALO_PULSO_AZUL) {
    ultimoPulsoAzul = millis();
    estadoAzul = !estadoAzul;
    digitalWrite(LED_B, estadoAzul ? HIGH : LOW);
    digitalWrite(LED_R, LOW);
    digitalWrite(LED_G, LOW);
  }

  // 2. ESCUTA RESPOSTA DA MASTER
  if (Serial.available()) {
    String resposta = Serial.readStringUntil('\n');
    resposta.trim();

    // Envia confirmacao (ACK) de volta para a Master mostrar no terminal do PC
    Serial.println("ACK:" + resposta);

    if (resposta == "OK_E") {
      // ENTRADA: 2 piscadas verdes rapidas
      piscaVerde(2, 120);

    } else if (resposta == "OK_S") {
      // SAIDA: 1 piscada verde mais lenta
      piscaVerde(1, 350);

    } else if (resposta == "OK") {
      // Generico (associacao de cartao, etc): 1 piscada verde
      piscaVerde(1, 180);

    } else if (resposta == "DENIED") {
      // NAO CADASTRADO: vermelho longo
      piscaVermelho(700);
    }

    // Volta para pulso azul de espera
    estadoAzul = true;
    setRGB(false, false, true);
    ultimoPulsoAzul = millis();
  }

  // 3. LEITURA DE CARTAO / TAG RFID
  uint8_t uid[7];
  uint8_t uidTamanho;

  // Timeout curto (50ms) para manter o loop responsivo
  bool sucesso = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidTamanho, 50);

  if (sucesso) {
    String uidHex = "";
    for (uint8_t i = 0; i < uidTamanho; i++) {
      if (uid[i] < 0x10) uidHex += "0";
      uidHex += String(uid[i], HEX);
    }
    uidHex.toUpperCase();

    unsigned long agora = millis();

    // Ignora se for o mesmo cartao dentro do cooldown
    if (uidHex != ultimoUID || (agora - ultimaLeitura) > COOLDOWN_MS) {
      ultimoUID    = uidHex;
      ultimaLeitura = agora;

      // Branco rapido: cartao lido, aguardando resposta
      piscaBranco(60);

      // Envia para a Master
      Serial.println("CARD:" + uidHex);
    }
  }
}
