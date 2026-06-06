# Mozilla CA SPIFFS Updater

Projeto ESP-IDF independente para gerenciar, em tempo de execucao, um bundle publico de CAs da Mozilla armazenado em uma particao SPIFFS.

O codigo reutilizavel fica em componentes ESP-IDF: `components/ca_manager`
gerencia o ciclo de vida do bundle, `components/manifest_file_updater` baixa e
verifica artefatos descritos por manifestos, e `components/ca_manifest_updater`
adapta esse fluxo para bundles de CA. O diretorio `main/` contem um app de
exemplo para ESP32/ESP32-S3 que configura Wi-Fi, diagnosticos e console.

Ele usa a mesma versao detectada no projeto principal aberto: ESP-IDF `6.2.0` (`dependencies.lock`). O devcontainer deste projeto fica pinado em `espressif/idf:v6.2.0`.

## Fluxo

1. Monta a particao SPIFFS `ca_store`.
2. Tenta carregar `/spiffs/bundle_ca.bin` e ativar com `esp_crt_bundle_set()`.
3. Se nao houver bundle valido em SPIFFS, usa o bundle embutido do ESP-IDF como fallback.
4. Conecta ao Wi-Fi configurado.
5. Baixa `bundle_ca.manifest.json` de uma URL HTTPS configurada.
6. Compara `version` com a versao salva em SPIFFS.
7. Se a versao mudou, compara o `sha256` do manifest com o bundle ativo em memoria.
8. Se o bundle ativo nao bater, `manifest_file_updater` baixa o `url` do manifest para um arquivo temporario em SPIFFS.
9. Confere tamanho e SHA-256 do binario baixado antes de entregar o arquivo ao adaptador de CA.
10. `ca_manifest_updater` chama `ca_manager_apply_file()`.
11. `ca_manager` valida o binario chamando `esp_crt_bundle_set()` e promove para `/spiffs/bundle_ca.bin` somente se as validacoes passarem.
12. Salva a nova versao em `/spiffs/bundle_ca.version`.
13. Reinicia o dispositivo com `esp_restart()` apos uma atualizacao confirmada.
14. Se configurado, testa URLs HTTPS de diagnostico e registra o resultado no log.
15. Se habilitado, inicia um console de diagnostico para testes manuais.

## Configuracao

Abra `idf.py menuconfig` e ajuste:

- `Mozilla CA SPIFFS updater example > Wi-Fi SSID`
- `Mozilla CA SPIFFS updater example > Wi-Fi password`
- `Mozilla CA SPIFFS updater example > Mozilla CA bundle manifest URL`
- `Mozilla CA SPIFFS updater example > Enable CA updater diagnostic console`
- `Mozilla CA SPIFFS updater example > Boot HTTPS diagnostic URLs`

As opcoes de storage do componente ficam em `CA manager`. Timeouts e buffers do
download generico ficam em `Manifest file updater`.

A URL deve servir um arquivo binario no formato `x509_crt_bundle` do ESP-IDF, gerado pelo script `components/mbedtls/esp_crt_bundle/gen_crt_bundle.py` a partir das CAs publicas da Mozilla. Nao use um arquivo PEM concatenado diretamente; `esp_crt_bundle_set()` espera o formato binario ordenado do ESP-IDF. No ESP-IDF v6.2, mantenha o bundle ordenado por subject name para a busca binaria em tempo de execucao.

O diretorio `tools/certificate_prepare` contem um preparador Python para gerar esse `bundle_ca.bin` a partir do `certdata.txt` da Mozilla, com suporte a download automatico e override por arquivo local.

Para GitHub Releases, configure URLs estaveis usando `latest/download`:

```text
https://github.com/maujabur/mozilla_ca_spiffs_updater/releases/latest/download/bundle_ca.manifest.json
```

O script de release gera e publica `bundle_ca.manifest.json`, `bundle_ca.bin`, `bundle_ca.version` e `bundle_ca.sha256`:

```bash
tools/certificate_prepare/create_release.sh 1.0.1
```

Para regenerar o bundle imediatamente antes de publicar:

```bash
tools/certificate_prepare/create_release.sh 1.0.1 --prepare
```

## Console de diagnostico

O app pode testar URLs HTTPS automaticamente no boot, sem depender de entrada
interativa pela serial. Configure uma lista separada por virgulas em `Boot HTTPS
diagnostic URLs`. Depois que o Wi-Fi conecta, o log mostra status HTTP,
`content_length` e resultado TLS/HTTP de cada URL.

Quando `Enable CA updater diagnostic console` esta habilitado, o app tambem
inicia um prompt `ca>` depois do check de boot. Use `help` para listar comandos
e `ca help` para os comandos especificos do updater. Essa opcao fica desligada
por padrao porque alguns monitores seriais conseguem ler logs, mas nao enviam
comandos de forma confiavel.

```text
ca status
ca https-test <url>
ca fetch-manifest <url>
ca check [manifest_url]
ca update [manifest_url]
```

`ca https-test` abre uma conexao HTTPS, segue redirects e imprime status HTTP e
tamanho anunciado. Ele e util para testar GitHub Releases, mirrors e servidores
com cadeias de certificados diferentes sem trocar o firmware.

### Monitor interativo no ESP32-S3 USB Serial/JTAG

Para enviar comandos ao prompt `ca>` pelo USB nativo do ESP32-S3, mantenha o
console do ESP-IDF roteado para o controlador USB Serial/JTAG. O projeto fixa
isso em `sdkconfig.defaults` com `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`; se o
`sdkconfig` ja existir, confirme em `idf.py menuconfig`:

```text
Component config -> ESP System Settings -> Channel for console output -> USB Serial/JTAG Controller
```

No host Linux, o usuario que abre o Dev Container deve pertencer ao grupo
`dialout`, pois a permissao da `/dev/ttyACM0` nasce no host:

```bash
sudo usermod -aG dialout seu_usuario_host
```

Depois de alterar o grupo, encerre a sessao do usuario no host e entre de novo.
Dentro do Dev Container o usuario normalmente e `root`, entao `sudo` nao e
necessario para acessar a porta.

Para evitar travamento de escrita causado por sinais DTR/RTS na USB Serial/JTAG,
abra o monitor em um terminal do Dev Container com o script da raiz do projeto:

```bash
./monitor.sh
```

Por padrao ele usa `/dev/ttyACM0` e executa:

```bash
idf.py -p /dev/ttyACM0 monitor --no-reset
```

Para usar outra porta:

```bash
./monitor.sh /dev/ttyACM1
```

Se a selecao do canal de console foi alterada em um `sdkconfig` ja gerado, rode
`idf.py fullclean` e faca um novo build antes de testar o prompt interativo.

## Exemplo com chunks do `esp_http_client`

Quando o download for conduzido por um handler de eventos, mantenha o contexto entre eventos e envie cada bloco recebido ao `ca_manager`:

```c
#include "ca_manager.h"
#include "esp_check.h"
#include "esp_http_client.h"

static ca_manager_update_ctx_t *s_ca_update_ctx;

static esp_err_t ca_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_ca_update_ctx == NULL) {
            int64_t len = esp_http_client_get_content_length(evt->client);
            size_t expected_size = len > 0 ? (size_t)len : 0;
            ESP_RETURN_ON_ERROR(ca_manager_update_begin(&s_ca_update_ctx, expected_size),
                                "ca_example", "begin failed");
        }
        return ca_manager_update_write(s_ca_update_ctx, evt->data, evt->data_len);

    case HTTP_EVENT_ON_FINISH:
        if (s_ca_update_ctx != NULL) {
            ca_manager_update_ctx_t *ctx = s_ca_update_ctx;
            s_ca_update_ctx = NULL;
            return ca_manager_update_finish(ctx, true);
        }
        break;

    case HTTP_EVENT_DISCONNECTED:
        ca_manager_update_abort(s_ca_update_ctx);
        s_ca_update_ctx = NULL;
        break;

    default:
        break;
    }

    return ESP_OK;
}
```

O fluxo pronto `ca_manager_update_from_http_client(url, true)` usa a mesma API de streaming internamente.

## Build no container

No VS Code, abra a pasta `mozilla_ca_spiffs_updater` no Dev Container.

Depois, dentro do container:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Observacoes de integracao

- A particao SPIFFS tem `0x80000` bytes no `partitions.csv`.
- O app mantem o buffer do bundle ativo em RAM, pois o ESP-IDF recebe um ponteiro para o binario do bundle.
- Clientes HTTPS, OTA e MQTT que usam `.crt_bundle_attach = esp_crt_bundle_attach` passam a usar o bundle ativado por `esp_crt_bundle_set()`.
- Se o download falhar ou o bundle novo for invalido, o arquivo ativo anterior permanece preservado.
- Para integracao com atualizadores baseados em manifesto, prefira `manifest_file_updater` + `ca_manager_apply_file(path, restart_on_success)`: a camada externa baixa e verifica o artefato, e o `ca_manager` valida semanticamente o bundle antes de promove-lo.
