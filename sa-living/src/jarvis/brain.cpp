#include "brain.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>

namespace jbrain
{
// ---------- текст ----------
std::string Lower(const std::string& s)
{
    std::string o; o.reserve(s.size());
    for(size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        if(c < 0x80) { o += (char)tolower(c); ++i; continue; }
        if((c == 0xD0 || c == 0xD1) && i + 1 < s.size())
        {
            unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            if(cp >= 0x410 && cp <= 0x42F) cp += 0x20;
            if(cp == 0x401 || cp == 0x451) cp = 0x435;          // Ё, ё → е
            o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F));
            i += 2; continue;
        }
        o += (char)c; ++i;
    }
    return o;
}

static bool Has(const std::string& t, const char* word) { return t.find(word) != std::string::npos; }
static bool HasAny(const std::string& t, std::initializer_list<const char*> words) { for(auto w : words) if(Has(t, w)) return true; return false; }

// слова текста (буквы/цифры), уже в нижнем регистре
static std::vector<std::string> Words(const std::string& t)
{
    std::vector<std::string> w; std::string cur;
    for(size_t i = 0; i < t.size();)
    {
        unsigned char c = t[i];
        if(c >= 0x80 && i + 1 < t.size()) { cur += t[i]; cur += t[i + 1]; i += 2; continue; }
        if(isalnum(c) || c == '.') cur += (char)c;
        else if(!cur.empty()) { w.push_back(cur); cur.clear(); }
        ++i;
    }
    if(!cur.empty()) w.push_back(cur);
    for(auto& x : w) while(!x.empty() && x.back() == '.') x.pop_back();
    w.erase(std::remove_if(w.begin(), w.end(), [](const std::string& x) { return x.empty(); }), w.end());
    return w;
}

// alias-список через «|»: совпадение — слово текста начинается с alias (для русских окончаний)
static bool MatchAliases(const std::vector<std::string>& words, const std::string& text, const char* aliases)
{
    std::string a = aliases; size_t pos = 0;
    while(pos <= a.size())
    {
        size_t e = a.find('|', pos); if(e == std::string::npos) e = a.size();
        std::string al = a.substr(pos, e - pos);
        if(!al.empty())
        {
            if(al.find(' ') != std::string::npos) { if(text.find(al) != std::string::npos) return true; }
            else for(auto& w : words) if(w.compare(0, al.size(), al) == 0) return true;
        }
        pos = e + 1;
    }
    return false;
}

// ---------- оружие ----------
static const std::vector<Weapon> kWeapons = {
    { 1, 331, "brassknuckle", "Кастет", "кастет", 1 }, { 2, 333, "golfclub", "Клюшка", "клюшк", 1 }, { 3, 334, "nightstick", "Дубинка", "дубинк", 1 },
    { 4, 335, "knife", "Нож", "нож", 1 }, { 5, 336, "bat", "Бита", "бит", 1 }, { 6, 337, "shovel", "Лопата", "лопат", 1 },
    { 7, 338, "poolcue", "Кий", "кий", 1 }, { 8, 339, "katana", "Катана", "катан", 1 }, { 9, 341, "chainsaw", "Бензопила", "бензопил|пилу|пила", 1 },
    { 16, 342, "grenade", "Гранаты", "гранат", 10 }, { 17, 343, "teargas", "Слезоточивый газ", "газ", 10 }, { 18, 344, "molotov", "Коктейль Молотова", "молотов|коктейл", 10 },
    { 39, 363, "satchel", "Взрывчатка", "взрывчатк|си четыре|с4", 10 },
    { 22, 346, "pistol", "Пистолет", "пистолет|кольт|ствол", 200 }, { 23, 347, "silenced", "Пистолет с глушителем", "с глушител|глушителем|глушак", 200 },
    { 24, 348, "deagle", "Desert Eagle", "дигл|дезерт|игл|deagle", 100 },
    { 25, 349, "shotgun", "Дробовик", "дробовик|ружь", 100 }, { 26, 350, "sawnoff", "Обрез", "обрез", 100 }, { 27, 351, "spas", "Боевой дробовик", "боевой дроб|спас", 100 },
    { 28, 352, "uzi", "Узи", "узи|uzi", 500 }, { 32, 372, "tec9", "Tec-9", "тек|tec", 500 }, { 29, 353, "mp5", "MP5", "мп5|мп пять|mp5|эмпэ", 500 },
    { 30, 355, "ak47", "АК-47", "ак|калаш|ak", 500 }, { 31, 356, "m4", "M4", "м4|эм четыре|эмка|m4", 500 },
    { 33, 357, "rifle", "Винтовка", "винтовк", 100 }, { 34, 358, "sniper", "Снайперская винтовка", "снайпер", 100 },
    { 35, 359, "rpg", "РПГ", "рпг|ракетниц|базук|гранатомет", 30 }, { 36, 360, "heatseeker", "Самонаводящаяся ракетница", "самонавод|теплов", 30 },
    { 37, 361, "flamethrower", "Огнемёт", "огнемет", 500 }, { 38, 362, "minigun", "Миниган", "миниган|пулемет", 1000 },
    { 41, 365, "spraycan", "Баллончик", "баллончик", 500 }, { 42, 366, "extinguisher", "Огнетушитель", "огнетушител", 500 },
    { 43, 367, "camera", "Фотоаппарат", "фотоаппарат|камер", 36 }, { 46, 371, "parachute", "Парашют", "парашют", 1 },
};
const std::vector<Weapon>& Weapons() { return kWeapons; }
const Weapon* WeaponByType(int type) { for(auto& w : kWeapons) if(w.type == type) return &w; return nullptr; }
const Weapon* FindWeapon(const std::string& raw)
{
    std::string t = Lower(raw);
    for(auto& w : kWeapons) if(t == w.key) return &w;
    auto words = Words(t);
    // сначала более «длинные» названия, чтобы «боевой дробовик» не стал «дробовик», а «глушитель» — «пистолет»
    const Weapon* best = nullptr; size_t bestLen = 0;
    for(auto& w : kWeapons)
    {
        std::string a = w.aliases; size_t pos = 0;
        while(pos <= a.size())
        {
            size_t e = a.find('|', pos); if(e == std::string::npos) e = a.size();
            std::string al = a.substr(pos, e - pos);
            bool hit = false;
            if(al.find(' ') != std::string::npos) hit = t.find(al) != std::string::npos;
            else for(auto& x : words) if(x.compare(0, al.size(), al) == 0 && (al.size() > 4 || x.size() <= al.size() + 4)) hit = true;
            if(hit && al.size() > bestLen) { best = &w; bestLen = al.size(); }
            pos = e + 1;
        }
    }
    return best;
}

// ---------- машины ----------
static const char* kVehNames[212] = {
    "landstalker","bravura","buffalo","linerunner","perennial","sentinel","dumper","firetruck","trashmaster","stretch","manana","infernus","voodoo","pony","mule","cheetah","ambulance","leviathan","moonbeam","esperanto",
    "taxi","washington","bobcat","mrwhoopee","bfinjection","hunter","premier","enforcer","securicar","banshee","predator","bus","rhino","barracks","hotknife","trailer1","previon","coach","cabbie","stallion",
    "rumpo","rcbandit","romero","packer","monster","admiral","squalo","seasparrow","pizzaboy","tram","trailer2","turismo","speeder","reefer","tropic","flatbed","yankee","caddy","solair","topfun",
    "skimmer","pcj600","faggio","freeway","rcbaron","rcraider","glendale","oceanic","sanchez","sparrow","patriot","quad","coastguard","dinghy","hermes","sabre","rustler","zr350","walton","regina",
    "comet","bmx","burrito","camper","marquis","baggage","dozer","maverick","newschopper","rancher","fbirancher","virgo","greenwood","jetmax","hotring","sandking","blista","policemaverick","boxville","benson",
    "mesa","rcgoblin","hotringa","hotringb","bloodring","rancherlure","supergt","elegant","journey","bike","mountainbike","beagle","cropduster","stuntplane","tanker","roadtrain","nebula","majestic","buccaneer","shamal",
    "hydra","fcr900","nrg500","copbike","cementtruck","towtruck","fortune","cadrona","fbitruck","willard","forklift","tractor","combine","feltzer","remington","slamvan","blade","freight","streak","vortex",
    "vincent","bullet","clover","sadler","firetruckla","hustler","intruder","primo","cargobob","tampa","sunrise","merit","utilityvan","nevada","yosemite","windsor","monstera","monsterb","uranus","jester",
    "sultan","stratum","elegy","raindance","rctiger","flash","tahoma","savanna","bandito","freightflat","streakcarriage","kart","mower","dune","sweeper","broadway","tornado","at400","dft30","huntley",
    "stafford","bf400","newsvan","tug","petroltrailer","emperor","wayfarer","euros","hotdog","club","freightbox","trailer3","andromada","dodo","rccam","launch","policels","policesf","policelv","policeranger",
    "picador","swat","alpha","phoenix","glendaleshit","sadlershit","baggagea","baggageb","tugstairs","boxburg","farmtrailer","utilitytrailer",
};
static const struct { int model; const char* aliases; } kVehRu[] = {
    { 411, "инфернус" }, { 451, "туризмо" }, { 415, "чит|гепард" }, { 429, "банши" }, { 541, "буллет|пуля" }, { 560, "султан" }, { 562, "элеги" }, { 559, "джестер" },
    { 506, "супер джи|супер гт" }, { 480, "комет" }, { 603, "феникс" }, { 402, "буффало|бафало" }, { 477, "зет эр|zr" }, { 565, "флэш|флеш" }, { 558, "уранус" },
    { 522, "нрг|энэрджи|nrg" }, { 521, "фцр|fcr" }, { 461, "пи си джей|pcj" }, { 468, "санчез" }, { 463, "фривей|чоппер" }, { 462, "фаджио|мопед|скутер" }, { 471, "квадроцикл|квад" },
    { 481, "бмх|bmx" }, { 510, "горный велосипед|велосипед|велик" }, { 432, "танк|рино" }, { 425, "хантер|боевой вертолет" }, { 520, "гидр|истребител" },
    { 487, "маверик|вертолет|вертушк" }, { 497, "полицейский вертолет" }, { 417, "левиафан" }, { 548, "каргобоб" }, { 519, "шамал|бизнес джет" }, { 593, "додо" },
    { 513, "стант|трюков" }, { 476, "растлер" }, { 592, "андромад" }, { 577, "боинг|эйт четыреста|лайнер" }, { 511, "бигл" }, { 460, "скиммер|гидроплан" },
    { 596, "полицейск|копск|ментовск" }, { 599, "полицейский джип|рейнджер" }, { 523, "полицейский мотоцикл" }, { 427, "энфорсер|автозак" }, { 601, "спецназ|сват" },
    { 490, "фбр джип|фбр ранчер" }, { 528, "фбр грузовик" }, { 420, "такси" }, { 438, "кэбби" }, { 416, "скорая|скорую|амбуланс" }, { 407, "пожарн" },
    { 444, "монстр" }, { 409, "лимузин|стретч" }, { 494, "хотринг|гоночн" }, { 502, "хотринг а" }, { 568, "бандито|багги" }, { 424, "би эф|инжекшн" }, { 573, "дюна|дюн" },
    { 470, "патриот|хаммер" }, { 433, "барракс|армейский грузовик" }, { 567, "саванн" }, { 536, "блейд" }, { 534, "ремингтон" }, { 535, "слэмвэн|сламван" }, { 575, "бродвей" },
    { 412, "вуду" }, { 576, "торнадо" }, { 515, "тягач|фура|роудтрейн" }, { 403, "лайнраннер" }, { 514, "бензовоз" }, { 431, "автобус" }, { 437, "междугородн" },
    { 408, "мусоровоз" }, { 406, "самосвал|дампер" }, { 486, "бульдозер" }, { 532, "комбайн" }, { 531, "трактор" }, { 530, "погрузчик" }, { 572, "газонокосилк" }, { 571, "картинг|карт" },
    { 457, "гольф кар|кадди" }, { 539, "вортекс|воздушная подушка" }, { 452, "спидер|катер" }, { 446, "сквало" }, { 493, "джетмакс" }, { 473, "лодк|динги" }, { 454, "яхт|тропик" },
    { 448, "пиццабой|пицца" }, { 423, "мороженщик|мороженое" }, { 588, "хот дог" }, { 428, "инкассатор" }, { 525, "эвакуатор" }, { 405, "сентинел" }, { 426, "премьер" },
    { 445, "адмирал" }, { 507, "элегант" }, { 580, "стаффорд" }, { 579, "хантли" }, { 400, "ландсталкер" }, { 489, "ранчер" }, { 500, "меса" }, { 495, "сэндкинг|сандкинг" },
    { 475, "сабр|сейбр" }, { 439, "стэллион|сталлион" }, { 474, "гермес" }, { 549, "тампа" }, { 542, "кловер" }, { 518, "буканир|букканир" }, { 466, "глендейл" }, { 492, "гринвуд" },
    { 587, "еврос" }, { 602, "альфа" }, { 533, "фельтцер" }, { 555, "виндзор" }, { 434, "хотнайф|хот род" }, { 545, "хастлер" }, { 496, "блиста" }, { 589, "клаб" }, { 561, "стратум" },
};
const char* VehicleName(int model) { return model >= 400 && model <= 611 ? kVehNames[model - 400] : ""; }
static bool SpawnableVehicle(int m) { return m >= 400 && m <= 611 && m != 449 && m != 537 && m != 538 && m != 569 && m != 570 && m != 590; }
int FindVehicle(const std::string& raw)
{
    std::string t = Lower(raw);
    // номер модели
    { int v = atoi(t.c_str()); if(v >= 400 && v <= 611) return SpawnableVehicle(v) ? v : -1; }
    std::string compact; for(char c : t) if(c != ' ' && c != '-' && c != '_') compact += c;
    for(int i = 0; i < 212; ++i) if(compact == kVehNames[i]) return SpawnableVehicle(400 + i) ? 400 + i : -1;
    auto words = Words(t);
    int best = -1; size_t bestLen = 0;
    for(auto& v : kVehRu)
    {
        std::string a = v.aliases; size_t pos = 0;
        while(pos <= a.size())
        {
            size_t e = a.find('|', pos); if(e == std::string::npos) e = a.size();
            std::string al = a.substr(pos, e - pos);
            bool hit = false;
            if(al.find(' ') != std::string::npos) hit = t.find(al) != std::string::npos;
            else for(auto& x : words) if(x.compare(0, al.size(), al) == 0) hit = true;
            if(hit && al.size() > bestLen) { best = v.model; bestLen = al.size(); }
            pos = e + 1;
        }
    }
    if(best >= 0) return best;
    for(int i = 0; i < 212; ++i) for(auto& x : words) if(x.size() >= 4 && x == kVehNames[i]) return SpawnableVehicle(400 + i) ? 400 + i : -1;
    return -1;
}

// ---------- места ----------
static const struct { const char* title; const char* aliases; float x, y, z; } kPlaces[] = {
    { "Гроув-стрит", "гроув|grove|домой|дом сиджея|дом cj|дом карла|гантон", 2495.0f, -1668.0f, 13.3f },
    { "Аэропорт Лос-Сантоса", "аэропорт лос сантос|аэропорт лос|аэропорт лс|аэропорт ls", 1685.0f, -2330.0f, 13.5f },
    { "Центр Лос-Сантоса (Першинг-сквер)", "центр лос|першинг|мэри|центр лс", 1480.0f, -1680.0f, 14.0f },
    { "Полиция Лос-Сантоса", "полиция лос|участок лос|участок лс", 1540.0f, -1675.0f, 13.5f },
    { "Больница Лос-Сантоса", "больниц", 1178.0f, -1323.0f, 14.1f },
    { "Вайнвуд", "вайнвуд|vinewood", 1380.0f, -1090.0f, 27.0f },
    { "Пляж Санта-Мария", "пляж|санта мари|верона", 370.0f, -1870.0f, 7.8f },
    { "Глен-Парк", "глен парк", 1970.0f, -1200.0f, 25.0f },
    { "Айдлвуд", "айдлвуд", 2090.0f, -1730.0f, 13.5f },
    { "Ист-Лос-Сантос", "ист лос|восточный лос", 2350.0f, -1300.0f, 24.0f },
    { "Маяк Санта-Марии", "маяк", 154.0f, -1940.0f, 3.8f },
    { "Гора Чилиад", "чилиад|гора", -2320.0f, -1630.0f, 484.0f },
    { "Энджел-Пайн", "энджел пайн|эйнджел", -2150.0f, -2400.0f, 30.6f },
    { "Аэропорт Сан-Фиерро", "аэропорт сан фиерро|аэропорт сан|аэропорт сф|аэропорт sf", -1420.0f, -290.0f, 14.1f },
    { "Гараж в Доэрти (Сан-Фиерро)", "доэрти|гараж сан|гараж сф", -2030.0f, 165.0f, 28.8f },
    { "Центр Сан-Фиерро", "центр сан|сан фиерро|сан-фиерро|сф", -1990.0f, 880.0f, 45.3f },
    { "Чайна-таун", "чайна|китайский квартал", -2190.0f, 640.0f, 49.4f },
    { "Мост Гант", "мост гант|гант бридж|золотые ворота", -2680.0f, 1590.0f, 65.0f },
    { "Лас-Вентурас, Стрип", "стрип|лас вентурас|лас-вентурас|вегас|лв", 2030.0f, 1340.0f, 10.8f },
    { "Казино «Четыре дракона»", "четыре дракона|драконы|казино", 2030.0f, 1008.0f, 10.8f },
    { "Казино Калигулы", "калигул", 2180.0f, 1680.0f, 10.8f },
    { "Аэропорт Лас-Вентураса", "аэропорт лас вентурас|аэропорт лас|аэропорт лв|аэропорт lv", 1680.0f, 1450.0f, 10.8f },
    { "Полиция Лас-Вентураса", "полиция лас|участок лв", 2290.0f, 2430.0f, 10.8f },
    { "Заброшенный аэродром (Вердант-Медоус)", "аэродром|вердант|кладбище самолет", 400.0f, 2500.0f, 16.5f },
    { "Зона 69", "зона 69|зона шестьдесят девять|военная база|area 69", 213.0f, 1900.0f, 17.6f },
    { "Плотина Шермана", "плотин|дамб", -720.0f, 2030.0f, 60.0f },
    { "Лас-Барранкас", "барранкас", -780.0f, 1550.0f, 27.0f },
    { "Форт-Карсон", "форт карсон", -210.0f, 1100.0f, 19.7f },
    { "Бэйсайд", "бэйсайд|бейсайд", -2450.0f, 2310.0f, 5.0f },
    { "Паломино-Крик", "паломино", 2270.0f, 30.0f, 26.5f },
    { "Блуберри", "блуберри|блюберри", 240.0f, -170.0f, 1.6f },
    { "Диллимор", "диллимор", 660.0f, -570.0f, 16.3f },
    { "Монтгомери", "монтгомери", 1300.0f, 300.0f, 19.5f },
    { "Карьер", "карьер", 690.0f, 880.0f, -40.0f },
    { "Стадион Лос-Сантоса", "стадион", 2700.0f, -1700.0f, 12.0f },
};
bool FindPlace(const std::string& raw, float& x, float& y, float& z, std::string& title)
{
    std::string t = Lower(raw);
    for(char& c : t) if(c == '-') c = ' ';
    auto words = Words(t);
    size_t bestLen = 0; int best = -1;
    for(size_t i = 0; i < sizeof(kPlaces) / sizeof(kPlaces[0]); ++i)
    {
        std::string a = kPlaces[i].aliases; for(char& c : a) if(c == '-') c = ' ';
        size_t pos = 0;
        while(pos <= a.size())
        {
            size_t e = a.find('|', pos); if(e == std::string::npos) e = a.size();
            std::string al = a.substr(pos, e - pos);
            bool hit = false;
            if(al.find(' ') != std::string::npos) hit = t.find(al) != std::string::npos;
            else for(auto& w : words) if(w.compare(0, al.size(), al) == 0 && (al.size() > 4 || w.size() == al.size())) hit = true;
            if(hit && al.size() > bestLen) { best = (int)i; bestLen = al.size(); }
            pos = e + 1;
        }
    }
    if(best < 0) return false;
    x = kPlaces[best].x; y = kPlaces[best].y; z = kPlaces[best].z; title = kPlaces[best].title;
    return true;
}
std::string PlaceList()
{
    std::string s;
    for(auto& p : kPlaces) { if(!s.empty()) s += ", "; s += p.title; }
    return s;
}

// ---------- сетка ----------
bool ParseGrid(const std::string& raw, float& x, float& y, std::string& norm)
{
    std::string t = Lower(raw);
    // кириллические буквы, похожие на латинские: а в с е н к (и «эй би си» произношением не разбираем)
    static const struct { const char* cyr; char lat; } map[] = { { "а", 'a' }, { "в", 'b' }, { "с", 'c' }, { "д", 'd' }, { "е", 'e' }, { "ф", 'f' },
        { "г", 'g' }, { "н", 'h' }, { "и", 'i' }, { "к", 'k' }, { "л", 'l' }, { "ж", 'j' } };
    for(size_t i = 0; i < t.size(); ++i)
    {
        char col = 0; size_t adv = 0;
        unsigned char c = t[i];
        if(c >= 'a' && c <= 'l') { col = (char)c; adv = 1; }
        else if(c >= 0xD0) { for(auto& m : map) if(t.compare(i, 2, m.cyr) == 0) { col = m.lat; adv = 2; } }
        if(!col) { if(c >= 0x80) ++i; continue; }
        // перед буквой — не буква (начало слова)
        if(i > 0) { unsigned char p = t[i - 1]; if(isalnum(p) || p >= 0x80) { if(adv == 2) ++i; continue; } }
        size_t j = i + adv;
        while(j < t.size() && (t[j] == ' ' || t[j] == '-')) ++j;
        if(j >= t.size() || !isdigit((unsigned char)t[j])) { if(adv == 2) ++i; continue; }
        int row = 0; size_t k = j;
        while(k < t.size() && isdigit((unsigned char)t[k]) && k - j < 2) { row = row * 10 + (t[k] - '0'); ++k; }
        if(row < 1 || row > 12) { if(adv == 2) ++i; continue; }
        if(k < t.size() && isdigit((unsigned char)t[k])) { if(adv == 2) ++i; continue; }
        int sub = 0;
        size_t m = k;
        while(m < t.size() && t[m] == ' ') ++m;
        if(m < t.size() && (t[m] == '.' || t[m] == ',')) { ++m; while(m < t.size() && t[m] == ' ') ++m; if(m < t.size() && t[m] >= '1' && t[m] <= '9') sub = t[m] - '0'; }
        int ci = col - 'a';
        float x0 = -3000.0f + ci * 500.0f, yTop = 3000.0f - (row - 1) * 500.0f;
        if(sub)
        {
            int sx = (sub - 1) % 3, sy = (sub - 1) / 3;
            x = x0 + (sx + 0.5f) * (500.0f / 3); y = yTop - (sy + 0.5f) * (500.0f / 3);
        }
        else { x = x0 + 250.0f; y = yTop - 250.0f; }
        char buf[16]; if(sub) snprintf(buf, sizeof(buf), "%c%d.%d", toupper(col), row, sub); else snprintf(buf, sizeof(buf), "%c%d", toupper(col), row);
        norm = buf;
        return true;
    }
    return false;
}

std::string GridOf(float x, float y)
{
    if(x < -3000) x = -3000; if(x > 2999) x = 2999; if(y > 3000) y = 3000; if(y < -2999) y = -2999;
    int ci = (int)((x + 3000.0f) / 500.0f), row = (int)((3000.0f - y) / 500.0f);
    if(ci > 11) ci = 11; if(row > 11) row = 11;
    float fx = (x + 3000.0f) - ci * 500.0f, fy = (3000.0f - y) - row * 500.0f;
    int sx = std::min(2, (int)(fx / (500.0f / 3))), sy = std::min(2, (int)(fy / (500.0f / 3)));
    char buf[16]; snprintf(buf, sizeof(buf), "%c%d.%d", 'A' + ci, row + 1, sy * 3 + sx + 1);
    return buf;
}

// ---------- погода ----------
int FindWeather(const std::string& raw)
{
    std::string t = Lower(raw);
    if(HasAny(t, { "песчан", "буря в пустын" })) return 19;
    if(HasAny(t, { "гроз", "шторм", "ливен", "ливн" })) return 16;
    if(HasAny(t, { "дожд" })) return 8;
    if(HasAny(t, { "туман" })) return 9;
    if(HasAny(t, { "пасмурн", "облачн", "тучи", "хмур" })) return 4;
    if(HasAny(t, { "ясн", "солн", "хорошую погод", "чистое небо" })) return 1;
    return -1;
}

// ---------- числа ----------
bool ParseNumber(const std::string& raw, long long& out)
{
    static const std::map<std::string, int> small = {
        { "ноль", 0 }, { "один", 1 }, { "одна", 1 }, { "одну", 1 }, { "одного", 1 }, { "два", 2 }, { "две", 2 }, { "двух", 2 }, { "три", 3 }, { "трех", 3 }, { "четыре", 4 }, { "пять", 5 }, { "шесть", 6 }, { "семь", 7 },
        { "восемь", 8 }, { "девять", 9 }, { "десять", 10 }, { "одиннадцать", 11 }, { "двенадцать", 12 }, { "тринадцать", 13 }, { "четырнадцать", 14 },
        { "пятнадцать", 15 }, { "шестнадцать", 16 }, { "семнадцать", 17 }, { "восемнадцать", 18 }, { "девятнадцать", 19 }, { "двадцать", 20 }, { "тридцать", 30 },
        { "сорок", 40 }, { "пятьдесят", 50 }, { "шестьдесят", 60 }, { "семьдесят", 70 }, { "восемьдесят", 80 }, { "девяносто", 90 }, { "сто", 100 }, { "сотню", 100 }, { "сотня", 100 }, { "двести", 200 },
        { "триста", 300 }, { "четыреста", 400 }, { "пятьсот", 500 }, { "шестьсот", 600 }, { "семьсот", 700 }, { "восемьсот", 800 }, { "девятьсот", 900 }, { "пол", 0 } };
    auto words = Words(Lower(raw));
    long long total = 0, cur = 0; bool any = false, started = false;
    for(size_t i = 0; i < words.size(); ++i)
    {
        const std::string& w = words[i];
        bool digits = !w.empty() && isdigit((unsigned char)w[0]);
        long long mult = 0;
        if(w.compare(0, 10, "тысяч") == 0 || w == "к" || w == "косар" || w.compare(0, 10, "косар") == 0 || w == "штук" || w == "штуки") mult = 1000;
        else if(w.compare(0, 14, "миллион") == 0 || w.compare(0, 6, "лям") == 0) mult = 1000000;
        if(digits)
        {
            if(started && !any) break;
            long long v = 0; size_t k = 0; while(k < w.size() && isdigit((unsigned char)w[k])) { v = v * 10 + (w[k] - '0'); ++k; }
            std::string suffix = w.substr(k);
            cur += v; any = true; started = true;
            if(suffix == "к" || suffix == "k") { total += cur * 1000; cur = 0; }
            continue;
        }
        if(mult) { if(!started) { cur = 1; } total += (cur ? cur : 1) * mult; cur = 0; any = true; started = true; continue; }
        auto it = small.find(w);
        if(it != small.end() && w != "пол") { cur += it->second; any = true; started = true; continue; }
        if(started) break;       // число закончилось
    }
    total += cur;
    if(!any) return false;
    out = total;
    return true;
}

// ---------- статы ----------
int FindStat(const std::string& raw, bool& isFloat, int& maxValue, std::string& title)
{
    std::string t = Lower(raw);
    struct S { int id; bool f; int max; const char* title; const char* aliases; };
    static const S stats[] = {
        { 23, true, 1000, "Мышцы", "мышц|качк|накача|сил" }, { 21, true, 1000, "Жир", "жир|толст" }, { 22, true, 1000, "Выносливость", "вынослив|стамин" },
        { 225, false, 1000, "Объём лёгких", "легки|дыхани" }, { 160, false, 1000, "Вождение", "вожден|водит" }, { 223, false, 1000, "Пилотирование", "пилот|полет|летат" },
        { 229, false, 1000, "Мотоцикл", "мотоцикл|байк" }, { 230, false, 1000, "Велосипед", "велосипед" }, { 25, true, 1000, "Привлекательность", "привлекат|секс" },
        { 24, true, 1000, "Макс. здоровье", "макс здоров|максимальное здоров" }, { 68, true, 1000, "Уважение", "уважен|респект" },
        { 69, true, 1000, "Навык: пистолет", "навык пистолет" }, { 71, true, 1000, "Навык: Desert Eagle", "навык дигл" }, { 72, true, 1000, "Навык: дробовик", "навык дробовик" },
        { 75, true, 1000, "Навык: узи", "навык узи" }, { 76, true, 1000, "Навык: MP5", "навык мп5" }, { 77, true, 1000, "Навык: АК-47", "навык ак|навык калаш" },
        { 78, true, 1000, "Навык: M4", "навык м4" }, { 79, true, 1000, "Навык: снайперка", "навык снайпер" },
    };
    auto words = Words(t);
    for(auto& s : stats) if(MatchAliases(words, t, s.aliases)) { isFloat = s.f; maxValue = s.max; title = s.title; return s.id; }
    return -1;
}

// ---------- разбор фраз без ИИ ----------
static Cmd Make(const char* tool) { Cmd c; c.tool = tool; c.in = json::Value::Object(); return c; }

std::vector<Cmd> ParseOffline(const std::string& raw)
{
    std::vector<Cmd> out;
    std::string t = Lower(raw);
    auto words = Words(t);
    long long num = 0; bool hasNum = ParseNumber(t, num);
    bool neg = HasAny(t, { "выключи", "отключи", "убери", "сними", "без ", "не надо", "хватит" });

    if(HasAny(t, { "отмени", "верни как было", "верни все как было", "отмена" })) { out.push_back(Make("undo")); return out; }
    if(HasAny(t, { "где я", "где мы", "мои координат", "какой квадрат" })) { out.push_back(Make("info")); return out; }
    if(HasAny(t, { "список мест", "какие места", "мои места" })) { Cmd c = Make("place"); c.in["action"] = "list"; out.push_back(c); return out; }
    {   // «запомни это место как база»
        size_t p = t.find("запомни");
        if(p != std::string::npos)
        {
            size_t k = t.find(" как ", p);
            std::string name = k != std::string::npos ? t.substr(k + 8) : "";
            while(!name.empty() && (name.back() == '.' || name.back() == ' ' || name.back() == '!')) name.pop_back();
            if(name.empty()) name = "место";
            Cmd c = Make("place"); c.in["action"] = "save"; c.in["name"] = name; out.push_back(c); return out;
        }
    }

    // перенос
    if(HasAny(t, { "перенес", "перемест", "телепорт", "отправ", "закинь", "хочу в ", "хочу на ", "в квадрат", "на метку", "на базу", "домой", "верни меня", "доставь" }))
    {
        Cmd c = Make("teleport");
        float x, y, z; std::string title, norm;
        if(HasAny(t, { "метк", "маркер", "точку на карте" })) c.in["place"] = "waypoint";
        else if(HasAny(t, { "назад", "обратно", "где был" })) c.in["place"] = "back";
        else if(HasAny(t, { "к другу", "к напарнику", "к хосту" })) c.in["place"] = "friend";
        else if(ParseGrid(t, x, y, norm)) c.in["place"] = norm;
        else if(FindPlace(t, x, y, z, title)) c.in["place"] = title;
        else
        {   // своё место: слово после «в/на»
            std::string name;
            for(size_t i = 0; i + 1 < words.size(); ++i) if(words[i] == "в" || words[i] == "на" || words[i] == "к") { name = words[i + 1]; if(i + 2 < words.size()) name += " " + words[i + 2]; }
            if(name.empty()) return out;
            c.in["place"] = name;
        }
        out.push_back(c); return out;
    }

    // розыск
    if(HasAny(t, { "розыск", "звезд", "звёзд" }) || (HasAny(t, { "копы", "копов", "полици", "менты", "ментов" }) && HasAny(t, { "не трог", "отстал", "игнор", "забыл", "убери", "сними" })))
    {
        Cmd c = Make("wanted");
        if(HasAny(t, { "не трог", "никогда", "игнор", "отстал", "чтоб не ловил", "забыл" })) { c.in["immune"] = !HasAny(t, { "снова лов", "пусть лов" }); c.in["level"] = 0; }
        else if(hasNum && num >= 0 && num <= 6 && !HasAny(t, { "сними", "убери", "сбрось", "очисти" })) c.in["level"] = (int)num;
        else if(HasAny(t, { "сними", "убери", "сбрось", "очисти", "без розыск", "ноль" })) c.in["level"] = 0;
        else if(HasAny(t, { "максим", "полный", "все звезд" })) c.in["level"] = 6;
        else if(HasAny(t, { "добав", "дай", "повысь", "больше" })) c.in["delta"] = 1;
        else c.in["level"] = 0;
        out.push_back(c);
    }

    // оружие
    if(HasAny(t, { "забери оруж", "убери оруж", "отними оруж", "без оруж", "разоруж" })) { out.push_back(Make("take_weapons")); return out; }
    if(HasAny(t, { "арсенал", "все оружие", "всё оружие", "полный набор", "вооружи" }))
    {
        for(const char* k : { "deagle", "spas", "mp5", "m4", "sniper", "rpg", "grenade", "katana" }) { Cmd c = Make("give_weapon"); c.in["weapon"] = k; c.in["ammo"] = 0; out.push_back(c); }
        return out;
    }
    if(const Weapon* w = FindWeapon(t))
    {
        bool vehicleWord = FindVehicle(t) >= 0 && HasAny(t, { "машин", "тачк", "вертолет", "мотоцикл" });
        if(!vehicleWord)
        {
            Cmd c = Make("give_weapon"); c.in["weapon"] = w->key;
            c.in["ammo"] = hasNum && num > 0 && num <= 99999 && HasAny(t, { "патрон", "штук", "обойм", "заряд", "ракет", "гранат" }) ? (int)num : 0;
            if(HasAny(t, { "другу", "напарнику" })) c.in["to"] = "friend";
            out.push_back(c);
        }
    }
    if(HasAny(t, { "бесконечн" }) && HasAny(t, { "патрон" })) { Cmd c = Make("player"); c.in["infinite_ammo"] = !neg; out.push_back(c); }

    // игрок
    {
        Cmd c = Make("player"); bool any = false;
        if(HasAny(t, { "бессмерт", "неуязвим", "режим бога" })) { c.in["god"] = !HasAny(t, { "выключи", "отключи", "убери", "сними" }); any = true; }
        if(HasAny(t, { "здоров", "вылечи", "полечи", "жизн", "аптечк" })) { c.in["health"] = 100; any = true; }
        if(HasAny(t, { "брон" })) { c.in["armour"] = 100; any = true; }
        if(HasAny(t, { "деньг", "денег", "бабл", "бабок", "баксов", "доллар", "кэш" }) && hasNum) { c.in["money"] = (double)(HasAny(t, { "забери", "отними", "сними" }) ? -num : num); any = true; }
        if(HasAny(t, { "бесконечный бег", "не уставать", "не устава" })) { c.in["never_tired"] = !neg; any = true; }
        if(any) out.push_back(c);
    }
    {   // статы
        bool f; int mx; std::string title;
        if(HasAny(t, { "прокач", "навык", "мышц", "выносливост", "максимум" }))
        {
            int id = FindStat(t, f, mx, title);
            if(id >= 0) { Cmd c = Make("stat"); c.in["name"] = title; c.in["value"] = hasNum && num <= mx ? (int)num : mx; out.push_back(c); }
        }
    }

    // машины
    if(HasAny(t, { "почини", "отремонт", "ремонт", "как новая" })) { Cmd c = Make("vehicle"); c.in["action"] = "repair"; out.push_back(c); }
    else if(HasAny(t, { "переверни", "на колеса", "на колёса" })) { Cmd c = Make("vehicle"); c.in["action"] = "flip"; out.push_back(c); }
    else if(HasAny(t, { "нитро", "закись" })) { Cmd c = Make("vehicle"); c.in["action"] = "nitro"; out.push_back(c); }
    else if(HasAny(t, { "убери машин", "удали машин", "убери тачк" })) { Cmd c = Make("vehicle"); c.in["action"] = "delete"; out.push_back(c); }
    else if(HasAny(t, { "джетпак", "реактивный ранец" })) { Cmd c = Make("vehicle"); c.in["action"] = "jetpack"; out.push_back(c); }
    else
    {
        int m = FindVehicle(t);
        bool weaponTaken = false; for(auto& c : out) if(c.tool == "give_weapon") weaponTaken = true;
        if(m >= 0 && !weaponTaken) { Cmd c = Make("vehicle"); c.in["action"] = "spawn"; c.in["model"] = m; out.push_back(c); }
    }

    // мир
    {
        Cmd c = Make("world"); bool any = false;
        int wt = FindWeather(t);
        if(wt >= 0 && HasAny(t, { "погод", "сделай", "включи", "пусть", "хочу", "давай", "дожд", "туман", "солн", "гроз" })) { c.in["weather"] = wt; any = true; }
        if(HasAny(t, { "ночь", "полноч" })) { c.in["hour"] = 0; any = true; }
        else if(HasAny(t, { "утро", "рассвет" })) { c.in["hour"] = 7; any = true; }
        else if(HasAny(t, { "полдень", "день" }) && HasAny(t, { "сделай", "включи", "поставь", "пусть", "хочу" })) { c.in["hour"] = 12; any = true; }
        else if(HasAny(t, { "вечер", "закат" })) { c.in["hour"] = 19; any = true; }
        else if(HasAny(t, { "время", "часов", "час " }) && hasNum && num >= 0 && num <= 23) { c.in["hour"] = (int)num; any = true; }
        if(HasAny(t, { "замедли", "слоу", "медленн" })) { c.in["time_scale"] = 0.4; any = true; }
        else if(HasAny(t, { "ускорь время", "быстрее время" })) { c.in["time_scale"] = 2.0; any = true; }
        else if(HasAny(t, { "нормальн" }) && HasAny(t, { "скорост", "время" })) { c.in["time_scale"] = 1.0; any = true; }
        if(HasAny(t, { "убери пробки", "без машин", "убери трафик", "пустые дороги" })) { c.in["traffic"] = 0.0; any = true; }
        else if(HasAny(t, { "верни машины", "верни трафик", "обычные дороги" })) { c.in["traffic"] = 1.0; any = true; }
        if(HasAny(t, { "убери людей", "без людей", "пустой город" })) { c.in["peds"] = 0.0; any = true; }
        else if(HasAny(t, { "верни людей" })) { c.in["peds"] = 1.0; any = true; }
        if(HasAny(t, { "очисти район", "убери всех", "тишин", "зачисти" })) { c.in["clear_area"] = true; any = true; }
        if(any) out.push_back(c);
    }
    return out;
}

// ---------- справочник команд ----------
namespace
{
    struct Param { std::string name, type; bool out = false; };
    struct LibCmd { uint16_t id = 0; std::string name, desc, cls, member, search; std::vector<Param> params; bool cond = false, bad = false; };
    std::vector<LibCmd> g_lib;
    std::map<std::string, size_t> g_byName;

    bool RiskyName(const std::string& n)
    {
        static const char* risky[] = { "MISSION", "SAVE", "FAIL", "KILL", "EXPLODE", "DELETE", "REMOVE", "DESTROY", "RESTART", "GANG_WAR", "ZONE", "CUTSCENE", "CREDITS" };
        for(const char* r : risky) if(n.find(r) != std::string::npos) return true;
        return false;
    }
    bool ForbiddenName(const std::string& n)
    {
        static const char* bad[] = { "GOTO", "GOSUB", "RETURN", "WAIT", "SCRIPT", "SWITCH_", "CASE", "LAUNCH_MISSION", "LOAD_AND_LAUNCH", "IF", "ANDOR", "SKIP_CUTSCENE", "ELSE", "REPEAT", "WHILE", "MENU", "WIDGET", "TOUCH" };
        for(const char* r : bad) { if(n == r) return true; if(strlen(r) > 3 && n.find(r) != std::string::npos) return true; }
        return false;
    }
}

bool LoadLibrary(const std::string& path, std::string& err)
{
    FILE* f = fopen(path.c_str(), "rb");
    if(!f) { err = "справочник команд не скачан"; return false; }
    std::string s; char buf[1 << 16]; size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    json::Value root; std::string perr;
    if(!json::Parse(s, root, &perr)) { err = "справочник повреждён: " + perr; return false; }
    g_lib.clear(); g_byName.clear();
    for(auto& ext : root["extensions"].arr())
    {
        if(ext["name"].str() != "default") continue;
        for(auto& c : ext["commands"].arr())
        {
            LibCmd k;
            k.id = (uint16_t)strtol(c["id"].str().c_str(), nullptr, 16);
            k.name = c["name"].str(); k.desc = c["short_desc"].str(); k.cls = c["class"].str(); k.member = c["member"].str();
            const json::Value& at = c["attrs"];
            k.cond = at["is_condition"].b();
            k.bad = at["is_unsupported"].b() || at["is_nop"].b() || at["is_branch"].b() || at["is_segment"].b() || at["is_variadic"].b() || ForbiddenName(k.name);
            for(auto& p : c["input"].arr())  { Param q; q.name = p["name"].str(); q.type = p["type"].str(); if(q.type == "arguments" || p["source"].str().compare(0, 3, "var") == 0) k.bad = true; k.params.push_back(q); }   // команды над переменными сценария нам не нужны и опасны
            for(auto& p : c["output"].arr()) { Param q; q.name = p["name"].str(); q.type = p["type"].str(); q.out = true; k.params.push_back(q); }
            if(k.name.empty() || k.bad) continue;
            k.search = Lower(k.name + " " + k.cls + " " + k.member + " " + k.desc);
            for(char& ch : k.search) if(ch == '_') ch = ' ';
            g_byName[k.name] = g_lib.size();
            g_lib.push_back(std::move(k));
        }
    }
    if(g_lib.size() < 500) { err = "в справочнике слишком мало команд"; g_lib.clear(); g_byName.clear(); return false; }
    return true;
}
bool LibraryLoaded() { return !g_lib.empty(); }
int  LibrarySize() { return (int)g_lib.size(); }

std::string Search(const std::string& query, int max)
{
    if(g_lib.empty()) return "справочник команд не загружен";
    auto qs = Words(Lower(query));
    std::vector<std::pair<int, size_t>> scored;
    for(size_t i = 0; i < g_lib.size(); ++i)
    {
        const LibCmd& c = g_lib[i]; int score = 0;
        std::string lname = Lower(c.name);
        for(auto& q : qs)
        {
            if(q.size() < 2) continue;
            if(lname.find(q) != std::string::npos) score += 3;
            else if(c.search.find(q) != std::string::npos) score += 1;
        }
        if(score) scored.push_back({ -score * 1000 + (int)c.name.size(), i });
    }
    std::sort(scored.begin(), scored.end());
    std::string out;
    for(size_t k = 0; k < scored.size() && (int)k < max; ++k)
    {
        const LibCmd& c = g_lib[scored[k].second];
        std::string line = c.name + "(";
        bool first = true;
        for(auto& p : c.params) if(!p.out) { if(!first) line += ", "; first = false; line += p.name + ":" + p.type; }
        line += ")";
        bool anyOut = false;
        for(auto& p : c.params) if(p.out) { line += anyOut ? ", " : " -> "; anyOut = true; line += p.name + ":" + p.type; }
        if(c.cond) line += " [условие: вернёт flag]";
        if(!c.desc.empty()) line += " — " + c.desc;
        out += line + "\n";
    }
    if(out.empty()) out = "ничего не найдено — попробуй другие английские слова (например: car, char, weapon, weather, explosion, fire, camera, stat)";
    return out;
}

static bool IsFloatType(const std::string& t) { return t == "float"; }
static bool IsStringType(const std::string& t) { return t == "string" || t == "gxt_key" || t == "zone_key" || t == "label" || t == "script_id"; }

bool Compile(const std::string& name, const json::Value& args, const Context& ctx, Compiled& out, std::string& err)
{
    std::string up; for(char c : name) up += (char)toupper((unsigned char)c);
    auto it = g_byName.find(up);
    if(it == g_byName.end()) { err = "нет команды " + up + " (найди через find_commands)"; return false; }
    const LibCmd& c = g_lib[it->second];
    out = Compiled(); out.opcode = c.id; out.condition = c.cond; out.name = c.name; out.risky = RiskyName(c.name);
    size_t ai = 0, nin = 0;
    for(auto& p : c.params) if(!p.out) ++nin;
    if(args.size() != nin) { char b[160]; snprintf(b, sizeof(b), "%s: нужно %d входных параметров, дано %d", c.name.c_str(), (int)nin, (int)args.size()); err = b; return false; }
    for(auto& p : c.params)
    {
        if(p.out) { out.args.push_back(IsFloatType(p.type) ? script::Arg::OF() : script::Arg::OI()); out.outNames.push_back(p.name + (IsFloatType(p.type) ? ":f" : ":i")); continue; }
        const json::Value& a = args[ai++];
        double num = 0; bool isNum = false; std::string str;
        if(a.isNumber() || a.isBool()) { num = a.num(); isNum = true; }
        else if(a.isString())
        {
            str = a.str();
            if(str == "$me" || str == "$player_char") { num = ctx.playerChar; isNum = true; }
            else if(str == "$player") { num = 0; isNum = true; }
            else if(str == "$car") { if(ctx.car < 0) { err = "игрок не в машине ($car)"; return false; } num = ctx.car; isNum = true; }
            else if(str.size() > 1 && str[0] == '$' && isdigit((unsigned char)str[1]))
            {
                int idx = atoi(str.c_str() + 1);
                if(idx < 1 || idx > (int)ctx.prev.size()) { err = "нет результата " + str; return false; }
                num = ctx.prev[idx - 1]; isNum = true;
            }
        }
        else { err = c.name + ": параметр " + p.name + " не задан"; return false; }
        if(IsStringType(p.type))
        {
            if(isNum && str.empty()) { err = c.name + ": параметр " + p.name + " должен быть строкой"; return false; }
            // у игры под такие параметры буферы на 8 (ключ текста, зона) или 16 байт — длиннее нельзя
            size_t cap = p.type == "string" ? 15 : 7;
            if(str.size() > cap) { err = c.name + ": параметр " + p.name + " — не длиннее " + std::to_string(cap) + " латинских символов (это ключ, а не текст)"; return false; }
            out.args.push_back(script::Arg::S(str));
        }
        else if(IsFloatType(p.type))
        {
            if(!isNum) { err = c.name + ": параметр " + p.name + " должен быть числом"; return false; }
            out.args.push_back(script::Arg::F((float)num));
        }
        else
        {
            if(!isNum)
            {   // модели и оружие можно назвать словом
                int v = -1;
                if(p.type == "model_vehicle" || p.type == "model_any") v = FindVehicle(str);
                if(v < 0 && (p.type == "WeaponType")) { if(const Weapon* w = FindWeapon(str)) v = w->type; }
                if(v < 0 && p.type == "model_any") { if(const Weapon* w = FindWeapon(str)) v = w->model; }
                if(v < 0) { err = c.name + ": параметр " + p.name + " (" + p.type + ") должен быть числом"; return false; }
                num = v;
            }
            out.args.push_back(script::Arg::I((int)llround(num)));
        }
    }
    return true;
}

} // namespace jbrain
