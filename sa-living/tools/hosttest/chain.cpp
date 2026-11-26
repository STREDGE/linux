// Проверка цепочки речи на компьютере: текст → голос (xVibe) → распознавание (GigaAM) → сравнение слов.
//   chain <base> <lib> [папка для wav]
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <set>
#include "speech/speech.h"
#include "wav.h"

static double Now() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static std::vector<std::string> Words(const std::string& s)
{
    std::vector<std::string> w; std::string cur;
    for(size_t i = 0; i < s.size();)
    {
        unsigned char c = s[i];
        if(c >= 0xD0 && i + 1 < s.size())
        {   // кириллица: к нижнему регистру, ё → е
            unsigned cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            if(cp >= 0x410 && cp <= 0x42F) cp += 0x20;
            if(cp == 0x401 || cp == 0x451) cp = 0x435;
            if(cp >= 0x430 && cp <= 0x44F) { cur += (char)(0xC0 | (cp >> 6)); cur += (char)(0x80 | (cp & 0x3F)); }
            i += 2; continue;
        }
        if(isalnum(c)) cur += (char)tolower(c);
        else if(!cur.empty()) { w.push_back(cur); cur.clear(); }
        ++i;
    }
    if(!cur.empty()) w.push_back(cur);
    return w;
}
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    speech::Init(argv[1], argv[2]);
    std::string err;
    double t0 = Now();
    if(!speech::TestLoad(err)) { printf("load: %s\n", err.c_str()); return 1; }
    printf("движки загружены за %.1f с\n", Now() - t0);
    const char* phrases[] = {
        "Эй, ты. Д+а, ты. Чег+о уст+авился?",
        "Пятьс+от б+аксов, и я заб+уду, что в+идел теб+я здесь.",
        "Полиц+ия! Р+уки, чтоб я в+идел! Не дв+игаться!",
        "Гот+ово, сэр. Миниг+ан и 500 патр+онов.",
        "Р+озыск снят. Что-ниб+удь ещё?",
        "Сл+ушай, я ничег+о не в+идела, +ясно? Ост+авь мен+я в пок+ое.",
    };
    int totalW = 0, hitW = 0; double totalGen = 0, totalAudio = 0;
    for(const speech::Voice& v : speech::Voices())
    {
        int vw = 0, vh = 0;
        for(size_t k = 0; k < sizeof(phrases) / sizeof(phrases[0]); ++k)
        {
            std::vector<float> pcm; double t = Now();
            if(!speech::TestSynthesize(v.key, phrases[k], pcm, err)) { printf("synth: %s\n", err.c_str()); return 1; }
            double gen = Now() - t; totalGen += gen; totalAudio += pcm.size() / 24000.0;
            std::vector<float> p16(pcm.size() * 2 / 3);
            for(size_t i = 0; i < p16.size(); ++i) { double x = i * 1.5; size_t a = (size_t)x; float f = (float)(x - a); p16[i] = a + 1 < pcm.size() ? pcm[a] * (1 - f) + pcm[a + 1] * f : pcm[a]; }
            std::string text;
            speech::TestRecognize(p16, text, err);
            std::string plain; for(const char* c = phrases[k]; *c; ++c) if(*c != '+') plain += *c;
            auto want = Words(speech::Normalize(plain)), got = Words(text);
            std::multiset<std::string> g(got.begin(), got.end()); int hit = 0;
            for(auto& w : want) { auto it = g.find(w); if(it != g.end()) { ++hit; g.erase(it); } }
            vw += (int)want.size(); vh += hit;
            printf("  [%s] %.1f с звука за %.1f с | %d/%d | «%s»\n", v.key.c_str(), pcm.size() / 24000.0, gen, hit, (int)want.size(), text.c_str());
            if(argc > 3) { char p[512]; snprintf(p, sizeof(p), "%s/%s_%d.wav", argv[3], v.key.c_str(), (int)k); WriteWav(p, pcm, 24000); }
        }
        printf("%s (%s): слов распознано %d%%\n", v.key.c_str(), v.title.c_str(), vw ? vh * 100 / vw : 0);
        totalW += vw; hitW += vh;
    }
    printf("ИТОГО: %d%% слов, скорость x%.2f (время синтеза / длина звука)\n", totalW ? hitW * 100 / totalW : 0, totalAudio > 0 ? totalGen / totalAudio : 0);
    fflush(stdout);
    _exit(0);      // фоновые потоки речи ждут задания — обычный выход их не дождётся
}
