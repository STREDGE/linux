// Проверка «знаний» Джарвиса на компьютере: разбор фраз без ИИ, сетка карты, справочник команд.
//   jarvis_test [путь к sa_mobile.json]
#include <stdio.h>
#include <string>
#include "jarvis/brain.h"

namespace script { bool Init(void*) { return true; } bool Ready() { return true; } bool Run(uint16_t, std::vector<Arg>&, bool*) { return true; } bool Do(uint16_t, std::vector<Arg>) { return true; } int PlayerChar() { return 1; } int CarHandle(uintptr_t) { return -1; } }

static int g_fail = 0, g_total = 0;
static void Expect(const char* phrase, const char* want)
{
    auto cmds = jbrain::ParseOffline(phrase);
    std::string got;
    for(auto& c : cmds) { if(!got.empty()) got += " ; "; got += c.tool + " " + c.in.dump(); }
    bool ok = got.find(want) != std::string::npos;
    ++g_total; if(!ok) ++g_fail;
    printf("%s «%s»\n      → %s%s%s\n", ok ? "OK  " : "FAIL", phrase, got.empty() ? "(не понял)" : got.c_str(), ok ? "" : "\n      ждал: ", ok ? "" : want);
}
int main(int argc, char** argv)
{
    Expect("Дай миниган и сто патронов", "give_weapon {\"weapon\":\"minigun\",\"ammo\":100");
    Expect("дай калаш", "\"weapon\":\"ak47\"");
    Expect("Дай мне снайперку, пожалуйста", "\"weapon\":\"sniper\"");
    Expect("выдай боевой дробовик пятьсот патронов", "\"weapon\":\"spas\",\"ammo\":500");
    Expect("пистолет с глушителем", "\"weapon\":\"silenced\"");
    Expect("дай РПГ другу", "\"to\":\"friend\"");
    Expect("полный арсенал", "give_weapon");
    Expect("забери оружие", "take_weapons");
    Expect("Сними розыск", "wanted {\"level\":0}");
    Expect("дай четыре звезды", "wanted {\"level\":4}");
    Expect("Сделай так, чтобы копы меня не трогали", "\"immune\":true");
    Expect("розыск шесть звёзд", "wanted {\"level\":6}");
    Expect("Перенеси меня в квадрат C7", "teleport {\"place\":\"C7\"}");
    Expect("телепортируй в квадрат с 7.3", "teleport {\"place\":\"C7.3\"}");
    Expect("перенеси на метку", "teleport {\"place\":\"waypoint\"}");
    Expect("перенеси меня на Гроув-стрит", "Гроув");
    Expect("отправь меня в аэропорт Лас-Вентураса", "Аэропорт Лас-Вентураса");
    Expect("хочу на гору Чилиад", "Чилиад");
    Expect("перенеси меня на базу", "teleport {\"place\":\"баз");
    Expect("верни меня назад", "\"place\":\"back\"");
    Expect("запомни это место как база", "place {\"action\":\"save\",\"name\":\"база\"}");
    Expect("полное здоровье и броня", "\"health\":100,\"armour\":100");
    Expect("включи бессмертие", "\"god\":true");
    Expect("выключи бессмертие", "\"god\":false");
    Expect("дай сто тысяч долларов", "\"money\":100000");
    Expect("дай денег два миллиона", "\"money\":2000000");
    Expect("дай инфернус", "vehicle {\"action\":\"spawn\",\"model\":411}");
    Expect("хочу танк", "\"model\":432");
    Expect("подгони вертолёт", "\"model\":487");
    Expect("дай нрг", "\"model\":522");
    Expect("почини машину", "\"action\":\"repair\"");
    Expect("переверни машину", "\"action\":\"flip\"");
    Expect("поставь нитро", "\"action\":\"nitro\"");
    Expect("дай джетпак", "\"action\":\"jetpack\"");
    Expect("сделай ночь", "world {\"hour\":0}");
    Expect("включи дождь", "\"weather\":8");
    Expect("поставь время двенадцать часов", "\"hour\":12");
    Expect("замедли время", "\"time_scale\":0.4");
    Expect("убери пробки", "\"traffic\":0");
    Expect("прокачай мышцы на максимум", "stat {\"name\":\"Мышцы\",\"value\":1000}");
    Expect("где я?", "info");
    Expect("отмени", "undo");
    Expect("устрой войну: ночь, дождь, шесть звёзд, миниган, броня", "wanted");

    float x, y; std::string n;
    printf("\nсетка: C7 → %s", jbrain::ParseGrid("в квадрат C7", x, y, n) ? "" : "FAIL"); printf(" %.0f, %.0f (%s), обратно %s\n", x, y, n.c_str(), jbrain::GridOf(x, y).c_str());
    printf("сетка: Гроув-стрит (2495, -1668) = %s; центр карты = %s\n", jbrain::GridOf(2495, -1668).c_str(), jbrain::GridOf(0, 0).c_str());
    long long v; jbrain::ParseNumber("две тысячи пятьсот", v); printf("число: «две тысячи пятьсот» = %lld\n", v);
    jbrain::ParseNumber("сто пятьдесят тысяч", v); printf("число: «сто пятьдесят тысяч» = %lld\n", v);

    if(argc > 1)
    {
        std::string err;
        if(!jbrain::LoadLibrary(argv[1], err)) { printf("справочник: %s\n", err.c_str()); return 1; }
        printf("\nсправочник: %d команд\n", jbrain::LibrarySize());
        printf("--- поиск «create car» ---\n%s", jbrain::Search("create car", 5).c_str());
        printf("--- поиск «explosion» ---\n%s", jbrain::Search("explosion add", 4).c_str());
        printf("--- поиск «jetpack» ---\n%s", jbrain::Search("jetpack", 3).c_str());
        jbrain::Context ctx; ctx.playerChar = 77; jbrain::Compiled c;
        json::Value a; json::Parse("[411, 2495.0, -1668, 13.5]", a);
        bool ok = jbrain::Compile("create_car", a, ctx, c, err);
        printf("CREATE_CAR: %s опкод %04X, параметров %d, выходов %d %s\n", ok ? "ok" : "FAIL", c.opcode, (int)c.args.size(), (int)c.outNames.size(), err.c_str());
        json::Parse("[\"$me\", \"$1\"]", a); ctx.prev = { 1234 };
        ok = jbrain::Compile("WARP_CHAR_INTO_CAR", a, ctx, c, err);
        printf("WARP_CHAR_INTO_CAR: %s (%d, %d) %s\n", ok ? "ok" : "FAIL", ok ? c.args[0].i : 0, ok ? c.args[1].i : 0, err.c_str());
        json::Parse("[0]", a);
        ok = jbrain::Compile("GOTO", a, ctx, c, err); printf("GOTO: %s (%s)\n", ok ? "FAIL — должна быть запрещена" : "запрещена", err.c_str());
        ok = jbrain::Compile("WAIT", a, ctx, c, err); printf("WAIT: %s\n", ok ? "FAIL — должна быть запрещена" : "запрещена");
        json::Parse("[\"infernus\", 1, 2, 3]", a);
        ok = jbrain::Compile("CREATE_CAR", a, ctx, c, err); printf("CREATE_CAR по имени: %s модель %d\n", ok ? "ok" : err.c_str(), ok ? c.args[0].i : 0);
    }
    printf("\nИТОГО фраз: %d, ошибок: %d\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
