# Ponto Digital Maker (Versão 2.0)
### Sistema de Controle de Acesso e Ponto Eletrônico Híbrido (Biometria + RFID)
**MakerSpace UNIFEI**

---

## 📌 Visão Geral

O **Ponto Digital Maker V2.0** é uma solução completa de controle de presença e ponto eletrônico desenvolvida para ambientes de laboratório de fabricação digital (*makerspaces*), oficinas e departamentos universitários. 

O sistema opera de forma **distribuída**, utilizando **duas placas NodeMCU (ESP8266)** conectadas em barramento serial ponto a ponto:
* **Master (NodeMCU #1):** Cérebro central responsável pelo leitor biométrico AS608, tela LCD 20x4 I2C, banco de dados local (LittleFS), conexão WiFi, envio de dados em tempo real para o Google Sheets, rotinas de fechamento automático e menu administrativo serial.
* **Slave (NodeMCU #2):** Coprocessador periférico dedicado à interface RFID/NFC via módulo PN532 V3 (barramento SPI de alta velocidade) e à sinalização luminosa de estados através de um LED RGB de catodo comum.

---

## 📂 Estrutura de Arquivos do Projeto

```
/home/vaugusto/Desktop/Ponto_Digital_Maker/
├── README.md                          # Visão geral e instruções rápidas de inicialização
├── LICENSE                            # Licença MIT de código aberto
├── docs/
│   ├── ESPECIFICACAO_TECNICA.md       # Diagrama elétrico completo, pinagens, strapping pins e protocolos
│   ├── MANUAL_DO_SISTEMA.md           # Guia de operação, menu administrativo, cadastros e nuvem
│   └── RELATORIO_EVOLUCAO_V1_V2.md    # Análise comparativa detalhada das mudanças da V1 para a V2
├── firmware/
│   ├── ponto/
│   │   └── ponto.ino                  # Firmware de produção da Placa MASTER
│   └── slave_rfid/
│       └── slave_rfid.ino             # Firmware de produção da Placa SLAVE
├── cloud/
│   ├── google_apps_script.js          # Backend Google Apps Script para Google Sheets
│   └── README.md                      # Guia passo a passo de implantação da nuvem
└── backup/
    └── backup_pessoas.txt             # Cópia de segurança em texto dos integrantes cadastrados
```

---

## ⚡ Destaques da Arquitetura V2

| Recurso | Detalhes Técnicos |
|---|---|
| **Arquitetura Distribuída** | Master-Slave via UART nativa/SoftwareSerial sem gargalo no processador principal |
| **Identificação Dupla** | Biometria Digital (AS608) + Tags/Cartões NFC/RFID (PN532 V3 SPI) |
| **Interface Visual Ampla** | Display LCD 20x4 I2C com centralização automática, relógio constante e avisos |
| **Feedback Audiovisual** | Sinalização por Buzzer diferenciado (Entrada, Saída, Negado) + LED RGB de estados |
| **Tolerância a Falhas** | Armazenamento de registros offline (LittleFS) com reenvio automático ao restaurar o WiFi |
| **Watchdog de Hardware** | Recuperação preventiva automática do sensor biométrico sem necessidade de reboot físico |
| **Segurança Anti-Spam** | Cooldown unificado de 5 segundos contra duplicidade de marcações seguidas |
| **Proteção de Dados** | Backup de fábrica embutido dos 29 integrantes com restauração via menu (Opção B) |

---

## 🚀 Guia Rápido de Instalação e Gravação

### 1. Bibliotecas Necessárias (Arduino IDE)
Instale via **Gerenciador de Bibliotecas**:
* `Adafruit Fingerprint Sensor Library` (para o AS608)
* `LiquidCrystal I2C` (por Frank de Brabander ou Marco Schwartz)
* `Adafruit PN532` (para o módulo RFID da Slave)

### 2. Configurações da IDE (Placa NodeMCU 1.0 ESP-12E)
* **Board:** `NodeMCU 1.0 (ESP-12E Module)`
* **Upload Speed:** `115200` ou `921600`
* **CPU Frequency:** `80 MHz` (ou `160 MHz` para maior rapidez)
* **Flash Size:** `4MB (FS:2MB OTA:~1019KB)` ou `4MB (FS:1MB OTA:~1019KB)`
* ⚠️ **Erase Flash:** **`Only Sketch`** *(fundamental para não apagar o LittleFS)*

### 3. Sequência de Gravação dos Módulos
1. **Configurar a Nuvem (Google Sheets):**
   * Siga o passo a passo em [`cloud/README.md`](cloud/README.md) para criar o script no Google Apps Script e gerar a URL WebApp.
2. **Gravar a SLAVE (NodeMCU #2):**
   * Desconecte temporariamente os fios `TX` e `RX` da Slave antes de plugar o USB (para não conflitar com a porta de gravação).
   * Abra e grave [`firmware/slave_rfid/slave_rfid.ino`](firmware/slave_rfid/slave_rfid.ino).
   * Reconecte os fios `TX` e `RX`.
3. **Configurar e Gravar a MASTER (NodeMCU #1):**
   * Abra [`firmware/ponto/ponto.ino`](firmware/ponto/ponto.ino) e preencha as suas credenciais no topo do arquivo:
     ```cpp
     const char* WIFI_SSID     = "SUA_REDE_WIFI";
     const char* WIFI_PASSWORD = "SUA_SENHA_WIFI";
     const char* WEBAPP_URL    = "https://script.google.com/macros/s/SEU_SCRIPT_ID/exec";
     ```
   * Conecte a Master via USB e grave o sketch.
   * Abra o Serial Monitor a **9600 baud**.
   * Se for a primeira inicialização da placa, digite **`B`** no terminal serial para restaurar o banco de membros inicial no LittleFS.

---

## 📖 Documentação Detalhada
Para obter o esquema elétrico detalhado, pinos, cálculo de resistores e relatórios de projeto, consulte a pasta [`docs/`](docs/):
* [Especificação Técnica Completa](docs/ESPECIFICACAO_TECNICA.md)
* [Manual Operacional do Sistema](docs/MANUAL_DO_SISTEMA.md)
* [Relatório de Evolução V1 ➔ V2](docs/RELATORIO_EVOLUCAO_V1_V2.md)
