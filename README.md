# Mozilla CA SPIFFS Updater

Projeto ESP-IDF independente para gerenciar, em tempo de execucao, um bundle publico de CAs da Mozilla armazenado em uma particao SPIFFS.

O codigo reutilizavel fica em componentes ESP-IDF: `components/ca_manager`
gerencia o ciclo de vida do bundle, `components/manifest_file_updater` baixa e
verifica artefatos descritos por manifestos, e `components/ca_manifest_updater`
adapta esse fluxo para bundles de CA. O diretorio `main/` contem um app de
exemplo para ESP32/ESP32-S3 que configura Wi-Fi e diagnosticos de boot.

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

## Configuracao

Abra `idf.py menuconfig` e ajuste:

- `Example consumer app > Wi-Fi > Wi-Fi SSID`
- `Example consumer app > Wi-Fi > Wi-Fi password`
- `Example consumer app > CA bundle update > CA bundle manifest URL`
- `Example consumer app > HTTPS diagnostics > Boot HTTPS diagnostic URLs`

As opcoes de storage do componente ficam em `CA manager`. Timeouts e buffers do
download generico ficam em `Manifest file updater`.

A URL deve servir um arquivo binario no formato `x509_crt_bundle` do ESP-IDF, gerado pelo script `components/mbedtls/esp_crt_bundle/gen_crt_bundle.py` a partir das CAs publicas da Mozilla. Nao use um arquivo PEM concatenado diretamente; `esp_crt_bundle_set()` espera o formato binario ordenado do ESP-IDF. No ESP-IDF v6.2, mantenha o bundle ordenado por subject name para a busca binaria em tempo de execucao.

O diretorio `tools/certificate_prepare` contem um preparador Python para gerar esse `bundle_ca.bin` a partir do `certdata.txt` da Mozilla, com suporte a download automatico e override por arquivo local.

Para publicar o bundle, use um repositorio separado de artefatos. Uma estrutura
simples e compativel com GitHub Pages seria:

```text
mozilla_ca_spiffs_artifacts/
  ca/stable/
    bundle_ca.bin
    bundle_ca.manifest.json
    bundle_ca.version
    bundle_ca.sha256
```

Com GitHub Pages habilitado nesse repo, a URL configurada no firmware fica:

```text
https://maujabur.github.io/mozilla_ca_spiffs_artifacts/ca/stable/bundle_ca.manifest.json
```

Para gerar, commitar e publicar os arquivos no repo de artefatos:

```bash
tools/certificate_prepare/publish_artifacts.sh 1.0.9
```

O script garante que `../mozilla_ca_spiffs_artifacts` seja um clone Git, gera o
bundle, atualiza `ca/stable`, cria o commit e faz `git push`. Internamente ele
tambem registra uma URL extra no campo `urls`, preparando o caminho para fallback
por mirrors quando o firmware passar a consumir essa lista.

## Diagnostico HTTPS

O app pode testar URLs HTTPS automaticamente no boot, sem depender de entrada
interativa pela serial. Configure uma lista separada por virgulas em `Boot HTTPS
diagnostic URLs`. Depois que o Wi-Fi conecta, o log mostra status HTTP,
`content_length` e resultado TLS/HTTP de cada URL.

## Aplicacao de arquivo verificado

Quando uma camada externa ja baixou e verificou o artefato, entregue o arquivo
local ao `ca_manager`:

```c
#include "ca_manager.h"

ESP_ERROR_CHECK(ca_manager_apply_file("/spiffs/manifest_artifact.tmp", true));
```

O componente `ca_manager` nao faz HTTP. Downloads, redirects, verificacao de
tamanho e SHA-256 pertencem a `manifest_file_updater`.

## Uso em outros projetos

Os componentes podem ser consumidos diretamente deste repositorio via ESP-IDF
Component Manager. Para usar o fluxo completo de CA, adicione no
`main/idf_component.yml` do projeto consumidor:

```yaml
dependencies:
  ca_manifest_updater:
    git: https://github.com/maujabur/mozilla_ca_spiffs_updater.git
    path: components/ca_manifest_updater
    version: lib-v0.1.0
```

`ca_manifest_updater` declara `ca_manager` e `manifest_file_updater` como
dependencias do mesmo repositorio. Se o projeto precisar apenas de um bloco
menor, tambem pode depender diretamente de um componente especifico:

```yaml
dependencies:
  manifest_file_updater:
    git: https://github.com/maujabur/mozilla_ca_spiffs_updater.git
    path: components/manifest_file_updater
    version: lib-v0.1.0
```

Use tags `lib-vX.Y.Z` para versionar a API e implementacao dos componentes. O
versionamento do bundle de CAs publicado para o dispositivo deve ser separado
do versionamento da biblioteca.

Cada componente expoe a propria versao em runtime:

```c
ESP_LOGI(TAG, "ca_manager=%s", ca_manager_get_component_version());
ESP_LOGI(TAG, "manifest_file_updater=%s", manifest_file_updater_get_component_version());
ESP_LOGI(TAG, "ca_manifest_updater=%s", ca_manifest_updater_get_component_version());
```

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
- O roadmap para tornar `manifest_file_updater` uma base mais generica fica em `docs/superpowers/specs/2026-06-05-manifest-file-updater-design.md`.
