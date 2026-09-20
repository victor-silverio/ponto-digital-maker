/**
 * ====================================================================
 * PONTO DIGITAL MAKER - SCRIPT DE INTEGRAÇÃO GOOGLE SHEETS
 * MakerSpace UNIFEI
 * ====================================================================
 * 
 * Este script recebe as requisições HTTP enviadas pela placa Master (ESP8266)
 * e registra automaticamente cada batida de ponto como uma nova linha
 * na planilha do Google Sheets.
 * 
 * --- COMO INSTALAR ---
 * 1. Crie uma nova planilha no Google Sheets (ex: "Registro de Ponto MakerSpace").
 * 2. No menu superior, clique em "Extensões" > "Apps Script".
 * 3. Apague o código existente e cole todo o conteúdo deste arquivo.
 * 4. Clique no ícone de salvar (disquete) ou pressione Ctrl + S.
 * 5. Clique em "Implantar" (botão azul no canto superior direito) > "Nova implantação".
 * 6. Na engrenagem ao lado de "Selecionar tipo", escolha: "App da Web".
 * 7. Preencha as configurações OBRIGATORIAMENTE assim:
 *    - Descrição: "Ponto Digital Maker API"
 *    - Executar como: "Eu" (seu e-mail)
 *    - Quem tem acesso: "Qualquer pessoa" (Anyone)  <-- FUNDAMENTAL!
 * 8. Clique em "Implantar" e conceda as permissões solicitadas pela sua conta Google.
 * 9. Copie a "URL do app da Web" gerada (termina com "/exec").
 * 10. Cole essa URL na constante WEBAPP_URL do arquivo firmware/ponto/ponto.ino!
 * ====================================================================
 */

// Trata requisições HTTP GET (usado pelo firmware do ESP8266)
function doGet(e) {
  return processarRequisicao(e);
}

// Trata requisições HTTP POST (suporte para integrações futuras)
function doPost(e) {
  return processarRequisicao(e);
}

function processarRequisicao(e) {
  try {
    // Bloqueio de concorrência: garante que requisições quase simultâneas
    // não colidam na escrita da mesma linha da planilha
    var lock = LockService.getScriptLock();
    lock.waitLock(30000); // aguarda até 30 segundos
    
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    
    // Se a planilha estiver vazia, cria os cabeçalhos formatados
    if (sheet.getLastRow() === 0) {
      var cabecalhos = [
        "Data e Hora (Local)",
        "ID do Membro",
        "Nome do Integrante",
        "Evento",
        "Carimbo do Servidor"
      ];
      sheet.appendRow(cabecalhos);
      
      // Estilização do cabeçalho
      var headerRange = sheet.getRange(1, 1, 1, cabecalhos.length);
      headerRange.setFontWeight("bold");
      headerRange.setBackground("#2c3e50");
      headerRange.setFontColor("#ffffff");
      headerRange.setHorizontalAlignment("center");
      sheet.setFrozenRows(1);
    }
    
    // Extração dos parâmetros recebidos da URL
    var params = e ? e.parameter : {};
    var dataHora = params.dataHora || Utilities.formatDate(new Date(), "America/Sao_Paulo", "dd/MM/yyyy HH:mm:ss");
    var id       = params.id       || "N/A";
    var nome     = params.nome     || "Desconhecido";
    var status   = params.status   || "Ponto";
    var serverTs = new Date();
    
    // Insere a nova linha com a batida de ponto
    sheet.appendRow([dataHora, id, nome, status, serverTs]);
    
    // Libera o lock
    lock.releaseLock();
    
    // Retorna mensagem de sucesso esperada pelo ESP8266
    return ContentService
      .createTextOutput("Ponto registrado com sucesso")
      .setMimeType(ContentService.MimeType.TEXT);
      
  } catch (erro) {
    return ContentService
      .createTextOutput("Erro ao processar ponto: " + erro.toString())
      .setMimeType(ContentService.MimeType.TEXT);
  }
}
