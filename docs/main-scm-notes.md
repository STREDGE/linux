# Что известно из сценария игры (mainV1.scm, GTA SA Android 2.10)

Файлы `mainV1.scm` / `scriptv1.img` прислал игрок (в репозиторий НЕ кладём — это файлы игры).
Разборщик — `tools/scm/dis.py` (как запустить — `tools/scm/README.md`), описание команд — Sanny Builder `sa_mobile.json`.

* `$ONMISSION` — глобальная переменная по смещению **1632** (`DECLARE_MISSION_FLAG` 0180 в начале main).
* Запуск миссии — опкод **0x0417** `LOAD_AND_LAUNCH_MISSION_INTERNAL` (176 мест). Перехват в `mission.cpp` верный.
* Маркер миссии (типовой): `IS_INT_VAR_EQUAL_TO_NUMBER $1632 0` → `LOCATE_CHAR_ON_FOOT_3D` (радиус 1,2) →
  `CAN_PLAYER_START_MISSION` → `$1632 = 1` → `PRINT_BIG <название> 1000 2` → GOSUB (управление выкл.,
  широкий экран, своя камера, `CLEAR_AREA`, затемнение 500 мс) → `0417 <номер миссии>`.
  Поэтому у друга: рядом со спрятанным маркером держим `$1632 = 1` (маркер не срабатывает), а если всё-таки
  сработал — возвращаем камеру, широкий экран, затемнение и управление.
* Метки контактов — `ADD_SPRITE_BLIP_FOR_CONTACT_POINT` 02A7 и `ADD_SHORT_RANGE_…` 0570 (тип метки 5).
  Значки: через переменные 5, 8, 12, 13, 15, 16, 23, 24, 25, 28, 33, 34, 36, 38, 40, 42, 43, 44, 46, 47, 52;
  напрямую 11, 13, 15, 31, 32, 33, 35, 36, 37, 47, 51, 53, 54. 31/32 — недвижимость, 35 — дома-сохранения,
  54 — спортзал (их у друга не прячем).
* Катсцена в миссии: `LOAD_MISSION_TEXT` → `LOAD_CUTSCENE` → ждать `HAS_CUTSCENE_LOADED` → `START_CUTSCENE` →
  `DO_FADE 1000 1` → ждать `HAS_CUTSCENE_FINISHED` → `SET_PLAYER_CONTROL 0` → `DO_FADE 0 0` → `CLEAR_CUTSCENE`.
  136 вызовов `LOAD_CUTSCENE` (имена вида `SWEET1A`).
* «Миссия пройдена»: `PRINT_WITH_NUMBER_BIG M_PASS / M_PASSR / M_PASSS / M_PASSD <деньги> 5000 1`;
  провал: `PRINT_BIG M_FAIL … 1` — стиль 1 → строка 0 `AddBigMessage`. Название миссии — стиль 2 → строка 1.
