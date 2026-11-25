# xVibePocketTTS → sherpa-onnx (голос педов на планшете)

Русская Pocket TTS ([genvoice/xVibePocketTTS](https://huggingface.co/genvoice/xVibePocketTTS), дообучение
[kyutai/pocket-tts](https://huggingface.co/kyutai/pocket-tts), базовые веса CC BY 4.0) переводится в ONNX
для движка [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 1.12.40 (C API на Android).

## Шаги (проверено 2026-11-25)

1. Экспортёр [lomotron/pocket-tts-onnx-export](https://github.com/lomotron/pocket-tts-onnx-export) (MIT).
   Правка: при `--config` передавать `language=None` в `export_flow_lm.py` и `export_mimi_and_conditioner.py`.
2. Локальный `config.yaml` из репозитория xVibe: пути `weights_path`/`tokenizer_path` на скачанные файлы,
   удалить строку `default_temperature` (экспортёр её не принимает).
3. `python export.py --config xvibe_local.yaml --quantize`.
4. `python patch_for_sherpa.py <результат экспорта> <папка>` — сдвиг `eos_logit` на −3: sherpa-onnx
   считает фразу оконченной при `eos_logit > -4` (зашито), а xVibe обучен с порогом −1 → обрезанные окончания.
   Вектор `bos_before_voice` экспортёр уже встраивает в `mimi_encoder`.

## Как запускать в sherpa-onnx

- `lm_flow`/`lm_main`/`decoder` — `*_int8.onnx`, `encoder`/`text_conditioner` — обычные; `vocab.json`, `token_scores.json`.
- `GenerationConfig`: `num_steps = 1` (как в родном xVibe), `extra.temperature = 0.5`, голос — `reference_audio` (5–12 с).
- Текст: ударение `+гласная` → гласная + U+0301 (NFC); убрать всё, кроме букв/цифр/пробела/знаков препинания
  (неизвестный символ даёт пустую последовательность и падение ONNX Runtime на Reshape).

## Замеры (облачный сервер, 2 потока; разборчивость — распознаванием GigaAM, 6 фраз × 3)

| Вариант | Скорость | Слов распознано | Последнее слово |
|---|---|---|---|
| родной xVibe (PyTorch fp32, 1 шаг) | — | 87% | 17/18 |
| sherpa int8, 5 шагов, без правки | x1.4 | 67% | 13/18 |
| sherpa int8, 1 шаг, с правкой | **x1.6** | **77%** | 14/18 |
| sherpa fp32, 1 шаг, с правкой | x1.1 | 77% | 16/18 |

Образцы голосов (`reference_audio`) — из Google FLEURS ru_ru (CC BY 4.0), автоматический отбор по чистоте и высоте голоса.
