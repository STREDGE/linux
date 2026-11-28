// Снос участка порта под базу (ТЗ-BASE §2, этап А).
// Что снести — заранее посчитано по карте игры (demolish_list.h: модель + координаты каждого объекта).
// Как: в пулах игры (здания, «пустышки» динамических объектов, объекты) находим ровно эти сущности и делаем их
// невидимыми и без столкновений (флаги CEntity: bIsVisible, bUsesCollision). Сущности остаются в пулах игры —
// стриминг их не теряет и не падает. Игра снова подгружает порт, когда подъезжаешь (stream IPL), — поэтому
// проверка повторяется каждую секунду, пока игрок рядом. Дороги и пешие пути внутри перекрыты, случайные люди убираются.
#include "demolish.h"

#include <mod/amlmod.h>
#include <math.h>
#include <string.h>

#include "demolish_list.h"
#include "layout.h"
#include "roads_off.h"
#include "log.h"
#include "script.h"

namespace demolish
{
namespace
{
    template<typename T> T& F(uintptr_t p, uintptr_t off) { return *(T*)(p + off); }
    constexpr uintptr_t kFlags = 0x28, kModel = 0x32, kMatrix = 0x18, kPlace = 0x08;
    constexpr uint32_t kUsesCollision = 1u << 0, kIsVisible = 1u << 7;
    uintptr_t g_bld = 0, g_dummy = 0, g_obj = 0;
    uint32_t g_next = 0, g_nextClear = 0;
    bool g_loggedList = false, g_roadsOff = false;
    int g_hiddenTotal = 0, g_lastHidden = -1;
    bool g_found[sizeof(kDemolish) / sizeof(kDemolish[0])] = {};
    constexpr int kCount = (int)(sizeof(kDemolish) / sizeof(kDemolish[0]));

    game::Vec EntPos(uintptr_t e)
    {
        uintptr_t m = F<uintptr_t>(e, kMatrix);
        if(m && (m < 0x10000 || (m & 7))) m = 0;                 // мусор вместо указателя — берём позицию из самой сущности
        const float* f = m ? (const float*)(m + 0x30) : (const float*)(e + kPlace);
        game::Vec v; v.x = f[0]; v.y = f[1]; v.z = f[2]; return v;
    }
    bool InRect(const game::Vec& p, float pad)
    {
        return p.x > layout::kRx0 - pad && p.x < layout::kRx1 + pad && p.y > layout::kRy0 - pad && p.y < layout::kRy1 + pad;
    }
    // Пройти пул: stride — размер записи
    int Sweep(uintptr_t sym, uintptr_t stride, float match, const char* what)
    {
        uintptr_t pp = sym ? *(uintptr_t*)sym : 0;
        if(!pp) return 0;
        uintptr_t objs = F<uintptr_t>(pp, 0), flags = F<uintptr_t>(pp, 8); int size = F<int>(pp, 0x10);
        if(!objs || !flags || size <= 0 || size > 60000) return 0;
        int hidden = 0;
        for(int i = 0; i < size; ++i)
        {
            if(F<uint8_t>(flags, i) & 0x80) continue;
            uintptr_t e = objs + (uintptr_t)i * stride;
            game::Vec p = EntPos(e);
            if(!InRect(p, 40)) continue;
            int model = F<int16_t>(e, kModel);
            for(int k = 0; k < kCount; ++k)
            {
                const DemoItem& d = kDemolish[k];
                if(d.model != model) continue;
                float dx = p.x - d.x, dy = p.y - d.y;
                if(dx * dx + dy * dy > match * match) continue;
                uint32_t& fl = F<uint32_t>(e, kFlags);
                if(fl & (kUsesCollision | kIsVisible))
                {
                    fl &= ~(kUsesCollision | kIsVisible);
                    hidden++;
                    if(!g_found[k]) { g_found[k] = true; LOG("СНОС", "убран %s (модель %d) %s в (%.1f, %.1f, %.1f)", d.name, d.model, what, p.x, p.y, p.z); }
                }
                break;
            }
        }
        return hidden;
    }
}

bool Init(void* h)
{
    g_bld   = aml->GetSym(h, "_ZN6CPools16ms_pBuildingPoolE");
    g_dummy = aml->GetSym(h, "_ZN6CPools13ms_pDummyPoolE");
    g_obj   = aml->GetSym(h, "_ZN6CPools14ms_pObjectPoolE");
    if(!g_bld || !g_dummy || !g_obj) LOG("ОШИБКА", "Снос: нет пулов игры (здания %p, пустышки %p, объекты %p)", (void*)g_bld, (void*)g_dummy, (void*)g_obj);
    return g_bld && g_dummy;
}

void Reset() { g_roadsOff = false; g_lastHidden = -1; memset(g_found, 0, sizeof(g_found)); g_hiddenTotal = 0; }

void Frame(bool swat)
{
    if(!swat || !game::Player()) return;
    game::Vec pl = game::PlayerPos();
    game::Vec c = layout::W(0, 0);
    float d = game::Dist2D(pl, c);
    uint32_t now = game::TimeMs();
    if(!g_loggedList)
    {   // этап А: список объектов участка — в лог (один раз за запуск)
        g_loggedList = true;
        LOG("СНОС", "участок базы: Ocean Docks, x %.0f…%.0f, y %.0f…%.0f, пол на высоте %.2f; объектов к сносу: %d",
            layout::kRx0, layout::kRx1, layout::kRy0, layout::kRy1, layout::kZ, kCount);
        for(int k = 0; k < kCount; ++k) LOG("СНОС", "  %2d. %s (модель %d) (%.1f, %.1f, %.1f)", k + 1, kDemolish[k].name, kDemolish[k].model, kDemolish[k].x, kDemolish[k].y, kDemolish[k].z);
    }
    if(!g_roadsOff)
    {   // дороги для машин и пешеходов внутри базы — выключить (как INTRO)
        g_roadsOff = true;
        script::Do(0x01E8, { script::Arg::F(layout::kRx0), script::Arg::F(layout::kRy0), script::Arg::F(0), script::Arg::F(layout::kRx1), script::Arg::F(layout::kRy1), script::Arg::F(60) });   // SWITCH_ROADS_OFF
        // Дорога через базу — сквозная (мост → база → юг порта): выключаем её всю, от развилки до развилки (roads_off.h),
        // иначе у машин с моста нет другого пути и они едут через базу. Так же делает игра в миссиях.
        for(int i = 0; i < kRoadsOffCount; ++i)
        {
            const RoadNode& n = kRoadsOff[i];
            std::vector<script::Arg> a = { script::Arg::F(n.x - 3), script::Arg::F(n.y - 3), script::Arg::F(n.z - 20), script::Arg::F(n.x + 3), script::Arg::F(n.y + 3), script::Arg::F(n.z + 20) };
            script::Run(0x01E8, a, nullptr, true);                                              // SWITCH_ROADS_OFF (без записи каждой в лог)
        }
        LOG("СНОС", "сквозная дорога через базу выключена целиком: %d узлов (мост, южная дорога порта, петля)", kRoadsOffCount);
        script::Do(0x022B, { script::Arg::F(layout::kRx0), script::Arg::F(layout::kRy0), script::Arg::F(0), script::Arg::F(layout::kRx1), script::Arg::F(layout::kRy1), script::Arg::F(60) });   // SWITCH_PED_ROADS_OFF
        LOG("СНОС", "дороги и пешие пути внутри базы выключены");
    }
    // рядом — каждую секунду (порт подгружается), издалека — раз в 5 с (дальние копии зданий видно за километр)
    if(now >= g_next || now + 10000 < g_next)
    {
        g_next = now + (d > 450 ? 5000 : 1000);
        int h = Sweep(g_bld, 0x60, 1.5f, "здание") + Sweep(g_dummy, 0x60, 2.0f, "пустышка") + Sweep(g_obj, 0x220, 4.0f, "объект");   // размеры записей — из IsXxxPointerValid в libGTASA: 0x60 / 0x60 / 0x220
        g_hiddenTotal += h;
        if(h > 0)
        {
            int found = 0; for(int k = 0; k < kCount; ++k) if(g_found[k]) found++;
            LOG("СНОС", "убрано сейчас %d (всего найдено %d из %d по списку)%s", h, found, kCount, g_lastHidden < 0 ? "" : " — игра подгрузила порт заново");
            g_lastHidden = h;
        }
    }
    if(d < layout::kLiveRadius && (now >= g_nextClear || now + 10000 < g_nextClear))
    {   // случайные люди внутри базы не появляются: убрать «прохожих» (свои — созданы скриптом, их не трогает)
        g_nextClear = now + 4000;
        std::vector<script::Arg> a = { script::Arg::F(layout::kRx0 + 4), script::Arg::F(layout::kRy0 + 4), script::Arg::F(layout::kZ - 3),
                                       script::Arg::F(layout::kRx1 - 4), script::Arg::F(layout::kRy1 - 4), script::Arg::F(layout::kZ + 30) };
        script::Run(0x042B, a, nullptr, true);                                                // CLEAR_AREA_OF_CHARS
    }
}

int Hidden() { return g_hiddenTotal; }
}
