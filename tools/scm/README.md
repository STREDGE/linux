# Разбор сценария игры (mainV1.scm)

Файлы игры `mainV1.scm` и `scriptv1.img` в репозиторий НЕ кладём (репозиторий публичный, файлы — Rockstar).
Попроси у игрока: он достаёт их из папки `data/script` установленной игры и присылает в чат.
Выводы прошлого разбора — `docs/main-scm-notes.md`.

```sh
mkdir -p /tmp/scm && cd /tmp/scm
# описание команд (открытая библиотека Sanny Builder)
curl -sO https://raw.githubusercontent.com/sannybuilder/library/master/sa_mobile/sa_mobile.json
# mainV1.scm положить сюда же
python3 <repo>/tools/scm/dis.py sa_mobile.json mainV1.scm main.pkl
```

`main.pkl` — список `(смещение, имя команды, опкод, аргументы)`; аргументы: `('i', число)`, `('f', число)`,
`('gv', смещение глобальной переменной)`, `('lv', номер локальной)`, `('s', строка)`, `('arr', …)`.
Пример поиска (все запуски миссий и что перед ними):

```python
import pickle
ins, _ = pickle.load(open('main.pkl', 'rb'))
for i, (p, name, op, args) in enumerate(ins):
    if op == 0x417:
        for j in range(i - 15, i + 1): print(hex(ins[j][0]), ins[j][1], ins[j][3])
```

Переходы (`GOTO`, `GOSUB`) в main — абсолютные смещения файла; в миссиях — отрицательные (от начала миссии).
