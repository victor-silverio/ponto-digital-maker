# ☁️ Integração com Google Sheets (Cloud Backend)

Este diretório contém o backend em nuvem do **Ponto Digital Maker**, implementado sem custos através do **Google Apps Script** e **Google Sheets**.

---

## 📋 Como Funciona
1. O microcontrolador **Master (ESP8266)** processa a identificação do membro (Biometria ou RFID).
2. Ele monta uma requisição segura HTTPS com os parâmetros `dataHora`, `id`, `nome` e `status`.
3. A requisição atinge o endpoint WebApp do Google Apps Script.
4. O script aciona uma trava de concorrência (`LockService`), abre a planilha vinculada e adiciona uma nova linha com o carimbo oficial.
5. Se a requisição for concluída, o ESP recebe código de sucesso. Se o Wi-Fi falhar, a batida fica guardada na memória flash local (`/pendentes.txt`) até a rede voltar.

---

## 🚀 Passo a Passo de Configuração

### 1. Criar a Planilha
1. Abra o [Google Sheets](https://sheets.google.com) e crie uma planilha em branco.
2. Nomeie-a como preferir (ex: `Ponto Eletronico MakerSpace`).

### 2. Adicionar o Script
1. Na planilha, acesse o menu **Extensões** > **Apps Script**.
2. Apague qualquer código presente no editor.
3. Copie todo o conteúdo do arquivo [`google_apps_script.js`](./google_apps_script.js) e cole no editor.
4. Salve o projeto (Ctrl + S).

### 3. Publicar como WebApp
1. No canto superior direito, clique no botão azul **Implantar** (Deploy) > **Nova implantação**.
2. Clique no ícone de engrenagem ao lado de *Selecionar tipo* e escolha **App da Web**.
3. Configure os seguintes campos:
   * **Descrição:** `Ponto Digital Maker API`
   * **Executar como:** `Eu (<seu-email>)`
   * **Quem tem acesso:** `Qualquer pessoa` *(obrigatoriamente "Qualquer pessoa" para que o ESP8266 possa enviar dados sem autenticação OAuth interativa)*.
4. Clique em **Implantar**.
5. Conceda as permissões de acesso solicitadas pela sua conta Google.
6. Copie a **URL do app da Web** gerada (exemplo: `https://script.google.com/macros/s/AKfycb.../exec`).

### 4. Configurar no Firmware
Abra o arquivo [`firmware/ponto/ponto.ino`](../firmware/ponto/ponto.ino) e cole a sua URL na linha correspondente:
```cpp
const char* WEBAPP_URL = "https://script.google.com/macros/s/SUA_URL_AQUI/exec";
```
Pronto! Toda batida de ponto será refletida instantaneamente na sua planilha.
