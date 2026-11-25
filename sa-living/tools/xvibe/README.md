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

## Свой движок (src/speech/xvibe.*) — вместо Pocket из sherpa-onnx

sherpa-onnx режет текст токенизатором unigram, а у xVibe токенизатор **BPE** — модель видит непривычные
куски («от|сю|да» вместо «отсю|да») и говорит хуже. Поэтому синтез — свой код на ONNX Runtime C API
(1:1 с эталоном KevinAHM/pocket-tts-onnx), с точным BPE (токены совпадают с SentencePiece).
Распознавание (GigaAM) остаётся на sherpa-onnx.

Голоса: встроенные голоса xVibe → `voice_state.py` (формат XVST); образцы — чистые студийные (Vosk TTS, Apache-2.0)
и их вариации (пересэмплирование ×0.88 / ×1.1 — ниже/медленнее и выше/быстрее). Образцы FLEURS (чтение вслух)
дают заметно худшую разборчивость — не используем.

| Голос (свой движок, int8, 1 шаг) | Слов распознано | Последнее слово |
|---|---|---|
| xVibe male_deep (встроенный) | **94%** | 18/18 |
| xVibe male_authoritative / female_silky | 86% / 86% | 17/18, 15/18 |
| Vosk M01 / M02 / F01 / F02 / F03 | 91 / 94 / 91 / 89 / 86% | 18,18,16,16,18 из 18 |
| вариации M01_low / M02_low / F01_low / M01_high / F02_high | 92 / 94 / 89 / 92 / 87% | 18,18,18,18,15 из 18 |
| для сравнения: образец FLEURS m05 | 80% | 15/18 |

Скорость (2 потока сервера): ×1.6–1.8 реального времени, первый звук ~0,5 с.
