# Certificate Prepare

Prepara o `bundle_ca.bin` consumido pelo updater SPIFFS. A saida e um binario no formato `x509_crt_bundle` do ESP-IDF, nao um PEM concatenado.

## Requisitos

- Python 3.
- Ambiente ESP-IDF carregado, com `IDF_PATH` apontando para a instalacao do ESP-IDF.
- Dependencias Python do ESP-IDF instaladas, incluindo `cryptography`.

## Uso

Baixar o `certdata.txt` atual da Mozilla e gerar o bundle:

```bash
python tools/certificate_prepare/prepare.py --output dist/bundle_ca.bin
```

Usar um `certdata.txt` local como override:

```bash
python tools/certificate_prepare/prepare.py \
  --certdata path/to/certdata.txt \
  --output dist/bundle_ca.bin
```

Preservar intermediarios para inspecao:

```bash
python tools/certificate_prepare/prepare.py \
  --certdata path/to/certdata.txt \
  --work-dir dist/certificate_prepare_work \
  --output dist/bundle_ca.bin
```

O arquivo `mozilla_ca.pem` fica no `--work-dir`. O binario final deve ser publicado e referenciado pelo `url` do `bundle_ca.manifest.json`.

## Pacote de artefatos

Para gerar, commitar e publicar o bundle em um repositorio separado de
artefatos, use:

```bash
tools/certificate_prepare/publish_artifacts.sh 1.0.9
```

Esse comando garante que `../mozilla_ca_spiffs_artifacts` seja um clone Git,
gera o bundle, atualiza `ca/stable`, cria o commit e executa `git push`.

Se quiser fazer o processo manualmente, clone o repo de artefatos e gere o
pacote em um diretorio que possa ser commitado nesse repo:

```bash
git clone https://github.com/maujabur/mozilla_ca_spiffs_artifacts.git \
  ../mozilla_ca_spiffs_artifacts
```

```bash
python tools/certificate_prepare/package_artifacts.py 1.0.9 \
  --prepare \
  --out-dir ../mozilla_ca_spiffs_artifacts/ca/stable \
  --base-url https://maujabur.github.io/mozilla_ca_spiffs_artifacts/ca/stable
```

Isso gera:

- `bundle_ca.bin`
- `bundle_ca.manifest.json`
- `bundle_ca.version`
- `bundle_ca.sha256`

Se quiser registrar mirrors para uso futuro, adicione uma ou mais URLs:

```bash
python tools/certificate_prepare/package_artifacts.py 1.0.9 \
  --out-dir ../mozilla_ca_spiffs_artifacts/ca/stable \
  --base-url https://maujabur.github.io/mozilla_ca_spiffs_artifacts/ca/stable \
  --extra-artifact-url https://raw.githubusercontent.com/maujabur/mozilla_ca_spiffs_artifacts/main/ca/stable/bundle_ca.bin
```

O firmware atual ainda consome o campo `url`. O campo `urls`, quando presente,
fica reservado para a etapa de fallback por mirrors.

## Validacao

Por padrao, o script rejeita saidas maiores que `262144` bytes, o mesmo limite de `CONFIG_CA_UPDATER_MAX_BUNDLE_SIZE`.

Opcoes uteis:

- `--max-size`: ajusta o limite de tamanho do binario final.
- `--max-certs`: repassa o limite de certificados para `gen_crt_bundle.py`.
- `--certdata-url`: troca a URL usada no modo download.
- `--gen-crt-bundle`: aponta manualmente para outro `gen_crt_bundle.py`.
