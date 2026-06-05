# Mozilla CA SPIFFS Updater

Projeto ESP-IDF independente para gerenciar, em tempo de execucao, um bundle publico de CAs da Mozilla armazenado em uma particao SPIFFS.

O codigo reutilizavel fica em `components/ca_manager`. O diretorio `main/` contem um app de exemplo para ESP32/ESP32-S3 que configura Wi-Fi e demonstra o fluxo de atualizacao.

Ele usa a mesma versao detectada no projeto principal aberto: ESP-IDF `6.2.0` (`dependencies.lock`). O devcontainer deste projeto fica pinado em `espressif/idf:v6.2.0`.

## Fluxo

1. Monta a particao SPIFFS `ca_store`.
2. Tenta carregar `/spiffs/bundle_ca.bin` e ativar com `esp_crt_bundle_set()`.
3. Se nao houver bundle valido em SPIFFS, usa o bundle embutido do ESP-IDF como fallback.
4. Conecta ao Wi-Fi configurado.
5. Baixa `bundle_ca.version` de uma URL HTTPS configurada e compara com a versao salva em SPIFFS.
6. Se a versao mudou, baixa um novo `bundle_ca.bin` para `/spiffs/bundle_ca.bin.tmp`.
7. Valida o binario baixado chamando `esp_crt_bundle_set()`.
8. Promove o arquivo temporario para `/spiffs/bundle_ca.bin` somente se a validacao passar.
9. Salva a nova versao em `/spiffs/bundle_ca.version`.
10. Reinicia o dispositivo com `esp_restart()` apos uma atualizacao confirmada.

## Configuracao

Abra `idf.py menuconfig` e ajuste:

- `Mozilla CA SPIFFS updater example > Wi-Fi SSID`
- `Mozilla CA SPIFFS updater example > Wi-Fi password`
- `Mozilla CA SPIFFS updater example > Mozilla CA bundle binary URL`
- `Mozilla CA SPIFFS updater example > Mozilla CA bundle version URL`

As opcoes de storage do componente ficam em `CA manager`.

A URL deve servir um arquivo binario no formato `x509_crt_bundle` do ESP-IDF, gerado pelo script `components/mbedtls/esp_crt_bundle/gen_crt_bundle.py` a partir das CAs publicas da Mozilla. Nao use um arquivo PEM concatenado diretamente; `esp_crt_bundle_set()` espera o formato binario ordenado do ESP-IDF. No ESP-IDF v6.2, mantenha o bundle ordenado por subject name para a busca binaria em tempo de execucao.

O diretorio `tools/certificate_prepare` contem um preparador Python para gerar esse `bundle_ca.bin` a partir do `certdata.txt` da Mozilla, com suporte a download automatico e override por arquivo local.

Para GitHub Releases, configure URLs estaveis usando `latest/download`:

```text
https://github.com/maujabur/mozilla_ca_spiffs_updater/releases/latest/download/bundle_ca.version
https://github.com/maujabur/mozilla_ca_spiffs_updater/releases/latest/download/bundle_ca.bin
```

O script de release gera e publica os dois assets:

```bash
tools/certificate_prepare/create_release.sh 1.0.1
```

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
- Para integracao com atualizadores baseados em manifesto, prefira `ca_manager_apply_file(path, restart_on_success)`: a camada externa baixa e verifica o artefato, e o `ca_manager` valida semanticamente o bundle antes de promove-lo.
