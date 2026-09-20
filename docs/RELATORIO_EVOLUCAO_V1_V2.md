# Relatório Técnico de Evolução e Engenharia
### Comparativo entre a Versão 1.0 (Original) e a Versão 2.0 (Final)
**Projeto: Ponto Digital MakerSpace UNIFEI**

---

## 1. Resumo Executivo da Evolução

A versão original do sistema (**V1.0**, baseada no arquivo `original.txt`) consistia em um protótipo monolítico implementado sobre uma única placa NodeMCU ESP8266. Apesar de funcional para batidas simples de digital, a V1 apresentava limitações críticas de confiabilidade operacional para uso contínuo em laboratório: tela pequena (16x2) com caracteres fantasmas, dependência exclusiva de biometria, travamentos por laços seriais infinitos, perda de sincronia horária ao longo dos dias, ausência de feedback sonoro e impossibilidade de reconexão de Wi-Fi sem reiniciar a placa fisicamente.

A **Versão 2.0 (V2.0)** representou um redesenho arquitetural completo. O sistema foi transformado em uma **arquitetura distribuída Master-Slave com dois microcontroladores**, integrando tecnologia **RFID/NFC de alta velocidade**, display **LCD 20x4 profissional**, sinalização audiovisual inteligente (**Buzzer + LED RGB**), **watchdog de auto-recuperação de hardware** e salvaguarda de dados locais.

---

## 2. Quadro Comparativo Direto (Métricas V1 vs V2)

| Parâmetro de Engenharia | Versão 1.0 (Original) | Versão 2.0 (Atual) | Impacto Prático |
|---|:---:|:---:|---|
| **Volume de Código Total** | 975 linhas (1 arquivo) | **1.998 linhas** (2 firmwares) | +105% de funcionalidades e proteções |
| **Arquitetura de Hardware** | 1x NodeMCU ESP8266 | **2x NodeMCU (Master + Slave)** | Elimina gargalos de tempo real e falta de pinos |
| **Métodos de Identificação** | Apenas Biometria Óptica | **Biometria Óptica + RFID/NFC** | Maior flexibilidade e velocidade de acesso |
| **Display LCD** | 16 colunas x 2 linhas | **20 colunas x 4 linhas** | +150% de área útil de texto com relógio e data |
| **Tratamento de Strings LCD**| Escrita direta (gerava fantasmas)| **Alinhador e Centralizador (20 chars)**| Telas limpas, sem caracteres sobrepostos |
| **Feedback Sonoro** | Inexistente | **Buzzer ativo (D8 Master)** | Tons diferentes para Entrada, Saída e Erro |
| **Feedback Luminoso** | Apenas LED onboard azul | **LED RGB de Alta Visibilidade** | Pulso azul (idle), verde (OK) e vermelho (erro)|
| **Comunicação Interplacas** | Inexistente | **UART com Handshake e ACK** | Auditoria total no monitor serial do PC |
| **Queda de Conexão Wi-Fi** | O sistema parava de enviar | **Reconexão Não-Bloqueante (30s)** | Recuperação automática sem intervenção humana |
| **Derivação de Relógio** | NTP único no boot | **Ressincronização a cada 6 horas** | Relógio oficial de ponto com desvio zero |
| **Proteção contra Travamento**| `while(!Serial.available());` infinito | **Timeouts de 30s e 60s em todas as entradas** | Operador não consegue mais travar o ponto |
| **Watchdog do Sensor AS608** | Inexistente (congelava em ruído) | **Soft-Reset Automático por UART** | Sensor se recupera sozinho sem reiniciar placa |
| **Duplicidade Acidental** | Sem controle (duplo turno rápido) | **Cooldown de 5s Unificado** | Impede registro consecutivo acidental |
| **Visualização de Pendências**| Texto estático genérico | **Contador em Tempo Real (`* Pendentes: X`)** | Visibilidade imediata de dados offline |
| **Recuperação de Desastre** | Redigitação manual de todos | **Opção B: Restauração Instantânea** | 29 integrantes recuperados em 1 segundo |

---

## 3. Detalhamento das 12 Grandes Inovações Técnicas

### Inovação 1: Arquitetura Distribuída Master-Slave
Na V1, todos os periféricos disputavam o único processador do ESP8266 e seu barramento GPIO limitado (apenas 9 pinos úteis). 
* **Na V2:** Dividiu-se a carga de trabalho. A **Master** cuida da lógica de negócio, biometria e nuvem, enquanto a **Slave** monitora a antena RFID em laço de altíssima velocidade e aciona a máquina de estados do LED RGB. A interconexão usa UART a 9600 baud sem perda de pacotes.

### Inovação 2: Integração do Módulo RFID PN532 V3 via SPI
* A V1 não possuía suporte a crachás ou tags.
* Na V2, o leitor PN532 V3 foi integrado em modo **SPI nativo** na placa Slave. O banco de dados da Master ganhou o arquivo `/cartoes.txt` no LittleFS, vinculando o UID hexadecimal de 4 a 7 bytes a um número de ID de membro (`UID;ID`). A aproximação de um cartão é processada e enviada para a nuvem com a mesma prioridade da digital.

### Inovação 3: Expansão e Engenharia de Texto do Display LCD 20x4
* Na V1, a tela era de 16x2. Ao tentar exibir strings variáveis, caracteres antigos permaneciam no fundo ("fantasmas").
* Na V2, o LCD foi migrado para **20x4**. Foram criadas três funções fundamentais:
  * `formatarLinha(s, largura)`: Completa com espaços até exatamente 20 caracteres ou trunca se maior.
  * `formatarCentro(s, largura)`: Calcula o padding matemático para centralização perfeita de títulos.
  * `getLinhaDataHora()`: Mantém a Linha 0 com data e hora atualizadas a cada segundo sem piscar o LCD (*clear-less update*).

### Inovação 4: Sistema de Feedback Audiovisual Completo
* A V1 não emitia nenhum som e dependia exclusivamente do usuário olhar para a tela para saber se o ponto passou.
* Na V2, desenvolveu-se uma máquina de estados audiovisual com:
  * **Buzzer:** 3 bips curtos (Boot pronto), 2 bips curtos (Entrada confirmada), 1 bip médio (Saída confirmada), 1 bip longo (Acesso negado).
  * **LED RGB:** Pulso azul suave (Aguardando), branco instantâneo (Cartão lido), verde duplo (Entrada), verde longo (Saída) e vermelho longo (Não cadastrado).

### Inovação 5: Engenharia Elétrica dos Strapping Pins
* Durante a integração da V2, identificou-se que o pino **D8 (GPIO15)** travava a Master quando conectado à Slave, pois portas UART RX possuem pull-up interno e o GPIO15 exige 0V (LOW) para bootar.
* **Solução de Engenharia:** 
  * A transmissão serial foi realocada para o pino **D0 (GPIO16)** da Master (isento de restrições de boot).
  * O pino **D8 (GPIO15)** da Master passou a controlar o **Buzzer**, cujo circuito para o terra (GND) mantém o GPIO15 naturalmente aterrado no momento do boot.
  * O canal verde do LED na Slave foi movido de D3 (GPIO0 com pull-up) para D0 (GPIO16 neutro).

### Inovação 6: Watchdog de Hardware e Soft-Reset do Sensor Biométrico
* Na V1, interferências eletromagnéticas ou ruídos no barramento serial faziam a biblioteca Adafruit travar em `verifyPassword()` ou falhar indefinidamente, exigindo puxar o cabo da tomada.
* Na V2, implementou-se o **Watchdog Preventivo**:
  * O sistema conta erros consecutivos de leitura. Se atingir 8 erros seguidos ou a cada 2 horas preventivas, dispara `reinicializarSensor()`.
  * É enviado um comando binário de soft-reset direto para o DSP do AS608 via `enviarSoftResetSensor()` e o canal `SoftwareSerial` é reinicializado via código, restaurando o sensor sem reiniciar o ESP.

### Inovação 7: Mecanismo de Cooldown Anti-Bounce (5 segundos)
* Na V1, se uma pessoa repousasse o dedo por mais tempo no leitor, o sistema registrava Entrada e Saída logo em seguida, invertendo seu status acidentalmente.
* Na V2, criou-se uma barreira temporal global `COOLDOWN_REGISTRO_MS = 5000`:
  * Se o mesmo `ID` for detectado (seja por biometria ou por RFID) dentro de 5 segundos após uma marcação confirmada, a segunda leitura é descartada com aviso no log serial.

### Inovação 8: Reconexão de Wi-Fi Não-Bloqueante
* Na V1, a função `conectarWiFi()` só rodava uma única vez dentro de `setup()`. Quedas de sinal desativavam o envio cloud até que a placa fosse desligada da tomada.
* Na V2, o `loop()` possui uma tarefa assíncrona periódica:
  * A cada 30 segundos, se `WiFi.status() != WL_CONNECTED`, tenta reconectar à rede sem travar o processamento das digitais nem o display LCD.

### Inovação 9: Sincronização Periódica de Relógio NTP (6 Horas)
* Na V1, o comando `configTime()` rodava apenas no boot. Com o passar de dias de uso contínuo, o relógio interno do microcontrolador apresentava desvio cumulativo (*drift*).
* Na V2, a variável `ultimaSincNTP` gerencia uma ressincronização automática com os servidores `pool.ntp.org` e `a.st1.ntp.br` a cada **6 horas**, garantindo precisão jurídica nas folhas de ponto.

### Inovação 10: Eliminação de Travamentos por Timeout Serial
* Na V1, comandos como `readnumber()` e `lerNomeViaSerial()` usavam `while(!Serial.available());`. Se o usuário selecionasse uma opção no menu e fechasse o monitor serial, a placa travava completamente.
* Na V2, todas as funções de leitura contam com **temporizadores não-bloqueantes de 60 segundos** (e 30 segundos para confirmações de segurança `S/N`), retornando com mensagem de cancelamento gracioso em caso de inatividade.

### Inovação 11: Exibição Dinâmica de Fila de Pendências
* Na V1, o sistema informava apenas `* PENDENCIAS NA FILA`.
* Na V2, a função `contarPendentes()` abre o arquivo `/pendentes.txt`, calcula a contagem exata de linhas válidas e exibe na tela de espera: `* Pendentes: 3`.

### Inovação 12: Sistema de Backup e Restauração de Fábrica com 1 Toque
* Durante a evolução, os dados de 29 integrantes foram resgatados da memória flash.
* Para blindar o sistema contra acidentes em regravações futuras da IDE, a V2 inclui:
  1. Arquivo de backup em texto plano no repositório (`backup_pessoas.txt`).
  2. Opção **`B`** no menu administrativo serial (`restaurarBackupNomes()`), que recria instantaneamente os 29 integrantes e seus status na partição LittleFS com um único toque de teclado.

---

## 4. Tabela de Novas Funções Implementadas no Firmware

| Nova Função C++ | Arquivo | Responsabilidade |
|---|---|---|
| `formatarLinha()` | `ponto.ino` | Padronização de strings para 20 caracteres |
| `formatarCentro()` | `ponto.ino` | Centralização geométrica de títulos no LCD |
| `getLinhaDataHora()` | `ponto.ino` | Formatação da linha de relógio em tempo real |
| `bip()` / `bipBoot()` | `ponto.ino` | Geração de tons e bips de inicialização |
| `bipEntrada()` / `bipSaida()` | `ponto.ino` | Sons de confirmação de registro de ponto |
| `bipNegado()` | `ponto.ino` | Alarme sonoro de acesso não cadastrado |
| `enviarSoftResetSensor()` | `ponto.ino` | Pulso serial de reset direto no DSP do AS608 |
| `reinicializarSensor()` | `ponto.ino` | Rotina de auto-recuperação do watchdog biométrico |
| `buscarIdPorCartao()` | `ponto.ino` | Consulta relacional UID ➔ ID no LittleFS |
| `salvarCartao()` | `ponto.ino` | Persistência de novas tags RFID no banco local |
| `listarCartoesCadastrados()` | `ponto.ino` | Relatório serial de todos os cartões cadastrados |
| `processarPontoRFID()` | `ponto.ino` | Validador de turnos e orquestrador de eventos RFID |
| `associarCartaoRFID()` | `ponto.ino` | Rotina guiada de pareamento de tag a um membro |
| `contarPendentes()` | `ponto.ino` | Contador instantâneo de itens na fila offline |
| `recadastrarDigital()` | `ponto.ino` | Substituição da amostra biométrica sem alterar ID |
| `restaurarBackupNomes()` | `ponto.ino` | Injeção em lote dos 29 integrantes no LittleFS |
| `setRGB()` / `apagar()` | `slave_rfid.ino` | Controle primitivo de barramento das cores do LED |
| `piscaVerde()` / `piscaVermelho()` | `slave_rfid.ino` | Padrões luminosos temporizados de feedback |
| `piscaBranco()` / `piscaAzulBoot()` | `slave_rfid.ino` | Sinalização de leitura de tag e pronto |

---

## 5. Conclusão

O projeto **Ponto Digital Maker V2.0** evoluiu de um protótipo experimental de bancada para um **equipamento de nível de produção (*appliance*)**, projetado para operar 24 horas por dia, 7 dias por semana, sem supervisão. 

Com as rotinas de auto-recuperação, blindagem elétrica de inicialização, sinalização audiovisual intuitiva e redundância de leitura (Biometria + RFID), o sistema está pronto para ser instalado em seu gabinete definitivo no MakerSpace UNIFEI.
