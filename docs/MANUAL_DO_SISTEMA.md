# Manual de Operação e Guia do Usuário
### Ponto Digital Maker (V2.0)
**MakerSpace UNIFEI**

---

## 1. Interface com o Usuário (Display LCD 20x4)

O display LCD exibe informações contínuas em 4 linhas de 20 colunas. Cada tela foi desenhada com alinhamento e limpeza automática de caracteres residuais:

### 1.1. Tela de Repouso (Modo Normal)
```
+--------------------+
|20/09/2026    15:42 |  <- Linha 0: Data e Hora atualizadas via NTP
|  PONTO ELETRONICO  |  <- Linha 1: Título centralizado
|  Aproxime o dedo   |  <- Linha 2: Instrução principal
|WiFi: Conectado (OK)|  <- Linha 3: Status de rede ou fila de pendências
+--------------------+
```

* **Indicação de Pendências:** Se a rede Wi-Fi estiver instável ou fora do ar, a linha 3 exibe dinamicamente:
  `* Pendentes: 4` (mostrando a quantidade exata de batidas guardadas na memória).
* **Indicação de Desconexão:** Se a conexão cair e não houver pendências:
  `WiFi: Desconectado!` (o sistema tentará se reconectar sozinho a cada 30 segundos).

### 1.2. Telas de Sucesso (Entrada e Saída)
```
+--------------------+      +--------------------+
|Ola, Victor         |      |Ola, Victor         |
|  >>> ENTRADA <<<   |  ou  |  >>>  SAIDA  <<<   |
|20/09/2026 15:42:10 |      |20/09/2026 18:30:05 |
|ID #12 - Registrado!|      |ID #12 - Registrado!|
+--------------------+      +--------------------+
```

### 1.3. Telas de Alerta e Negação
```
+--------------------+      +--------------------+
|   ACESSO NEGADO!   |      |   ACESSO NEGADO!   |
|Digital nao cadast. |  ou  | Cartao nao cadast. |
|Tente novamente...  |      |Tente novamente...  |
|                    |      |                    |
+--------------------+      +--------------------+
```

---

## 2. Menu Administrativo Serial (Via PC)

Para acessar o painel de administração:
1. Conecte o cabo micro-USB da **Master** no computador.
2. Abra o **Serial Monitor** da Arduino IDE (ou PuTTY / screen).
3. Configure a velocidade para **`9600 baud`** e fim de linha para **`Newline`** ou **`Both NL & CR`**.
4. O menu será exibido automaticamente:

```text
--- MENU ADMINISTRATIVO ---
1 - CADASTRAR nova pessoa (ID automatico)
2 - RECADASTRAR digital de alguem (mesma pessoa, nova digital)
3 - APAGAR UMA digital especifica
4 - APAGAR TODAS as digitais
5 - LISTAR digitais cadastradas
6 - LISTAR arquivo de nomes (bruto)
7 - EDITAR/CORRIGIR nome de um ID
8 - VER/REENVIAR pendentes agora
9 - CADASTRAR/ASSOCIAR cartao RFID a um ID
A - LISTAR cartoes RFID cadastrados
B - RESTAURAR nomes de backup (29 integrantes)
Digite a opcao:
```

### Detalhamento das Funções

* **`1` — Cadastrar Nova Pessoa:**
  * O sistema varre o sensor óptico (capacidade: 1 a 127) e encontra o primeiro slot vago.
  * Solicita a primeira leitura da digital no sensor óptico.
  * Pede para remover o dedo e posicionar o mesmo dedo novamente (confirmação).
  * Cria o modelo biométrico, armazena no sensor e solicita a digitação do **Nome** no terminal.
  * Inicializa o status da pessoa como `S` (fora do laboratório).

* **`2` — Recadastrar Digital de Alguém:**
  * Mantém o mesmo número de ID e o mesmo nome da pessoa.
  * Substitui apenas as amostras ópticas da digital (útil se o usuário mudou o dedo ou está tendo dificuldade de leitura).

* **`3` — Apagar Uma Digital Específica:**
  * Solicita o ID e remove o registro correspondente tanto do sensor AS608 quanto dos arquivos `/pessoas.txt` e `/status.txt`.

* **`4` — Apagar Todas as Digitais (Formatação Completa):**
  * Limpa a memória interna do sensor óptico e zera os arquivos locais do LittleFS. Requer confirmação digitando `S` no terminal (com timeout de 30 segundos).

* **`5` — Listar Digitais Cadastradas:**
  * Consulta o sensor óptico e compara com os arquivos de banco de dados, exibindo:
    `-> ID #12 - Victor [Dentro]` ou `[Fora]`.

* **`6` — Listar Arquivo de Nomes (Bruto):**
  * Imprime o conteúdo cru do arquivo `/pessoas.txt` no formato `ID;Nome`.

* **`7` — Editar/Corrigir Nome de um ID:**
  * Permite corrigir a grafia do nome de um usuário sem precisar pedir para ele colocar a digital novamente.

* **`8` — Ver/Reenviar Pendentes Agora:**
  * Exibe todas as batidas que ocorreram sem conexão com a internet e força uma tentativa imediata de envio para a planilha.

* **`9` — Cadastrar/Associar Cartão RFID a um ID:**
  * Solicita o ID do membro (1 a 127).
  * Abre uma janela de escuta de **15 segundos**: basta aproximar a tag/cartão no leitor PN532.
  * A Slave captura o UID, envia para a Master e o vínculo é salvo no arquivo `/cartoes.txt`.
  * Também permite digitar o UID manualmente pelo teclado do PC.

* **`A` — Listar Cartões RFID Cadastrados:**
  * Lista todos os UIDs associados aos respectivos nomes e números de identificação:
    `Tag UID: CFDBC5C4 -> ID #12 (Victor)`.

* **`B` — Restaurar Nomes de Backup (29 Integrantes):**
  * Função de contingência de fábrica. Escreve instantaneamente a lista completa dos 29 integrantes originais da equipe no LittleFS, corrigindo falhas de inicialização com um único clique.

> [!NOTE]
> **Proteção por Timeout:** Todas as rotinas que aguardam digitação no terminal serial possuem um temporizador de segurança de **60 segundos** (ou 30s para confirmações). Se o operador abrir uma opção e esquecer o terminal aberto, o sistema cancela a operação sozinho e volta para a tela de ponto.

---

## 3. Cadastro Rápido sem PC (Botão Físico FLASH)

Para cadastrar um novo integrante sem precisar abrir o computador:
1. Pressione o **Botão FLASH (D3)** na placa Master por 1 segundo.
2. O LCD indicará o primeiro ID vago e solicitará:
   * *1ª Leitura:* Coloque o dedo no sensor.
   * *Aviso:* Remova o dedo.
   * *2ª Leitura:* Coloque o mesmo dedo novamente.
3. Se as digitais baterem, o ID é gravado e o sistema atribui temporariamente o nome `"Pessoa <ID>"` (ex: `"Pessoa 15"`).
4. Mais tarde, o administrador pode usar a opção `7` do menu serial no computador para substituir pelo nome real.

---

## 4. Fechamento Automático de Turno (23:00h)

Para evitar que membros do laboratório esqueçam o ponto aberto e fiquem registrados como presentes durante a madrugada:
* Todos os dias, às **23:00** (verificado via relógio NTP), o sistema executa automaticamente a rotina `forcarSaidaAutomatica()`.
* O arquivo `/status.txt` é varrido.
* Qualquer usuário com status `E` (Entrada pendente de saída) recebe uma batida automática de saída (`S`).
* O evento é enviado para a planilha Google Sheets com o carimbo de data/hora oficial das 23h.

---

## 5. Estrutura do Banco de Dados Local (LittleFS)

O sistema utiliza a partição Flash interna SPIFFS/LittleFS do ESP8266 com 4 arquivos principais:

| Arquivo | Formato dos Registros | Exemplo de Conteúdo |
|---|---|---|
| **`/pessoas.txt`** | `<ID>;<Nome>\n` | `12;Victor\n13;Andre Mota\n` |
| **`/status.txt`** | `<ID>;<E_ou_S>\n` | `12;E\n13;S\n` |
| **`/cartoes.txt`** | `<UID_HEX>;<ID>\n` | `CFDBC5C4;12\nD6D697AC;3\n` |
| **`/pendentes.txt`**| `<DataHora>;<ID>;<Nome>;<Status>;<Tentativas>\n` | `20/09/2026 15:30:00;12;Victor;E;1\n` |

### Tolerância a Falhas de Internet (Fila Offline)
* Toda vez que um ponto é batido, o ESP8266 tenta disparar uma requisição HTTPS para o Google Apps Script.
* Se a requisição falhar (sem sinal de Wi-Fi, roteador fora do ar ou timeout do Google):
  1. A batida é **armazenada imediatamente em `/pendentes.txt`**.
  2. O LCD e os bips confirmam o registro com sucesso para o usuário.
  3. No fundo (*background*), a Master tenta reenviar os pendentes a cada **30 segundos**.
  4. Cada registro pode falhar até **3 vezes** antes de ser descartado para não entupir a memória em casos de erro de sintaxe.

---

## 6. Integração com a Planilha Google Sheets

A comunicação com a nuvem é feita via **Google Apps Script WebApp**:
* **URL:** Configurada na constante `WEBAPP_URL` no cabeçalho do `ponto.ino`.
* **Método:** `POST` / `GET` HTTPS seguro via `WiFiClientSecure` (com redirecionamento automático `followRedirects(true)`).
* **Parâmetros Enviados (URL Encoded):**
  * `dataHora`: Timestamp no formato `DD/MM/AAAA HH:MM:SS`
  * `id`: Número identificador único do membro (1 a 127)
  * `nome`: Nome completo ou de identificação
  * `status`: `E` (Entrada) ou `S` (Saída)
* **Resposta Esperada:** Código HTTP `200` com texto `"Ponto registrado com sucesso"`.
