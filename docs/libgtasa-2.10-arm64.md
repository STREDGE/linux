# libGTASA.so 2.10 arm64 — справка

SHA-256: `92b3714534742cf8e31eba35dbfd5e83871ecb5b936f5519d9b6ef3562234f9e` (размер 54731968 байт)

Пакет: `com.rockstargames.gtasa`, разрешение INTERNET есть. Экспортировано символов: 21652.

Сам файл игры в репозиторий не кладём.

## Справочник по всей игре

В `libGTASA.so` 2.10 есть отладочная информация (DWARF): классы с полями и смещениями, функции с параметрами.
Разобранная база лежит в репозитории игрока `STREDGE/helicover-brain` (только читать!):
`game_knowledge/ref/classes/*.md` (563 класса), `ref/functions_all.tsv`, `ref/tasks.md` (номера задач),
`raw/dwarf_types.json`. Битовые поля там записаны как `bits N@K` — K считается **от старшего бита** поля
(для `uint8`: бит = 7 − K; для `uint32`: бит = 31 − K).

Что использует 0.8.0 (все смещения — в `src/game/gta.h`, раздел «из отладочной информации»):
`CVehicle` (водитель 0x5A0, пассажиры 0x5A8, здоровье 0x634, флаги 0x568, тип 0x734), `CAutomobile`
(повреждения 0x758), `CPed` (оружие 0x730 по 0x20, текущее 0x8DC, IK 0x678, флаги 0x584),
`CTaskComplexEnterCar` (машина 0x18, место 0x2C), `CPedDamageResponseCalculator` (кто 0x0, урон 0x8,
часть тела 0xC, оружие 0x10), `CWanted::m_WantedLevel` 0x2C, `CStreamingInfo` (0x14 байт, статус 0x10).
vtable: `GetTaskType` +0x28, удалить объект +0x08, `CVehicle::BlowUpCar` +0x150. Пул задач — записи 0xA8.
`CWorld::Process` вызывает `CPlayerInfo::Process` для обоих игроков (второго — если слот занят).

## Ключевые символы (смещение от начала библиотеки)

| Символ | Смещение |
|---|---|
| `CWorld::Players` | `0000000000bdc738` |
| `CWorld::PlayerInFocus` | `0000000000bdcae8` |
| `FindPlayerPed(int)` | `00000000004efae0` |
| `CPlayerPed::SetupPlayerPed(int)` | `00000000005c0fd4` |
| `CPlayerPed::RemovePlayerPed(int)` | `00000000005c10b4` |
| `CPad::GetPad(int)` | `00000000004db628` |
| `CGameLogic::IsCoopGameGoingOn()` | `00000000003ce920` |
| `CGameLogic::n2PlayerPedInFocus` | `000000000085f834` |
| `CWanted::SetWantedLevel(int)` | `0000000000505d2c` |
| `CPools::ms_pPedPool` | `0000000000bc3bb8` |
| `CPools::ms_pVehiclePool` | `0000000000bc3bc0` |
| `CTheScripts::Process()` | `00000000003f3ad8` |
| `CRunningScript::ProcessOneCommand()` | `00000000003f445c` |
| `CGame::Process()` | `00000000004d62d4` |

## Смещения полей (arm64)

| Поле | Смещение | Как найдено |
|---|---|---|
| `sizeof(CPlayerInfo)` | `0x1D8` | `FindPlayerPed`, `SetupPlayerPed` |
| `CPlaceable::m_placement.pos` / `.heading` | `0x08` / `0x14` | `FindPlayerCoors`, `CalculateNewOrientation` |
| `CPlaceable::m_matrix` | `0x18` | `FindPlayerCoors` |
| `CMatrix::forward` / `pos` | `0x10` / `0x30` | `CMatrix::SetRotateZOnly`, `FindPlayerCoors` |
| `CPhysical::m_vecMoveSpeed` / `m_vecTurnSpeed` | `0x68` / `0x74` | `CPed::Teleport` |
| `CPed` флаг «в машине» | `0x585` бит 0 | `FindPlayerCoors` |
| `CPed::m_nPedState` | `0x54C` | `CPed::IsAlive` |
| `CPed::m_fHealth` | `0x6AC` | `CPlayerInfo::AddHealth` |
| `CPed::m_fCurrentRotation` / `m_fAimingRotation` | `0x6C4` / `0x6C8` | `CalculateNewOrientation` (+0x16C к ПК) |
| `CPed::m_pVehicle` | `0x708` | `FindPlayerCoors` |
| `CPed::m_nPedType` | `0x720` | `SetupPlayerPed`, `GetPadFromPlayer` |
| `sizeof(CPad)` / `DisablePlayerControls` | `0x158` / `0x110` | `GetPad`, `GetPedWalkUpDown` |
| `CEntity::m_pRwObject` | `0x20` | есть модель — можно создавать второго CJ |
| `CPed::m_nCreatedBy` | `0x548` | `SetCharCreatedBy` (1 игра, 2 миссия) |
| `CPed::m_pPlayerData` / `+0x20 m_fMoveBlendRatio` | `0x540` / `0x20` | `PlayerControlZelda` |
| `CPed::m_nMoveState` | `0x550` | `SetMoveState` |
| `CPed::m_fArmour` | `0x6B4` | `CCheat::MoneyArmourHealthCheat` |
| `TheCamera.Orientation` | `TheCamera + 0x154` | `PlayerControlZelda` |

## Режим на двоих (PS2) — где игра смотрит на `Players[1]`

`CGameLogic::Update` (поводок, общая смерть), `CCamera::CamControl` (общая камера),
`CPickups::Update`, `CDarkel::Update`, `IsCoopGameGoingOn`, `IsPlayerUse2PlayerControls`.
Мод на время этих функций убирает второго CJ из слота, а две последние возвращают false.

Джойстик на экране (`CTouchInterface`) читается для **любого** `CPad`, поэтому у второго CJ
выставляется `DisablePlayerControls`, иначе он повторял бы касания.

## Найденные ловушки

- **Выход из интерьера удаляет второго CJ.** `CEntryExit::TransitionFinished` вызывает
  `CPopulation::RemoveAllRandomPeds`, а та удаляет любого персонажа, для которого
  `CPed::CanBeDeleted()` == true. Эта проверка не смотрит, игрок ли это, — только
  «в машине», «в банде игрока» и `m_nCreatedBy` (`0x548`). Поэтому после создания
  второго CJ мод вызывает `CPed::SetCharCreatedBy(2)` (как скриптовая `CREATE_PLAYER`).

## Как игра двигает игрока пешком (`CTaskSimplePlayerOnFoot::PlayerControlZelda`)

- `x = pad->GetPedWalkLeftRight()`, `y = pad->GetPedWalkUpDown()` (short, ±128; вперёд — минус).
- Сила движения = `sqrt(x² + y²) / 60` → `CPlayerPedData::m_fMoveBlendRatio` (`+0x20`, плавно).
- Курс = `atan2(-x, -y) - TheCamera.Orientation` (`TheCamera + 0x154`) → `m_fAimingRotation`.
- `pad->GetSprint(0)` → `SetMoveState(6)`. `m_nMoveState` — `CPed + 0x550`.
- `CTaskSimplePlayerOnFoot::ProcessPed` флаг `DisablePlayerControls` не проверяет — он
  работает только внутри геттеров `CPad`. Поэтому мод отключает пульт 2, но три геттера
  (ходьба X/Y и спринт) для пульта 2 перехватывает и отдаёт «виртуальный джойстик» друга.

## Совместимость с другими модами игрока

- **Touch x4** (`libMultiTouch64.so`, net.helicover.touch4) подменяет GOT-ячейку `Points`
  (`0x851D38`) на свой массив на 4 пальца. Поэтому меню читает касания через эту ячейку,
  а не по символу `Points`.
- **Helicover Brain** проверяет начало `CTouchInterface::IsTouchDown` (`0x36F9A4`) и
  `LIB_PointerGetCoordinates` (`0x326FEC`) и прячет свои кнопки, если код изменён, а также
  во время паузы и катсцен. Кнопки рисует через GOT-ячейку `CHud::Draw` (`0x845EC8`).
  SA Coop эти функции не трогает.

## Прочее, на что опирается интерфейс

| Что | Где |
|---|---|
| `CTimer::m_UserPause` | символ `_ZN6CTimer11m_UserPauseE` — снимаем в коопе, чтобы меню не ставило паузу |
| `CCutsceneMgr::ms_running` | символ `_ZN12CCutsceneMgr10ms_runningE` |
| массив касаний `Points` | GOT-ячейка `0x851D38`: 2 пальца × 0x1C байт (x, y, …, state; 2 = нажат). Touch x4 подменяет на 4 пальца |
| `CAdjustableHUD` («Настройка сенсоров») | `self+0x64` — нажатая кнопка экрана (2 сохранить, 3 отмена, 4 — НЕ надёжно), после обработки −1; `self+0x52E == 1` — экран закрывается |
| `MobileMenu::Render/Update`, `Menu_SwitchOffToGame` | меню игры; Update блокируем, пока открыто наше меню |
| `Render2dStuff` | интерфейс игры; после него рисуем свой слой |
