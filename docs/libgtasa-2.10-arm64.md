# libGTASA.so 2.10 arm64 — справка

SHA-256: `92b3714534742cf8e31eba35dbfd5e83871ecb5b936f5519d9b6ef3562234f9e` (размер 54731968 байт)

Пакет: `com.rockstargames.gtasa`, разрешение INTERNET есть. Экспортировано символов: 21652.

Сам файл игры в репозиторий не кладём.

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
