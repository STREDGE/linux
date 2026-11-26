# Таблица пропуска миссий (Джарвис)

`src/jarvis/mission_skip.h` получен из сценария игры `mainV1.scm` (GTA SA Android 2.10). Сам файл игры в репозиторий
не кладём — попросить у игрока (`data/script` установленной игры), описание команд — Sanny Builder `sa_mobile.json`.

```sh
cd /tmp/scm   # mainV1.scm и sa_mobile.json здесь
python3 <repo>/sa-living/tools/missions/hdr.py mainV1.scm                 # → missions.pkl (смещения миссий)
python3 <repo>/sa-living/tools/missions/ana2.py sa_mobile.json mainV1.scm # → skip_table.pkl + список «можно/нельзя»
python3 <repo>/sa-living/tools/missions/gen.py sa_mobile.json mainV1.scm  # → пишет src/jarvis/mission_skip.h
```

Правило «можно пропустить»: корень миссии типовой (`GOSUB основная часть → IF HAS_DEATHARREST_BEEN_EXECUTED →
GOSUB провал → GOSUB уборка → TERMINATE`), а `REGISTER_MISSION_PASSED` лежит в отдельном блоке: вход только прыжком
(перед блоком RETURN/GOTO/TERMINATE), до отметки нет RETURN/WAIT/TERMINATE, прыжки внутри блока — только внутрь него.
Берётся самое раннее такое начало (иначе можно пропустить счётчик сюжета в начале блока). 91 из 151: весь сюжет;
гонки, подработки, тиры, спортзалы, подружки, финалы из нескольких частей — нельзя.
