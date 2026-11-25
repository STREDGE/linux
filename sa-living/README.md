# SA Living

Мод для GTA SA Android 2.10 arm64 (AML): живые педы с ИИ (Claude, ключ игрока) и реализм — полиция,
банды, прохожие, свидетели, слухи, телефон, суд и тюрьма. План — [`DESIGN.md`](DESIGN.md) и [`docs/`](docs/).

Статус: **0.1.0 — первая сборка, в игре не проверялась.** Готовый файл — `release/libSALiving.so`,
инструкция для игрока — `release/README.txt`.

## Устройство кода

| Папка | Что |
|---|---|
| `src/main.cpp` | вход AML, игровой кадр (перехват `CGame::Process`), автосохранение мира, сообщения коопа |
| `src/platform/bridge.*` + `java/` | мост к Android: вшитый `classes.dex` (InMemoryDexClassLoader), HTTPS с потоковым ответом, SpeechRecognizer, TextToSpeech |
| `src/ai/claude.*` | Messages API напрямую (SSE), инструменты, кэш подсказки, effort, ответы по схеме, учёт расхода; у друга в коопе — через хоста |
| `src/talk/` | разговор: выбор собеседника, обстановка, история, инструменты-«кирпичики» (`prompt.cpp`) и их исполнение |
| `src/world/` | мир: персонажи (характер, отношения к CJ и к другу, память), события и свидетели, слухи, банды, дело в полиции; JSON-файл |
| `src/sim/police.*` | розыск по свидетелям (перехват `CWanted::RegisterCrime*`), задержание по шагам (перехват `CPlayerInfo::ArrestPlayer`), участок, суд, тюрьма |
| `src/sim/life.*` | персонажи по распорядку, месть и засады, инициатива педов, подмога банд, режиссёр города, профили персонажей |
| `src/net/link.*` | надёжные сообщения поверх канала SA Coop (`SACoopNet`, канал `L`), определение роли автоматически |
| `src/game/game.*` | libGTASA: педы, задачи (конструкторы задач + `CTaskManager::SetTask`), оружие, появление, деньги (`CPlayerInfo+0xF0`), розыск |
| `src/ui/hud.*` | ImGui поверх игры: «Говорить», «Телефон», кнопки по ситуации, субтитры, сцены суда; плитка «AI» в настройках (SAUtils `STB_Settings`) |

## Сборка

```sh
# из папки sa-living
$NDK/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk NDK_APPLICATION_MK=jni/Application.mk -j8
# Java-мост (только если менялся Bridge.java):
ANDROID_JAR=.../android.jar D8=.../build-tools/d8 bash tools/build_dex.sh
```

## В работе (0.2.0) — на паузе

**Где остановились и что делать дальше — [`STATUS.md`](STATUS.md).** Коротко: своя речь без Google —
распознавание GigaAM v3 (sherpa-onnx) и голоса xVibePocketTTS на своём движке `src/speech/xvibe.*` (готов,
проверен на компьютере), Java-мост готов; `speech.cpp`, меню пакета, групповые разговоры — не сделаны.
Подробности по модели голоса — `tools/xvibe/README.md`, образцы — `docs/samples/`.
