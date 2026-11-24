# Задание для сессии на компьютере: сборка APK для SA Coop

Вставь текст под чертой первым сообщением в сессию Claude Code, запущенную **на компьютере**
в папке проекта, где лежат APK GTA San Andreas с AML и файлы OBB.

---

Ты собираешь APK GTA San Andreas (Android 2.10, 64-bit, уже пропатчен под AML) для мода
кооператива SA Coop. Код мода: github.com/STREDGE/linux, ветка `ccr-ffecc13b-oyswo0`
(готовая сборка: `release/libSACoop.so`). В этот репозиторий ничего не пиши — только читай.

Правила: перед установкой любых программ и перед каждым шагом, который трудно откатить, —
спроси меня. Исходные файлы (APK, OBB) не меняй: работай с копиями в отдельной папке.
Если что-то из пунктов ниже невозможно или рискованно — скажи прямо и не делай.

## 1. Разрешение на микрофон (обязательно)

1. Найди APK игры с AML (~300 МБ). Если их несколько — спроси, какой.
2. Проверь инструменты: `java`, `apktool`, `zipalign`, `apksigner` (Android build-tools).
3. Текущая подпись: `apksigner verify --print-certs <apk>` — запиши, чем подписан.
4. `apktool d -s <apk> -o work`, в `work/AndroidManifest.xml` рядом с другими разрешениями добавь
   `<uses-permission android:name="android.permission.RECORD_AUDIO"/>`.
   Проверь, что `MANAGE_EXTERNAL_STORAGE` там уже есть (нужен для пункта 3).

## 2. OBB внутрь APK (сначала проверить, можно ли)

Игра ищет свои данные в файлах `And1.obb`, `And1a.obb` … `And6.obb` (строки в `lib/arm64-v8a/libGTASA.so`).
1. Посмотри, какие OBB есть у меня, что внутри (обычный zip? в нём And*.obb?) и где игра их ищет:
   `Android/obb/com.rockstargames.gtasa/`, assets внутри APK или другое место.
2. Вшивать в APK — только если игра реально читает эти данные из assets APK (проверь по коду/строкам
   libGTASA и по тому, как устроены сборки «APK с кэшем»). Учти размер: APK > ~2 ГБ может не установиться
   на части телефонов.
3. Если игра из APK их читать не умеет — не вшивай; напиши, куда класть OBB руками.

## 3. Папка `SA/cleo` в корне памяти — должна создавать сама игра

CLEO у меня читает скрипты из `/storage/emulated/0/SA/cleo`. Сейчас эти две папки (`SA` и внутри `cleo`)
приходится создавать руками. Нужно, чтобы их создавала **сама игра** при каждом запуске, если их нет
(мод SA Coop тоже их создаёт, но это запасной вариант — игра должна делать это без модов).

1. Найди в smali точку старта: `onCreate` главной Activity (по `AndroidManifest.xml`, intent-filter MAIN/LAUNCHER)
   или Java-часть AML, если она есть и вызывается при старте.
2. Добавь в начало `onCreate` (после вызова `super.onCreate`) эквивалент:
   ```java
   java.io.File d = new java.io.File(android.os.Environment.getExternalStorageDirectory(), "SA/cleo");
   if (!d.exists()) d.mkdirs();
   ```
   Без исключений наружу (обернуть в try/catch), чтобы при отказе в доступе игра не падала.
3. Доступ: на Android 11+ без «Доступа ко всем файлам» папку в корне создать нельзя. Если доступа нет
   (`Environment.isExternalStorageManager()` == false, только на API 30+), один раз при запуске открой экран
   разрешения: `Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))`.
   На Android 10 и ниже — обычное разрешение на запись (`WRITE_EXTERNAL_STORAGE`, оно в манифесте есть).
   Проверь, что `MANAGE_EXTERNAL_STORAGE` в манифесте есть.
4. Не ломай текущую настройку CLEO (путь `SA/cleo`). Покажи мне изменённый smali-кусок до сборки.

## 4. Моды сразу в комплекте (AML + SA Coop + SAUtils)

AML (RusJJ/AndroidModLoader) грузит моды только из папок: `Android/data/com.rockstargames.gtasa/mods/`
и, если в его конфиге задан `InternalModsFolder`, из `/storage/emulated/0/<папка>/com.rockstargames.gtasa`.
Моды, просто положенные внутрь APK, он сам не подхватит.
- Посмотри, как AML встроен в мой APK (есть ли его Java/smali-часть). Если можно аккуратно добавить
  на первом запуске копирование `libSACoop.so` и `libSAUtils64.so` из assets APK в папку `mods`
  (если их там ещё нет) — предложи план и спроси меня, прежде чем делать.
- Если нельзя или рискованно — сделай вместо этого zip «положить в Android/data/com.rockstargames.gtasa»
  со всеми нужными модами.
- `libSACoop.so` бери из `release/` репозитория, `libSAUtils64.so` — из моей папки модов.

## 5. Сборка и подпись

1. `apktool b work -o unsigned.apk`, `zipalign -p -f 4`, `apksigner sign`.
   - Если исходная подпись — известный публичный тестовый ключ (MT Manager / AOSP testkey и т.п.),
     подпиши **тем же**: APK встанет поверх, без удаления игры.
   - Иначе — новый ключ и **предупреди**: перед установкой игру удалить, а значит заранее скопировать
     `Android/data/com.rockstargames.gtasa`, `Android/obb/com.rockstargames.gtasa` и `Android_unprotected`.
2. Проверь: `apksigner verify`; в манифесте есть RECORD_AUDIO.
3. На телефон ничего не ставь сам. В конце — отчёт: путь к APK, встанет ли поверх, что сделано по пунктам
   2–4 и что нет (и почему), путь к папке скриптов CLEO.

После установки: Настройки Android → Приложения → GTA SA → Разрешения → **Микрофон** и
**Доступ ко всем файлам** → Разрешить.
