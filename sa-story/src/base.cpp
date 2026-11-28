// База S.W.A.T. (ТЗ-BASE §3–§6): всё снаружи, из деталей игры. Объекты создаются, когда игрок ближе ~450 м
// (видно издалека), люди — ближе 150 м (как в ТЗ: экономия памяти телефона). Перед сохранением всё созданное
// убирается (в файл игры не попадает), после — создаётся снова.
#include "base.h"

#include <mod/amlmod.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>

#include <swat/sgame.h>
#include <swat/tactics.h>

#include "layout.h"
#include "log.h"
#include "story.h"
#include "ui.h"
#include "wanted.h"

using namespace swat;
using layout::W;

namespace base
{
namespace
{
    // План построек — layout::kPieces (base_plan.h, сгенерирован tools/base_plan.py из файлов игры).

    // Чужие машины внутри забора: мод их НЕ удаляет (решение игрока — без костылей), только пишет в лог,
    // чтобы найти и закрыть дыру, через которую они попали.
    void IntruderLog()
    {
        static std::vector<int> seen; static uint32_t next = 0, lastSum = 0; static int total = 0;
        uint32_t now = NowMs();
        if(now < next && next - now < 10000) return;
        next = now + 2000;
        uintptr_t mine = PedVehicle(Player());
        ForEachVehicle([&](uintptr_t v) {
            if(v == mine) return;
            game::Vec p = layout::G(Pos(v));
            if(!Inside(p, -4.0f) || !VehDriver(v)) return;
            int m = Model(v);
            bool service = m == 427 || m == 601 || m == 596 || m == 490 || m == 599 || m == 523 || m == 416 || m == 497 || m == 487 || m == 597 || m == 598 || m == 528 || m == 433 || m == 470;
            if(service) return;
            int ref = VehRef(v);
            if(std::find(seen.begin(), seen.end(), ref) != seen.end()) return;
            seen.push_back(ref); if(seen.size() > 64) seen.erase(seen.begin());
            total++;
            V3 vel = Velocity(v);
            LOG("БАЗА", "ЧУЖАЯ МАШИНА внутри забора: модель %d в (%.0f, %.0f), едет (%.1f, %.1f) м/с — всего за игру %d", m, p.x, p.y, vel.x, vel.y, total);
        });
        if(now - lastSum > 120000) { lastSum = now; LOG("БАЗА", "чужих машин внутри забора за игру: %d", total); }
    }

    // ---------------------------------------------------------------- люди
    struct Staff
    {
        const char* name; int kind; int model; float u, v, planH; int weapon;
        int ref = 0; bool released = false; uint32_t lineAt = 0, taskAt = 0; int route = 0; int animStep = 0;
        bool alarmHurt = false; uint32_t hurtAt = 0;
    };
#define SPOT(i) layout::kStaffSpots[i].u, layout::kStaffSpots[i].v, layout::kStaffSpots[i].planH
    std::vector<Staff> g_staff = {
        { "Охранник Бойко", 1, 280, SPOT(0), 22 },
        { "Охранник Мартинес", 1, 282, SPOT(1), 22 },
        { "Фрэнк", 2, 286, SPOT(2), 24 },
        { "Томми", 3, 281, SPOT(3), 22 },
        { "Сержант Ковальски", 0, 280, SPOT(4), 0 },
        { "Офицер Рейес", 0, 282, SPOT(5), 0 },
        { "Офицер Ли", 0, 281, SPOT(6), 0 },
        { "Офицер Карлос", 0, 288, SPOT(7), 0 },
        { "Офицер Ньюман", 0, 280, SPOT(8), 0 },
    };
#undef SPOT
    // маршрут сотрудников по двору — layout::kRoute (вдоль проездов базы)
    constexpr int kRouteN = (int)(sizeof(layout::kRoute) / sizeof(layout::kRoute[0]));

    const char* kGuardLines[2][4] = {
        { "Проезжай.", "С возвращением, Артём.", "На посту без происшествий.", "Документы? Шучу, проезжай." },
        { "Проезжай. Опять дождь…", "С возвращением. Тут тихо — пока.", "Ночь будет длинной.", "Смотри в оба, Артём." } };
    const char* kFrankLines[2][4] = {
        { "Опять опоздал?", "Отчёт где?", "Город сам себя не почистит.", "Работы много — бери вызов." },
        { "Опять опоздал? Город горит.", "Отчёт где? Мэрия опять на телефоне.", "В этом городе честных не осталось — кроме нас.", "Бери вызов. Ночь не ждёт." } };
    const char* kTommyLines[2][4] = {
        { "Артём! Шашлык будешь?", "Я машину «починил». Ну почти.", "Скукота. Поехали кого-нибудь ловить!", "Слышал анекдот про патрульного? Потом расскажу." },
        { "Артём, тут даже шашлык мрачный.", "Я машину «починил». Она сама не знает.", "Говорят, в доках опять стреляли.", "Не смотри на меня так, я в форме!" } };
    const char* kStaffLines[2][3] = {
        { "Здравия желаю!", "Всё по графику.", "Хорошая погода сегодня." },
        { "Опять льёт…", "Лишь бы ночь пережить.", "Слышал сирены? Опять Гроув." } };

    // ---------------------------------------------------------------- состояние
    struct Created { int handle; int model; const char* what; };
    std::vector<Created> g_objs;
    int g_gate[2] = { 0, 0 }; float g_gateOpen = 0; bool g_gateWant = false; uint32_t g_gateFreeAt = 0;   // створки ворот: 0 закрыты … 1 открыты
    layout::Piece g_gatePiece[2] = {};
    int g_builtLook = -1;
    bool g_built = false, g_peopleOn = false, g_live = false;
    uint32_t g_nextCars = 0, g_nextAmbient = 0;
    int g_blipBase = 0;
    struct Car { int model; float u, v, planH; int handle; const char* what; uint32_t goneAt; };
    std::vector<Car> g_cars = {
        { 427, layout::kFleetU, layout::kFleetV, 0, 0, "Enforcer", 0 }, { 601, layout::kFleetU - 4.0f, layout::kFleetV, 0, 0, "фургон S.W.A.T.", 0 },
        { 596, layout::kFleetU - 8.0f, layout::kFleetV, 0, 0, "полицейская машина", 0 }, { 490, layout::kFleetU - 12.0f, layout::kFleetV, 0, 0, "FBI Rancher", 0 },
        { 599, layout::kFleetU - 16.0f, layout::kFleetV, 0, 0, "полицейский джип", 0 }, { 523, layout::kFleetU - 20.0f, layout::kFleetV, 0, 0, "полицейский мотоцикл", 0 },
        { 416, layout::kMedU - 13.0f, layout::kMedV + 1.0f, 90, 0, "скорая у медпункта", 0 },
    };
    struct Heli { int model; float u, v; int handle; uint32_t goneAt; const char* what; };
    std::vector<Heli> g_helis = { { 497, layout::kPadU, layout::kPadV, 0, 0, "полицейский «Маврик»" }, { 487, layout::kHeli2U, layout::kHeli2V, 0, 0, "«Маврик»" } };
    int g_introHeli = 0;
    bool g_holdPad = false;                 // идёт вступление: площадка занята его вертолётом — свой «Маврик» не ставим
    bool g_gateBlocked = false;             // Т149: Enforcer стоит в воротах
    std::vector<int> g_blips;               // значки точек базы на карте

    // тревога и оборона
    bool g_alarm = false; uint32_t g_alarmAt = 0, g_quietSince = 0; std::string g_alarmWhy;
    Team* g_def = nullptr;
    std::vector<int> g_raiders;
    // маркеры
    int g_inMarker = -1;
    // тир
    struct Range { bool on = false; uint32_t until = 0, nextPop = 0; int score = 0, lane = -1; int target = 0; uint32_t popAt = 0; } g_range;
    // задержанные в камере
    struct Jailed { int ref; uint32_t at; };
    std::vector<Jailed> g_jailed;

    // ---------------------------------------------------------------- утилиты
    uint32_t Now() { return NowMs(); }
    bool Ago(uint32_t t, uint32_t ms) { uint32_t n = Now(); return n < t || n - t >= ms; }
    int PlayerChar() { return PlayerRef(); }
    float PlanToWorldH(float planH) { return layout::H(planH); }
    bool LoadModels(const std::vector<int>& models)
    {
        bool all = true;
        for(int m : models) if(!Is(0x0248, { Ai(m) })) { Do(0x0247, { Ai(m) }); all = false; }   // HAS_MODEL_LOADED / REQUEST_MODEL
        return all;
    }
    // Объект ставится командой «без смещения» (029B): начало модели — ровно в заданной точке.
    // Обычная команда создания (0107) сама поднимает объект на высоту его низа — из-за этого первая база «висела».
    int CreateAt(const layout::Piece& p, int* offCount)
    {
        int h = OutI(0x029B, { Ai(p.model), Af(p.x), Af(p.y), Af(p.z), Aoi() });               // CREATE_OBJECT_NO_OFFSET
        if(h <= 0) { LOG("ОШИБКА", "База: объект %s (модель %d) не создан", p.what, p.model); return 0; }
        Do(0x0177, { Ai(h), Af(p.h) });                                                      // SET_OBJECT_HEADING
        Do(0x0392, { Ai(h), Ai(0) });                                                        // SET_OBJECT_DYNAMIC 0 — не сдвинуть
        Do(0x09CA, { Ai(h), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });                            // SET_OBJECT_PROOFS
        V3 at;
        if(Out3(0x01BB, { Ai(h), Aof(), Aof(), Aof() }, at) && fabsf(at.z - p.z) > 0.05f)     // GET_OBJECT_COORDINATES — проверка высоты
        {
            if(offCount && (*offCount)++ < 6) LOG("БАЗА", "ВНИМАНИЕ: %s (модель %d) встал на высоту %.2f вместо %.2f", p.what, p.model, at.z, p.z);
        }
        g_objs.push_back({ h, p.model, p.what });
        return h;
    }
    void DeleteObjs()
    {
        for(auto& o : g_objs) if(Is(0x03CA, { Ai(o.handle) })) Do(0x0108, { Ai(o.handle) }); // DOES_OBJECT_EXIST / DELETE_OBJECT
        g_objs.clear(); g_gate[0] = g_gate[1] = 0; g_gateOpen = 0; g_builtLook = -1;
    }

    // ---------------------------------------------------------------- постройка
    // Сохранение игры записывает объекты и машины, созданные скриптом. Удалять их ПЕРЕД сохранением нельзя:
    // автосохранение телефона идёт посреди отрисовки кадра — игра обращается к удалённому и падает.
    // Поэтому после загрузки, перед постройкой, убираем старые копии: та же модель на том же месте по плану.
    template<typename T> T& Fld(uintptr_t p, uintptr_t off) { return *(T*)(p + off); }
    game::Vec EntPos(uintptr_t e)
    {
        uintptr_t m = Fld<uintptr_t>(e, 0x18);
        if(m && (m < 0x10000 || (m & 7))) m = 0;
        const float* f = m ? (const float*)(m + 0x30) : (const float*)(e + 0x08);
        game::Vec v; v.x = f[0]; v.y = f[1]; v.z = f[2]; return v;
    }
    uintptr_t g_objPoolSym = 0, g_vehPoolSym = 0;
    void CleanStale()
    {
        struct Spot { int model; float x, y, r; };
        std::vector<Spot> spots;
        for(auto& p : layout::kPieces) spots.push_back({ p.model, p.x, p.y, p.model == 2933 ? 10.0f : 0.8f });   // створки ворот могли быть сдвинуты
        for(auto& c : g_cars) { game::Vec w = W(c.u, c.v); spots.push_back({ c.model, w.x, w.y, 5.0f }); }
        for(auto& h : g_helis) { game::Vec w = W(h.u, h.v); spots.push_back({ h.model, w.x, w.y, 6.0f }); }
        auto mine = [](int h) {
            for(auto& o : g_objs) if(o.handle == h) return true;
            for(auto& c : g_cars) if(c.handle == h) return true;
            for(auto& x : g_helis) if(x.handle == h) return true;
            return h == g_introHeli;
        };
        int objs = 0, cars = 0;
        for(int pass = 0; pass < 2; ++pass)
        {
            uintptr_t sym = pass == 0 ? g_objPoolSym : g_vehPoolSym;
            uintptr_t pp = sym ? *(uintptr_t*)sym : 0;
            if(!pp) continue;
            uintptr_t arr = Fld<uintptr_t>(pp, 0), flags = Fld<uintptr_t>(pp, 8); int size = Fld<int>(pp, 0x10);
            if(!arr || !flags || size <= 0 || size > 60000) continue;
            uintptr_t stride = pass == 0 ? 0x220 : 0xC68;                                   // из IsObjectPointerValid / IsVehiclePointerValid
            for(int i = 0; i < size; ++i)
            {
                uint8_t fl = Fld<uint8_t>(flags, i);
                if(fl & 0x80) continue;
                uintptr_t e = arr + (uintptr_t)i * stride;
                int model = Fld<int16_t>(e, 0x32);
                game::Vec p = EntPos(e);
                for(auto& sp : spots)
                {
                    if(sp.model != model) continue;
                    float dx = p.x - sp.x, dy = p.y - sp.y;
                    if(dx * dx + dy * dy > sp.r * sp.r) continue;
                    int h = (i << 8) | fl;                                                  // как CPools::GetObjectRef / GetVehicleRef
                    if(mine(h)) break;
                    if(pass == 0) { Do(0x0108, { Ai(h) }); objs++; }                          // DELETE_OBJECT
                    else if(!VehDriver(e)) { Do(0x00A6, { Ai(h) }); cars++; }                 // DELETE_CAR (только пустую)
                    break;
                }
            }
        }
        if(objs || cars) LOG("БАЗА", "после загрузки убраны старые копии базы из сохранения: объектов %d, машин %d", objs, cars);
    }

    // Дальность прорисовки: у объектов базы нет «дальних копий», игра рисует их только вблизи (30–200 м).
    // На время режима поднимаем дальность моделей базы до 300 м — поле CBaseModelInfo::m_fDrawDistance.
    uintptr_t g_modelInfoPtrs = 0;
    constexpr uintptr_t kDrawDistOff = 0x38;                                                // CBaseModelInfo::Init пишет сюда 2000.0
    void RaiseDrawDistance()
    {
        if(!g_modelInfoPtrs) return;
        int done = 0, odd = 0;
        for(auto& p : layout::kPieces)
        {
            uintptr_t mi = ((uintptr_t*)g_modelInfoPtrs)[p.model];
            if(!mi) continue;
            float& dd = Fld<float>(mi, kDrawDistOff);
            if(dd >= 299.0f) continue;
            if(dd < 5.0f || dd > 1500.0f) { odd++; continue; }                                // не похоже на дальность — не трогаем
            dd = 300.0f; done++;
        }
        if(done || odd) LOG("БАЗА", "дальность прорисовки поднята до 300 м у %d моделей%s", done, odd ? " (у части поле не опознано — пропущены)" : "");
    }

    bool Build()
    {
        CleanStale();
        int look = story::GetInt("look", 1);
        std::vector<int> models;
        for(auto& p : layout::kPieces) if((p.look < 0 || p.look == look) && std::find(models.begin(), models.end(), p.model) == models.end()) models.push_back(p.model);
        if(!LoadModels(models)) return false;
        RaiseDrawDistance();
        int n = 0, off = 0, gates = 0;
        for(auto& p : layout::kPieces)
        {
            if(p.look >= 0 && p.look != look) continue;
            int h = CreateAt(p, &off);
            if(!h) continue;
            n++;
            if(p.model == 2933 && gates < 2) { g_gate[gates] = h; g_gatePiece[gates] = p; gates++; }   // створки ворот
        }
        g_gateOpen = 0; g_gateWant = false;
        for(int m : models) Do(0x0249, { Ai(m) });                                           // MARK_MODEL_AS_NO_LONGER_NEEDED
        g_builtLook = look;
        LOG("БАЗА", "построена: %d объектов (вид «%s»), створок ворот %d%s", n, look ? "Готэм" : "Обычная", gates, off ? " — у части объектов высота не та, см. выше" : "");
        return true;
    }

    // ---------------------------------------------------------------- машины и вертолёты
    void KeepCars()
    {
        bool far = Dist2(Pos(Player()), layout::V(W(0, 0))) > 60.0f;
        std::vector<int> need;
        for(auto& c : g_cars) need.push_back(c.model);
        for(auto& h : g_helis) need.push_back(h.model);
        for(auto& c : g_cars)
        {
            bool alive = c.handle > 0 && Is(0x056E, { Ai(c.handle) }) && !Is(0x0119, { Ai(c.handle) });
            if(alive)
            {
                // машину увели далеко — «вернётся сама», когда игрок отъедет (новая на месте, старую отпускаем)
                uintptr_t v = Veh(c.handle);
                bool away = v && Dist2(Pos(v), layout::V(W(c.u, c.v))) > 30 && !(g_gateBlocked && &c == &g_cars[0]);   // Enforcer в воротах — на службе
                bool used = v && VehDriver(v) != 0;
                if(away && !used && far) { Do(0x01C3, { Ai(c.handle) }); c.handle = 0; LOG("БАЗА", "%s оставлен в городе — на базе будет новый", c.what); }
                continue;
            }
            // разбита или угнана: новая на месте — когда игрока рядом нет (не из воздуха на глазах)
            if(c.handle > 0) { Do(0x01C3, { Ai(c.handle) }); c.handle = 0; c.goneAt = Now(); }
            if(c.goneAt && !far) continue;
            if(!LoadModels({ c.model })) continue;
            game::Vec p = W(c.u, c.v, 0.6f);
            int h = OutI(0x00A5, { Ai(c.model), Af(p.x), Af(p.y), Af(p.z), Aoi() });          // CREATE_CAR
            if(h <= 0) continue;
            Do(0x0175, { Ai(h), Af(PlanToWorldH(c.planH)) });                                 // SET_CAR_HEADING
            Do(0x020A, { Ai(h), Ai(1) });                                                     // LOCK_CAR_DOORS 1 — открыта
            Do(0x0249, { Ai(c.model) });
            c.handle = h; c.goneAt = 0;
            LOG("БАЗА", "служебный транспорт: %s", c.what);
        }
        for(auto& hh : g_helis)
        {
            if(hh.model == 497 && g_holdPad && g_introHeli <= 0) continue;                      // вертолёт вступления ещё летит сюда
            if(hh.model == 497 && g_introHeli > 0)
            {
                if(Is(0x056E, { Ai(g_introHeli) }) && !Is(0x0119, { Ai(g_introHeli) })) { hh.handle = g_introHeli; continue; }
                g_introHeli = 0;
            }
            bool alive = hh.handle > 0 && Is(0x056E, { Ai(hh.handle) }) && !Is(0x0119, { Ai(hh.handle) });
            if(alive)
            {
                uintptr_t v = Veh(hh.handle);
                bool away = v && Dist2(Pos(v), layout::V(W(hh.u, hh.v))) > 40 && !VehDriver(v);
                if(away && far) { Do(0x01C3, { Ai(hh.handle) }); hh.handle = 0; LOG("БАЗА", "%s брошен — на крыше будет новый", hh.what); }
                continue;
            }
            // разбит или угнан — новый, когда тебя нет рядом (ТЗ-BASE №3)
            if(hh.handle > 0) { Do(0x01C3, { Ai(hh.handle) }); hh.handle = 0; hh.goneAt = Now(); }
            if(!far && hh.goneAt) continue;
            if(!LoadModels({ hh.model })) continue;
            game::Vec p = W(hh.u, hh.v); p.z = layout::kRoofZ + 1.2f;
            int h = OutI(0x00A5, { Ai(hh.model), Af(p.x), Af(p.y), Af(p.z), Aoi() });
            if(h <= 0) continue;
            Do(0x0175, { Ai(h), Af(PlanToWorldH(90)) });
            Do(0x0249, { Ai(hh.model) });
            hh.handle = h; hh.goneAt = 0;
            LOG("БАЗА", "на крыше штаба: %s", hh.what);
        }
    }

    // ---------------------------------------------------------------- шлагбаум
    // Откатные ворота: две створки разъезжаются в стороны. Открываются только своим: Артёму, служебным машинам
    // и подмоге; закрываются через 3 с после того, как проезд свободен. Тревога (Т143) — закрыты.
    void Gate()
    {
        if(g_gate[0] <= 0 || g_gate[1] <= 0) return;
        V3 gate = layout::WV(layout::kGateU, layout::kGateV);
        bool want = false;
        uintptr_t pl = Player();
        if(pl && Dist(Pos(pl), gate) < 16.0f) want = true;                                     // герой
        if(!want && !g_alarm)
            ForEachVehicle([&](uintptr_t v) {                                                  // служебная машина / подмога
                if(want) return;
                int m = Model(v);
                bool service = m == 427 || m == 601 || m == 596 || m == 490 || m == 599 || m == 523 || m == 416 || m == 597 || m == 598 || m == 528 || m == 433 || m == 470;
                if(service && Dist(Pos(v), gate) < 20.0f && VehDriver(v)) want = true;
            });
        uint32_t now = Now();
        if(want) g_gateFreeAt = now;
        bool open = want || (g_gateFreeAt && !Ago(g_gateFreeAt, 3000));
        if(g_alarm) open = false;
        if(open != g_gateWant) { g_gateWant = open; LOG("БАЗА", "ворота %s", open ? "открываются" : "закрываются"); }
        for(int k = 0; k < 2; ++k)
        {
            const layout::Piece& gp = g_gatePiece[k];
            float dir = gp.x > gate.x ? 1.0f : -1.0f;                                          // каждая створка уезжает от середины проезда
            float tx = gp.x + (open ? dir * 8.9f : 0.0f);
            Is(0x034E, { Ai(g_gate[k]), Af(tx), Af(gp.y), Af(gp.z), Af(0.09f), Af(0.09f), Af(0.09f), Ai(0) });   // SLIDE_OBJECT
        }
    }

    // ---------------------------------------------------------------- люди
    void SayLine(Staff& s, const char* text)
    {
        unsigned rgb = s.kind == 2 ? 0xF2C14E : (s.kind == 3 ? 0x5AAAFF : 0xB8C0CC);
        std::string who = s.name;
        ui::Subtitle(who, text, 3.5f, rgb);
        s.lineAt = Now();
    }
    void SpawnStaff(Staff& s)
    {
        if(!LoadModels({ s.model })) return;
        game::Vec p = W(s.u, s.v, 0.5f);
        int h = OutI(0x009A, { Ai(6), Ai(s.model), Af(p.x), Af(p.y), Af(p.z), Aoi() });      // CREATE_CHAR (тип: полиция)
        if(h <= 0) return;
        Do(0x0173, { Ai(h), Af(PlanToWorldH(s.planH)) });
        Do(0x02AB, { Ai(h), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });                             // SET_CHAR_PROOFS — свои неуязвимы
        Do(0x0568, { Ai(h), Ai(1) });                                                         // SET_CHAR_NEVER_TARGETTED
        if(s.weapon) { swat::GiveWeapon(h, s.weapon, 200); }
        Do(0x0249, { Ai(s.model) });
        s.ref = h; s.taskAt = 0; s.alarmHurt = false;
        LOG("БАЗА", "на базе: %s", s.name);
    }
    void RemoveStaff(Staff& s, bool del)
    {
        if(s.ref > 0 && !s.released) { if(del) Do(0x009B, { Ai(s.ref) }); else Do(0x01C2, { Ai(s.ref) }); }   // DELETE_CHAR / MARK_CHAR_AS_NO_LONGER_NEEDED
        s.ref = 0;
    }
    void PeopleFrame()
    {
        int look = g_builtLook < 0 ? 1 : g_builtLook;
        uintptr_t pl = Player(); V3 pp = Pos(pl);
        for(auto& s : g_staff)
        {
            if(s.released) continue;
            if(s.ref > 0 && !RefAlive(s.ref)) { s.ref = 0; }
            if(s.ref <= 0) { SpawnStaff(s); continue; }
            if(g_alarm) continue;                                     // в бою людьми командует оборона
            uintptr_t ped = Ped(s.ref); if(!ped) continue;
            V3 sp = Pos(ped);
            float d = Dist(sp, pp);
            // реплики, когда подходишь
            static uint32_t lastLineAt = 0;
            if(d < 4.5f && Ago(s.lineAt, 40000) && Ago(lastLineAt, 20000))
            {
                lastLineAt = Now();
                int k = s.animStep++ % 4;                                                     // по кругу: фраза не повторяется, пока не сказаны остальные
                if(s.kind == 1) SayLine(s, kGuardLines[look][k]);
                else if(s.kind == 2) SayLine(s, kFrankLines[look][k]);
                else if(s.kind == 3) SayLine(s, kTommyLines[look][k]);
                else SayLine(s, kStaffLines[look][k % 3]);
                if(s.kind == 1) Do(0x0639, { Ai(s.ref), Ai(PlayerChar()) });                    // TASK_TURN_CHAR_TO_FACE_CHAR
            }
            if(!Ago(s.taskAt, s.kind == 0 ? 9000u : 14000u)) continue;
            s.taskAt = Now();
            switch(s.kind)
            {
                case 1:   // охрана: стоит у КПП, поворачивается к подходящему; иногда «проезжай» жестом
                {
                    game::Vec home = W(s.u, s.v);
                    if(Dist2(sp, Mk(home.x, home.y, home.z)) > 2) Do(0x05D3, { Ai(s.ref), Af(home.x), Af(home.y), Af(home.z), Ai(4), Ai(10000) });
                    else if(d < 20) Do(0x0605, { Ai(s.ref), As("CopTraf_Come"), As("POLICE"), Af(4), Ai(0), Ai(0), Ai(0), Ai(0), Ai(2500) });
                    else Do(0x05D4, { Ai(s.ref), Af(PlanToWorldH(s.planH)) });
                    break;
                }
                case 2:   // Фрэнк: ходит туда-сюда у кабинета
                {
                    s.route ^= 1;
                    game::Vec t = W(layout::kFrankU + (s.route ? 5.0f : -1.0f), layout::kFrankV - 2.5f);
                    Do(0x05D3, { Ai(s.ref), Af(t.x), Af(t.y), Af(t.z), Ai(4), Ai(12000) });
                    break;
                }
                case 3:   // Томми: сидит/жуёт у мангала, иногда «чинит» машину у гаража
                {
                    s.route = (s.route + 1) % 4;
                    if(s.route == 3)
                    {
                        game::Vec t = W(layout::kFleetU - 6.0f, layout::kFleetV + 4.0f);
                        Do(0x05D3, { Ai(s.ref), Af(t.x), Af(t.y), Af(t.z), Ai(4), Ai(20000) });
                    }
                    else
                    {
                        game::Vec t = W(layout::kRestU + 4.5f, layout::kRestV - 1.0f);
                        if(Dist2(sp, Mk(t.x, t.y, t.z)) > 2) Do(0x05D3, { Ai(s.ref), Af(t.x), Af(t.y), Af(t.z), Ai(4), Ai(20000) });
                        else Do(0x0605, { Ai(s.ref), As("M_smklean_loop"), As("SMOKING"), Af(4), Ai(1), Ai(0), Ai(0), Ai(0), Ai(12000) });
                    }
                    break;
                }
                default:  // сотрудники: ходят по маршруту, курят, болтают, звонят
                {
                    int r = s.route = (s.route + 1) % (kRouteN + 3);
                    if(r < kRouteN)
                    {
                        int k = (r + (int)(&s - &g_staff[0]) * 2) % kRouteN;
                        game::Vec t = W(layout::kRoute[k].u, layout::kRoute[k].v);
                        Do(0x05D3, { Ai(s.ref), Af(t.x), Af(t.y), Af(t.z), Ai(4), Ai(15000) });
                    }
                    else if(r == kRouteN) Do(0x0605, { Ai(s.ref), As("M_smk_in"), As("SMOKING"), Af(4), Ai(0), Ai(0), Ai(0), Ai(0), Ai(6000) });
                    else if(r == kRouteN + 1) Do(0x0605, { Ai(s.ref), As("phone_talk"), As("ped"), Af(4), Ai(1), Ai(0), Ai(0), Ai(0), Ai(7000) });
                    else Do(0x0605, { Ai(s.ref), As("IDLE_chat"), As("ped"), Af(4), Ai(1), Ai(0), Ai(0), Ai(0), Ai(6000) });
                    break;
                }
            }
        }
    }
    // блоки анимаций для людей (кроме «ped» — он всегда в памяти, его не заказывать)
    bool AnimsReady()
    {
        bool ok = true;
        for(const char* b : { "POLICE", "SMOKING", "MEDIC", "DEALER", "BAR", "CARRY" })
            if(!Is(0x04EE, { As(b) })) { Do(0x04ED, { As(b) }); ok = false; }                     // HAS_ANIMATION_LOADED / REQUEST_ANIMATION
        return ok;
    }

    // ---------------------------------------------------------------- тревога и оборона (Т141–Т158)
    bool OwnPed(uintptr_t p)
    {
        int r = Ref(p);
        for(auto& s : g_staff) if(s.ref == r) return true;
        return p == Player();
    }
    bool RaiderFilter(uintptr_t p)
    {
        int r = Ref(p);
        for(int x : g_raiders) if(x == r) return true;
        return false;
    }
    void DefSay(int ref, const std::string& who, const std::string& text, bool urgent)
    {
        (void)ref; (void)urgent;
        ui::Subtitle(who, text, 3.0f, 0xFF8A65);
    }
    void DefBackup(int kind, const V3& at) { wanted::CallBackup(kind, at.x, at.y, at.z, "оборона базы"); }
    void StartAlarm(const char* why)
    {
        if(g_alarm) return;
        g_alarm = true; g_alarmAt = Now(); g_quietSince = 0; g_alarmWhy = why;
        LOG("ОБОРОНА", "ТРЕВОГА: %s", why);
        ui::Title("ТРЕВОГА!", 3.0f);
        ui::Notice(std::string("Тревога на базе: ") + why, 6.0f);
        Do(0x018C, { Af(W(0, 0).x), Af(W(0, 0).y), Af(W(0, 0).z + 5), Ai(1057) });            // ADD_ONE_OFF_SOUND — сигнал
        if(!g_def) g_def = new Team("оборона базы");
        g_def->Clear();
        g_def->SetHome(Mk(W(0, 0).x, W(0, 0).y, W(0, 0).z), 95.0f);
        std::vector<V3> posts;
        for(auto& t : layout::kTowers) { game::Vec w = W(t.u * 0.92f, t.v * 0.9f, 0); posts.push_back(Mk(w.x, w.y, w.z)); }   // у вышек
        const float P[][3] = { { 26, 1, 0 }, { -26, 1, 0 }, { -46, -14, 0 }, { -30, 18, 0 }, { 12, 50, 0 }, { 30, 14, 0 } };  // углы штаба, ангары, городок, КПП, оружейная
        for(auto& q : P) { game::Vec w = W(q[0], q[1], q[2]); posts.push_back(Mk(w.x, w.y, w.z)); }
        { game::Vec w = W(layout::kPadU, layout::kPadV); posts.push_back(Mk(w.x, w.y, layout::kRoofZ)); }                    // крыша
        g_def->SetPosts(posts);
        game::Vec med = W(layout::kMedU, layout::kMedV), arm = W(layout::kArmoryU, layout::kArmoryV), cell = W(layout::kCellDropU, layout::kCellDropV), pad = W(layout::kPadU, layout::kPadV);
        pad.z = layout::kRoofZ;
        g_def->SetPlace(0, Mk(med.x, med.y, med.z)); g_def->SetPlace(1, Mk(arm.x, arm.y, arm.z));
        g_def->SetPlace(2, Mk(cell.x, cell.y, cell.z)); g_def->SetPlace(3, Mk(pad.x, pad.y, pad.z));
        g_def->SetEnemyFilter(RaiderFilter);
        g_def->SetSay(DefSay);
        g_def->SetBackup(DefBackup);
        g_def->SetStars(wanted::Stars);
        g_def->SetLeader(0);
        g_def->Auto(true);
        for(auto& s : g_staff)
            if(!s.released && s.ref > 0 && RefAlive(s.ref))
            {
                Do(0x02AB, { Ai(s.ref), Ai(0), Ai(0), Ai(0), Ai(0), Ai(0) });                 // в бою свои уязвимы
                g_def->Add(s.ref, s.name, s.kind == 0 ? R_MARKSMAN : R_LEAD, true);
            }
        Order o; o.tactic = 141; o.why = why; g_def->Command(o);
        for(auto& s : g_staff) if(s.ref > 0 && s.kind == 2 && !s.released) { SayLine(s, "Тревога! Все к оружию!"); break; }
    }
    void StopAlarm()
    {
        if(!g_alarm) return;
        g_alarm = false;
        LOG("ОБОРОНА", "отбой тревоги (Т155): все по местам");
        for(auto& s : g_staff)
            if(s.ref > 0 && !s.released)
            {
                if(g_def) g_def->Remove(s.ref);
                Do(0x02AB, { Ai(s.ref), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });
                Do(0x0687, { Ai(s.ref) });
                s.taskAt = 0;
            }
        if(g_gateBlocked && g_cars.size() && g_cars[0].handle > 0 && Is(0x056E, { Ai(g_cars[0].handle) }))
        {   // Enforcer из ворот — обратно на стоянку
            uintptr_t v = Veh(g_cars[0].handle);
            if(v && !VehDriver(v))
            {
                game::Vec b = W(g_cars[0].u, g_cars[0].v, 0.6f);
                Do(0x00AB, { Ai(g_cars[0].handle), Af(b.x), Af(b.y), Af(b.z) });               // SET_CAR_COORDINATES
                Do(0x0175, { Ai(g_cars[0].handle), Af(PlanToWorldH(g_cars[0].planH)) });
                LOG("ОБОРОНА", "Enforcer возвращён из ворот на стоянку");
            }
        }
        g_gateBlocked = false;
        ui::Notice("Отбой тревоги. Чисто, все по местам.", 5.0f);
        ui::Subtitle("Фрэнк", "Чисто. Доклад принят. По местам!", 3.5f, 0xF2C14E);   // Т153: доклад Артёму
        g_raiders.clear();
    }
    void AlarmTriggers()
    {
        if(g_alarm) return;
        V3 c = Mk(W(0, 0).x, W(0, 0).y, W(0, 0).z);
        // Т141: стрельба у базы, вооружённый враг за забором, кто-то стреляет по охране
        bool trig = false; const char* why = "";
        ForEachPed([&](uintptr_t p) {
            if(trig || OwnPed(p) || !PedAlive(p)) return;
            int t = PedType(p);
            if(t == 6 || t == 18 || t == 19) return;                                          // полиция/медики
            V3 pp = Pos(p);
            float d = Dist2(pp, c);
            if(d > 110) return;
            int ref = Ref(p);
            if(Shooting(ref)) { trig = true; why = "стрельба у базы"; g_raiders.push_back(ref); return; }
            uintptr_t aim = AimTarget(p);
            if(aim && OwnPed(aim) && CurWeapon(p) >= 22) { trig = true; why = "вооружённый враг целится в наших"; g_raiders.push_back(ref); }
        });
        if(trig) StartAlarm(why);
    }
    void DefenseFrame()
    {
        if(!g_alarm || !g_def) return;
        // все стреляющие рядом — враги (нападающие)
        V3 c = Mk(W(0, 0).x, W(0, 0).y, W(0, 0).z);
        ForEachPed([&](uintptr_t p) {
            if(OwnPed(p) || !PedAlive(p) || PedType(p) == 6) return;
            if(Dist2(Pos(p), c) > 120) return;
            int ref = Ref(p);
            uintptr_t aim = AimTarget(p);
            if((Shooting(ref) || (aim && OwnPed(aim))) && std::find(g_raiders.begin(), g_raiders.end(), ref) == g_raiders.end()) g_raiders.push_back(ref);
        });
        int alive = 0;
        for(int r : g_raiders) if(RefAlive(r)) alive++;
        // Т154: много нападающих — подмога из города (один раз)
        static uint32_t backupAt = 0;
        if(alive >= 6 && Ago(backupAt, 60000)) { backupAt = Now(); wanted::CallBackup(1, c.x, c.y, c.z, "много нападающих на базу"); ui::Subtitle("Фрэнк", "Центр, база атакована! Нужен S.W.A.T.!", 3.5f, 0xF2C14E); }
        // Т149: перекрыть въезд Enforcer'ом поперёк ворот
        if(!g_gateBlocked && g_cars.size() && g_cars[0].handle > 0)
        {
            uintptr_t v = Veh(g_cars[0].handle);
            if(v && !VehDriver(v))
            {
                game::Vec g = W(layout::kGateU, layout::kGateV - 4, 0.6f);
                Do(0x00AB, { Ai(g_cars[0].handle), Af(g.x), Af(g.y), Af(g.z) });               // SET_CAR_COORDINATES
                Do(0x0175, { Ai(g_cars[0].handle), Af(PlanToWorldH(90)) });
                g_gateBlocked = true;
                LOG("ОБОРОНА", "Т149: въезд перекрыт Enforcer'ом");
            }
        }
        // Т148: раненый свой — в медпункт, «выбыл», вернётся позже; насовсем не погибают
        for(auto& s : g_staff)
        {
            if(s.released || s.ref <= 0) continue;
            uintptr_t p = Ped(s.ref);
            if(!p) continue;
            if(!s.alarmHurt && Health(p) < 30)
            {
                s.alarmHurt = true; s.hurtAt = Now();
                Do(0x0223, { Ai(s.ref), Ai(30) });
                Do(0x02AB, { Ai(s.ref), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) });
                game::Vec m = W(layout::kMedU, layout::kMedV);
                if(g_def) g_def->Remove(s.ref);
                Do(0x05D3, { Ai(s.ref), Af(m.x), Af(m.y), Af(m.z), Ai(6), Ai(20000) });
                ui::Subtitle(s.name, "Меня задело! Ухожу в медпункт!", 3, 0xFF8A65);
            }
            if(s.alarmHurt && Ago(s.hurtAt, 45000) && g_alarm)
            {   // подлатали — снова в строю
                s.alarmHurt = false;
                Do(0x0223, { Ai(s.ref), Ai(100) });
                Do(0x02AB, { Ai(s.ref), Ai(0), Ai(0), Ai(0), Ai(0), Ai(0) });
                if(g_def) g_def->Add(s.ref, s.name, R_MARKSMAN, true);
            }
        }
        g_def->Tick();
        Event e;
        while(g_def->PopEvent(e)) LOG("ОБОРОНА", "%s: %s", e.kind.c_str(), e.text.c_str());
        if(alive == 0 && g_def->Threats() == 0) { if(!g_quietSince) g_quietSince = Now(); }
        else g_quietSince = 0;
        if(g_quietSince && Ago(g_quietSince, 15000)) StopAlarm();
        ui::Status(alive ? "ТРЕВОГА: на базе бой" : "ТРЕВОГА: осмотр периметра", 0xFF4D4D);
    }
    // учебное нападение (кабинет Фрэнка): банда у ворот
    void TrainingRaid()
    {
        std::vector<int> models = { 102, 103, 104 };
        if(!LoadModels(models)) { Do(0x038B, {}); }
        for(int i = 0; i < 5; ++i)
        {
            game::Vec p = W(-20.0f + i * 10.0f, 80.0f + (i % 2) * 6.0f, 0.5f);
            p.z = GroundZ(p.x, p.y, layout::kZ + 2) + 0.5f;
            int h = OutI(0x009A, { Ai(7), Ai(models[i % 3]), Af(p.x), Af(p.y), Af(p.z), Aoi() });
            if(h <= 0) continue;
            swat::GiveWeapon(h, i % 2 ? 28 : 30, 400);
            Do(0x02E2, { Ai(h), Ai(35) });
            uintptr_t guard = 0; for(auto& s : g_staff) if(s.kind == 1 && s.ref > 0) { guard = Ped(s.ref); break; }
            if(guard) Do(0x05E2, { Ai(h), Ai(Ref(guard)) });                                  // TASK_KILL_CHAR_ON_FOOT
            Do(0x01C2, { Ai(h) });
            g_raiders.push_back(h);
        }
        for(int m : models) Do(0x0249, { Ai(m) });
        LOG("ОБОРОНА", "учебное нападение: 5 бандитов у ворот");
        StartAlarm("нападение у ворот (учебное)");
    }

    // ---------------------------------------------------------------- маркеры и меню
    enum MarkerId { M_SAVE_A, M_SAVE_M, M_ARMORY, M_MED, M_LIFT_UP, M_LIFT_DOWN, M_FRANK, M_RANGE, M_OWN_GARAGE, M_CELL, M_COUNT };
    struct Marker { int id; float u, v; float dz; bool inCar; const char* label; const char* sign; const char* hint; };
    const Marker kMarkers[] = {
        { M_SAVE_A, layout::kArtyomU, layout::kArtyomV, 0, false, "Комната Артёма", "КОМНАТА АРТЁМА", "сохранить игру, отдохнуть" },
        { M_SAVE_M, layout::kMagaU, layout::kMagaV, 0, false, "Комната Маги", "КОМНАТА МАГИ", "сохранить игру, отдохнуть" },
        { M_ARMORY, layout::kArmoryU, layout::kArmoryV, 0, false, "Оружейная", "ОРУЖЕЙНАЯ", "оружие и бронежилет" },
        { M_MED, layout::kMedU, layout::kMedV, 0, false, "Медпункт", "МЕДПУНКТ", "встань в круг — вылечат" },
        { M_LIFT_UP, layout::kLiftU, layout::kLiftV, 0, false, "Наверх", "НА КРЫШУ", "к вертолётам" },
        { M_LIFT_DOWN, layout::kRoofLiftU, layout::kRoofLiftV, layout::kRoofZ - layout::kZ, false, "Вниз", "ВНИЗ", "во двор" },
        { M_FRANK, layout::kFrankU, layout::kFrankV, 0, false, "Кабинет Фрэнка", "ШТАБ — ФРЭНК", "вызовы, зарплата, сводка" },
        { M_RANGE, layout::kRangeStandU + 3.0f, layout::kRangeV, 0, false, "Тир", "ТИР", "стрельба на время" },
        { M_OWN_GARAGE, layout::kOwnGarageU, layout::kOwnGarageV, 0, true, "Личный гараж", "ЛИЧНЫЙ ГАРАЖ", "заезжай на машине" },
        { M_CELL, layout::kCellDropU, layout::kCellDropV, 0, false, "Сдать задержанного", "КАМЕРА", "сдать задержанного" },
    };

    void Fade(int ms, bool in) { Do(0x016A, { Ai(ms), Ai(in ? 1 : 0) }); }
    void Teleport(const game::Vec& p, float h)
    {
        Do(0x00A1, { Ai(PlayerChar()), Af(p.x), Af(p.y), Af(p.z) });                          // SET_CHAR_COORDINATES
        Do(0x0173, { Ai(PlayerChar()), Af(h) });
        Do(0x0373, {});                                                                       // SET_CAMERA_BEHIND_PLAYER
    }
    void ArmoryMenu()
    {
        static const struct { const char* name; int w; int ammo; } W8[] = {
            { "Пистолет", 22, 170 }, { "Пистолет с глушителем", 23, 170 }, { "Desert Eagle", 24, 70 }, { "Дробовик", 25, 60 },
            { "Боевой дробовик (SPAS)", 27, 90 }, { "Пистолет-пулемёт MP5", 29, 300 }, { "Автомат M4", 31, 300 }, { "Бронежилет", -1, 0 }, { "Взять всё", -2, 0 } };
        std::vector<std::string> items; for(auto& w : W8) items.push_back(w.name);
        ui::OpenMenu("ОРУЖЕЙНАЯ", items, [](int i) {
            if(i < 0) return;
            auto give = [](int w, int ammo) { swat::GiveWeapon(PlayerChar(), w, ammo); };
            if(W8[i].w == -1) { Do(0x035F, { Ai(PlayerChar()), Ai(100) }); ui::Notice("Бронежилет надет"); }
            else if(W8[i].w == -2) { for(auto& x : W8) if(x.w > 0 && x.w != 23) give(x.w, x.ammo); Do(0x035F, { Ai(PlayerChar()), Ai(100) }); ui::Notice("Полный комплект: оружие, патроны, броня"); }
            else { give(W8[i].w, W8[i].ammo); Do(0x01B9, { Ai(PlayerChar()), Ai(W8[i].w) }); ui::Notice(std::string(W8[i].name) + " — патроны пополнены"); }
            LOG("БАЗА", "оружейная: %s", W8[i].name);
        }, "Бесплатно. Только полицейское оружие.");
    }
    void RoomMenu(bool artyom)
    {
        ui::OpenMenu(artyom ? "КОМНАТА АРТЁМА" : "КОМНАТА МАГИ", { "Сохранить игру", "Отдохнуть до утра (7:00)", "Отдохнуть до вечера (19:00)", "Отдохнуть до ночи (23:00)" }, [](int i) {
            if(i < 0) return;
            ui::CloseMenu();
            if(i == 0) { Do(0x03D8, {}); LOG("БАЗА", "сохранение игры (комната)"); return; }   // ACTIVATE_SAVE_MENU
            int h = i == 1 ? 7 : (i == 2 ? 19 : 23);
            Fade(500, false);
            Do(0x00C0, { Ai(h), Ai(0) });                                                        // SET_TIME_OF_DAY
            Fade(1200, true);
            ui::Notice("Отдохнул. Время — " + std::to_string(h) + ":00");
            LOG("БАЗА", "отдых до %d:00", h);
        }, "Переодеться — позже.");
    }
    void FrankMenu()
    {
        int look = story::GetInt("look", 1);
        std::vector<std::string> items = { "Взять вызов", "Зарплата", "Сводка", std::string("Вид базы: ") + (look ? "Готэм" : "Обычная") + " — сменить", "Учебная тревога (нападение на базу)" };
        ui::OpenMenu("КАБИНЕТ ФРЭНКА", items, [](int i) {
            if(i < 0) return;
            if(i == 0)
            {
                ui::CloseMenu();
                if(wanted::StartCall()) { AddCall(); ui::Subtitle("Фрэнк", "Есть вызов. Не облажайся.", 3.5f, 0xF2C14E); }
                else ui::Subtitle("Фрэнк", "Ты ещё прошлый не закрыл.", 3.0f, 0xF2C14E);
            }
            else if(i == 1)
            {
                int day = game::GameDay(), last = story::GetInt("salary_day", -1);
                if(day == last) ui::Subtitle("Фрэнк", "Зарплата раз в сутки. Приходи завтра.", 3.0f, 0xF2C14E);
                else { story::SetInt("salary_day", day); game::AddMoney(350); AddEarned(350); ui::Notice("+$350 — зарплата"); ui::Subtitle("Фрэнк", "Держи. Не пропей.", 3.0f, 0xF2C14E); LOG("БАЗА", "зарплата $350 (день %d)", day); }
            }
            else if(i == 2)
            {
                char b[200]; snprintf(b, sizeof(b), "Задержано: %d   Вызовов: %d   Заработано: $%d", story::GetInt("arrests", 0), story::GetInt("calls", 0), story::GetInt("earned", 0));
                ui::Notice(b, 8.0f);
            }
            else if(i == 3) { ui::CloseMenu(); SetLook(story::GetInt("look", 1) ? 0 : 1); }
            else if(i == 4) { ui::CloseMenu(); TrainingRaid(); }
        });
    }
    void GarageMenu(int carRef)
    {
        // в машине — поставить на хранение; пешком — забрать
        if(carRef > 0)
        {
            int model = OutI(0x0441, { Ai(carRef), Aoi() });                                    // GET_CAR_MODEL
            std::vector<A> c = { Ai(carRef), Aoi(), Aoi() }; Cmd(0x03F3, c);                     // GET_CAR_COLOURS
            int n = story::GetInt("garage_n", 0);
            if(n >= 4) { ui::Notice("Личный гараж полон (4 машины)"); return; }
            story::SetInt("garage_m" + std::to_string(n), model);
            story::SetInt("garage_c" + std::to_string(n), c[1].i * 256 + c[2].i);
            story::SetInt("garage_n", n + 1);
            Do(0x0362, { Ai(PlayerChar()), Af(W(layout::kOwnGarageU + 6, layout::kOwnGarageV + 4).x), Af(W(layout::kOwnGarageU + 6, layout::kOwnGarageV + 4).y), Af(layout::kZ + 0.5f) });   // WARP_CHAR_FROM_CAR_TO_COORD
            Do(0x00A6, { Ai(carRef) });                                                         // DELETE_CAR
            ui::Notice("Машина поставлена в личный гараж (запишется при сохранении игры)");
            LOG("БАЗА", "личный гараж: машина %d на хранении (%d из 4)", model, n + 1);
            return;
        }
        int n = story::GetInt("garage_n", 0);
        if(n <= 0) { ui::Notice("В личном гараже пусто. Заезжай в метку на машине — она останется тут."); return; }
        std::vector<std::string> items;
        for(int i = 0; i < n; ++i) items.push_back("Машина №" + std::to_string(i + 1) + " (модель " + std::to_string(story::GetInt("garage_m" + std::to_string(i), 0)) + ")");
        ui::OpenMenu("ЛИЧНЫЙ ГАРАЖ", items, [](int i) {
            if(i < 0) return;
            ui::CloseMenu();
            int n = story::GetInt("garage_n", 0);
            int model = story::GetInt("garage_m" + std::to_string(i), 0), col = story::GetInt("garage_c" + std::to_string(i), 0);
            if(!model) return;
            if(!LoadModels({ model })) { Do(0x038B, {}); }
            game::Vec p = W(layout::kParkU + (i % 4) * 5.0f, layout::kParkV, 0.6f);
            int h = OutI(0x00A5, { Ai(model), Af(p.x), Af(p.y), Af(p.z), Aoi() });
            if(h > 0) { Do(0x0175, { Ai(h), Af(PlanToWorldH(0)) }); Do(0x0229, { Ai(h), Ai(col / 256), Ai(col % 256) }); Do(0x01C3, { Ai(h) }); }
            Do(0x0249, { Ai(model) });
            // убрать из списка
            for(int k = i; k + 1 < n; ++k) { story::SetInt("garage_m" + std::to_string(k), story::GetInt("garage_m" + std::to_string(k + 1), 0)); story::SetInt("garage_c" + std::to_string(k), story::GetInt("garage_c" + std::to_string(k + 1), 0)); }
            story::SetInt("garage_n", n - 1);
            ui::Notice("Машина на парковке у штаба");
        });
    }
    // тир: 40 секунд, мишени «поднимаются» по одной — попал (объект повреждён) → очко; растёт навык оружия игры
    void RangeStart()
    {
        if(g_range.on) return;
        g_range = Range(); g_range.on = true; g_range.until = Now() + 40000; g_range.nextPop = Now() + 1500;
        ui::Title("ТИР", 2.0f); ui::Objective("Сбивай мишени! 40 секунд", 5.0f);
        LOG("БАЗА", "тир: старт");
    }
    void RangeFrame()
    {
        if(!g_range.on) return;
        int left = (int)((g_range.until - Now()) / 1000);
        if(Now() >= g_range.until)
        {
            g_range.on = false;
            if(g_range.target > 0 && Is(0x03CA, { Ai(g_range.target) })) Do(0x0108, { Ai(g_range.target) });
            ui::Timer(-1); ui::Counter("", -1);
            int best = story::GetInt("range_best", 0);
            bool rec = g_range.score > best; if(rec) story::SetInt("range_best", g_range.score);
            ui::Result(true, rec ? "НОВЫЙ РЕКОРД!" : "ТИР ЗАВЕРШЁН", std::to_string(g_range.score) + " попаданий (рекорд " + std::to_string(rec ? g_range.score : best) + ")");
            // навык оружия игры: попадания → стат навыка текущего оружия
            uintptr_t p = Player(); int w = CurWeapon(p);
            int stat = w == 22 ? 69 : w == 23 ? 70 : w == 24 ? 71 : w == 25 ? 72 : w == 27 ? 74 : w == 29 ? 76 : w == 31 ? 78 : 0;
            if(stat && g_range.score) Do(0x0623, { Ai(stat), Ai(g_range.score * 10) });         // INCREMENT_INT_STAT
            LOG("БАЗА", "тир: %d попаданий, навык оружия %d +%d", g_range.score, w, g_range.score * 10);
            return;
        }
        ui::Timer(left); ui::Counter("Попаданий", g_range.score);
        if(g_range.target > 0)
        {
            bool hit = Is(0x0366, { Ai(g_range.target) });                                       // HAS_OBJECT_BEEN_DAMAGED
            if(hit || Ago(g_range.popAt, 2500))
            {
                if(hit) { g_range.score++; Do(0x018C, { Af(Pos(Player()).x), Af(Pos(Player()).y), Af(Pos(Player()).z), Ai(1056) }); }
                Do(0x0108, { Ai(g_range.target) }); g_range.target = 0; g_range.nextPop = Now() + (uint32_t)RndI(300, 900);
            }
        }
        else if(Now() >= g_range.nextPop)
        {
            g_range.lane = RndI(0, 3);
            game::Vec t = W(layout::kRangeTargetsU - 1.5f, layout::kRangeV - 4.5f + g_range.lane * 3.0f, 1.4f);
            int h = OutI(0x0107, { Ai(1218), Af(t.x), Af(t.y), Af(t.z), Aoi() });                // barrel1 как мишень-поплавок (у мишеней тира нет «урона»)
            if(h > 0) { Do(0x0392, { Ai(h), Ai(0) }); g_range.target = h; g_range.popAt = Now(); }
        }
    }

    void MarkersFrame()
    {
        if(ui::MenuOpen()) return;
        int pc = PlayerChar();
        bool inCar = PedVehicle(Player()) != 0;
        int now = -1;
        for(auto& m : kMarkers)
        {
            game::Vec p = W(m.u, m.v, m.dz);
            float md = Dist(Pos(Player()), Mk(p.x, p.y, p.z));
            if(md > 120) continue;
            // надпись над точкой: издалека — название, вблизи — ещё и что тут делают
            if(md < 90) { game::Vec sg = p; sg.z += 3.2f; ui::Sign(sg, m.sign, md < 18 ? m.hint : "", md < 40 ? 0xF2C14E : 0xD8DCE2); }
            bool in = m.inCar ? Is(0x00FE, { Ai(pc), Af(p.x), Af(p.y), Af(p.z), Af(3.5f), Af(3.5f), Af(2.5f), Ai(1) })   // LOCATE_CHAR_ANY_MEANS_3D (рисует круг)
                              : (!inCar && Is(0x00FF, { Ai(pc), Af(p.x), Af(p.y), Af(p.z), Af(2.0f), Af(2.0f), Af(2.0f), Ai(1) }));   // LOCATE_CHAR_ON_FOOT_3D
            if(in) now = m.id;
        }
        if(now == g_inMarker) return;
        g_inMarker = now;
        if(now < 0) return;
        LOG("БАЗА", "маркер: %s", kMarkers[now].label);
        switch(now)
        {
            case M_SAVE_A: RoomMenu(true); break;
            case M_SAVE_M: RoomMenu(false); break;
            case M_ARMORY: ArmoryMenu(); break;
            case M_MED:
                Do(0x0223, { Ai(pc), Ai(176) });                                                  // SET_CHAR_HEALTH (игра сама ограничит максимумом игрока)
                Do(0x035F, { Ai(pc), Ai(100) });                                                  // ADD_ARMOUR_TO_CHAR — заодно бронежилет
                ui::Notice("Медпункт: здоровье и броня восстановлены");
                break;
            case M_LIFT_UP:
            {
                Fade(400, false);
                game::Vec r = W(layout::kRoofLiftU - 2.5f, layout::kRoofLiftV - 2.5f, layout::kRoofZ - layout::kZ + 0.5f);
                Teleport(r, PlanToWorldH(180)); Fade(700, true);
                g_inMarker = M_LIFT_DOWN;
                break;
            }
            case M_LIFT_DOWN:
            {
                Fade(400, false);
                game::Vec r = W(layout::kLiftU, layout::kLiftV + 3.0f, 0.5f);
                Teleport(r, PlanToWorldH(0)); Fade(700, true);
                g_inMarker = M_LIFT_UP;
                break;
            }
            case M_FRANK: FrankMenu(); break;
            case M_RANGE: RangeStart(); break;
            case M_OWN_GARAGE:
            {
                uintptr_t v = PedVehicle(Player());
                GarageMenu(v && VehDriver(v) == Player() ? VehRef(v) : 0);
                break;
            }
            case M_CELL: wanted::DeliverAtCell(); break;
        }
    }

    void JailFrame()
    {
        for(auto it = g_jailed.begin(); it != g_jailed.end();)
        {
            if(Ago(it->at, 30000)) { if(Is(0x056D, { Ai(it->ref) })) Do(0x009B, { Ai(it->ref) }); it = g_jailed.erase(it); }   // посидел — увели
            else ++it;
        }
    }
}

// ======================================================================================================
bool Init(void* h)
{
    g_objPoolSym = aml->GetSym(h, "_ZN6CPools14ms_pObjectPoolE");
    g_modelInfoPtrs = aml->GetSym(h, "_ZN10CModelInfo16ms_modelInfoPtrsE");
    g_vehPoolSym = aml->GetSym(h, "_ZN6CPools15ms_pVehiclePoolE");
    return true;
}

void Reset()
{
    g_objs.clear(); g_gate[0] = g_gate[1] = 0; g_gateOpen = 0; g_gateWant = false; g_builtLook = -1; g_built = false; g_peopleOn = false; g_blipBase = 0;
    for(auto& s : g_staff) { s.ref = 0; s.released = false; }
    for(auto& c : g_cars) { c.handle = 0; c.goneAt = 0; }
    for(auto& hh : g_helis) { hh.handle = 0; hh.goneAt = 0; }
    g_alarm = false; g_raiders.clear(); if(g_def) g_def->Clear();
    g_range = Range(); g_jailed.clear(); g_inMarker = -1; g_introHeli = 0; g_holdPad = false; g_gateBlocked = false; g_blips.clear();
}

void BeforeSave()
{
    // ничего не удаляем: автосохранение идёт посреди отрисовки — удаление роняет игру. Копии из сохранения
    // убирает CleanStale после загрузки. Значок базы снимаем — иначе после загрузки их будет два.
    if(g_blipBase > 0) { Do(0x0164, { Ai(g_blipBase) }); g_blipBase = 0; }
    for(int b : g_blips) Do(0x0164, { Ai(b) });
    g_blips.clear();
}

void Teardown()
{
    if(!g_built) return;
    LOG("БАЗА", "игрок далеко — созданное базой убрано (появится снова, когда вернёшься)");
    DeleteObjs();
    for(auto& s : g_staff) RemoveStaff(s, true);
    for(auto& c : g_cars) { if(c.handle > 0 && Is(0x056E, { Ai(c.handle) })) { uintptr_t v = Veh(c.handle); if(v && !VehDriver(v)) Do(0x00A6, { Ai(c.handle) }); else Do(0x01C3, { Ai(c.handle) }); } c.handle = 0; c.goneAt = 0; }
    for(auto& hh : g_helis) { if(hh.handle > 0 && Is(0x056E, { Ai(hh.handle) })) { uintptr_t v = Veh(hh.handle); if(v && !VehDriver(v)) Do(0x00A6, { Ai(hh.handle) }); else Do(0x01C3, { Ai(hh.handle) }); } hh.handle = 0; hh.goneAt = 0; }
    if(g_blipBase > 0) Do(0x0164, { Ai(g_blipBase) });
    g_blipBase = 0; g_built = false; g_peopleOn = false;
}

void Frame()
{
    uintptr_t pl = Player();
    if(!pl) return;
    game::Vec c = W(0, 0);
    float d = Dist2(Pos(pl), Mk(c.x, c.y, c.z));
    uint32_t now = Now();
    bool wantBuild = d < 700.0f;                                       // заранее: на подлёте база уже стоит целиком
    if(wantBuild && (!g_built || g_builtLook != story::GetInt("look", 1)))
    {
        if(g_built) { DeleteObjs(); g_built = false; }
        WHERE("база: постройка");
        g_built = Build();
        WHERE("кадр: S.W.A.T.");
    }
    if(!wantBuild && g_built && d > 900.0f) { Teardown(); return; }   // уехал далеко — освободить память
    if(!g_built) return;
    if(g_blipBase <= 0)
    {
        game::Vec bc = W(0, 0);
        g_blipBase = OutI(0x02A8, { Af(bc.x), Af(bc.y), Af(bc.z), Ai(30), Aoi() });           // значок базы на всей карте (снимается перед сохранением)
    }
    if(g_blips.empty() && d < 400.0f)
    {   // значки точек базы на мини-карте: медпункт, оружейная, комната (сохранение), гараж
        const struct { float u, v; int sprite; } B[] = { { layout::kMedU, layout::kMedV, 22 }, { layout::kArmoryU, layout::kArmoryV, 6 },
            { layout::kArtyomU, layout::kArtyomV, 35 }, { layout::kOwnGarageU, layout::kOwnGarageV, 55 } };
        for(auto& b : B) { game::Vec w = W(b.u, b.v); int h = OutI(0x04CE, { Af(w.x), Af(w.y), Af(w.z), Ai(b.sprite), Aoi() }); if(h > 0) g_blips.push_back(h); }
    }
    if(now >= g_nextCars || now + 10000 < g_nextCars) { g_nextCars = now + 1500; WHERE("база: транспорт"); KeepCars(); }
    WHERE("база: ворота"); Gate();
    IntruderLog();
    g_live = d < layout::kLiveRadius;
    if(g_live)
    {
        // надпись при заходе в ворота
        static bool wasInside = false;
        bool inside = Inside(game::PlayerPos(), 0);
        if(inside && !wasInside && !g_holdPad) ui::Title("БАЗА S.W.A.T.", 2.5f);
        wasInside = inside;
        if(AnimsReady() && (now >= g_nextAmbient || now + 10000 < g_nextAmbient)) { g_nextAmbient = now + 400; WHERE("база: люди"); PeopleFrame(); }
        g_peopleOn = true;
        WHERE("база: маркеры"); MarkersFrame();
        WHERE("база: тир"); RangeFrame();
        WHERE("база: тревога"); AlarmTriggers(); DefenseFrame();
        JailFrame();
    }
    else if(g_peopleOn && d > layout::kLiveRadius + 30)
    {   // уехал — люди исчезают (экономия памяти), база «засыпает»; нападений, пока ты далеко, нет
        for(auto& s : g_staff) RemoveStaff(s, true);
        StopAlarm();
        g_peopleOn = false;
        LOG("БАЗА", "игрок уехал — люди базы убраны до возвращения");
    }
}

bool Live() { return g_live; }
bool Built() { return g_built; }
bool Inside(const game::Vec& p, float pad)
{
    return p.x > layout::kCx - layout::kHalfU - pad && p.x < layout::kCx + layout::kHalfU + pad && p.y > layout::kCy - layout::kHalfV - pad && p.y < layout::kCy + layout::kHalfV + pad;
}
int StaffCount() { return (int)g_staff.size(); }
int StaffRef(int i) { return i >= 0 && i < (int)g_staff.size() ? g_staff[i].ref : 0; }
const char* StaffName(int i) { return i >= 0 && i < (int)g_staff.size() ? g_staff[i].name : ""; }
int StaffKind(int i) { return i >= 0 && i < (int)g_staff.size() ? g_staff[i].kind : 0; }
int StaffModel(int i) { return i >= 0 && i < (int)g_staff.size() ? g_staff[i].model : 0; }
int StaffIndexOf(int ref) { for(size_t i = 0; i < g_staff.size(); ++i) if(ref > 0 && g_staff[i].ref == ref) return (int)i; return -1; }
void ReleaseStaff(int ref)
{
    int i = StaffIndexOf(ref); if(i < 0) return;
    Staff& s = g_staff[i];
    if(s.kind == 2) { LOG("БАЗА", "Фрэнк в отряд не идёт — всегда на базе"); return; }
    s.released = true;
    if(g_def) g_def->Remove(ref);
    Do(0x02AB, { Ai(ref), Ai(0), Ai(0), Ai(0), Ai(0), Ai(0) });                               // в отряде — уязвим (не бессмертный)
    Do(0x0568, { Ai(ref), Ai(0) });
    LOG("БАЗА", "%s ушёл в отряд Артёма", s.name);
}
void ReturnStaff(int ref)
{
    for(auto& s : g_staff)
        if(s.released && (s.ref == ref || ref == 0))
        {
            s.released = false;
            if(s.ref > 0 && RefAlive(s.ref)) { Do(0x02AB, { Ai(s.ref), Ai(1), Ai(1), Ai(1), Ai(1), Ai(1) }); Do(0x0568, { Ai(s.ref), Ai(1) }); s.taskAt = 0; }
            else s.ref = 0;
            LOG("БАЗА", "%s вернулся на базу", s.name);
        }
}
bool AlarmOn() { return g_alarm; }
void RaiseAlarm(const char* why) { if(g_live) StartAlarm(why ? why : "по рации"); }
void DeliverPrisoner(int ref)
{
    game::Vec in = W(layout::kCellU, layout::kCellV, 0.5f);
    if(ref > 0 && Is(0x056D, { Ai(ref) }))
    {
        Do(0x0687, { Ai(ref) });
        Do(0x00A1, { Ai(ref), Af(in.x), Af(in.y), Af(in.z) });                                 // в камеру
        Do(0x05BA, { Ai(ref), Ai(30000) });                                                    // TASK_STAND_STILL
        g_jailed.push_back({ ref, Now() });
    }
    story::SetInt("arrests", story::GetInt("arrests", 0) + 1);
}
int Look() { return story::GetInt("look", 1); }
void SetLook(int look)
{
    story::SetInt("look", look);
    LOG("БАЗА", "вид базы: %s", look ? "Готэм" : "Обычная");
    ui::Notice(look ? "Вид базы: «Готэм» — пасмурно, дождь" : "Вид базы: «Обычная»", 5.0f);
    if(look) { int w = RndI(0, 3) ? 4 : 8; Do(0x01B5, { Ai(w) }); Do(0x01B6, { Ai(w) }); }    // FORCE_WEATHER + _NOW: пасмурно, реже дождь (туман 9 убран — не видно базы)
    else Do(0x01B7, {});                                                                       // RELEASE_WEATHER
}
// Родной скрипт игры сбрасывает погоду — раз в 5 с проверяем и возвращаем «Готэм»
void KeepWeather()
{
    static int16_t* forced = nullptr; static bool tried = false;
    static uint32_t next = 0;
    if(!tried) { tried = true; forced = (int16_t*)aml->GetSym(aml->GetLibHandle("libGTASA.so"), "_ZN8CWeather17ForcedWeatherTypeE"); }
    uint32_t now = Now();
    if(now < next && next - now < 10000) return;
    next = now + 5000;
    if(!forced || Look() != 1) return;
    if(*forced == 4 || *forced == 8) return;
    LOG("БАЗА", "погода сброшена игрой (%d) — снова «Готэм»", (int)*forced);
    int w = RndI(0, 3) ? 4 : 8;
    Do(0x01B5, { Ai(w) });                                                                     // FORCE_WEATHER
    Do(0x01B6, { Ai(w) });                                                                     // FORCE_WEATHER_NOW
}
void HoldPad(bool on) { g_holdPad = on; }
void HeliIntroLanded(int heliRef) { g_holdPad = false; g_introHeli = heliRef; for(auto& hh : g_helis) if(hh.model == 497) { if(hh.handle > 0 && hh.handle != heliRef) Do(0x00A6, { Ai(hh.handle) }); hh.handle = heliRef; } }
void AddCall() { story::SetInt("calls", story::GetInt("calls", 0) + 1); }
void AddEarned(int usd) { story::SetInt("earned", story::GetInt("earned", 0) + usd); }
}
