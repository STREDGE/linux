// Готовит встроенные голоса пакета: образец wav или готовое состояние .xvst → сжатый .xvst
//   make_voices <папка модели> <libonnxruntime.so> <папка вывода> ключ=файл.wav|файл.xvst ...
#include <stdio.h>
#include <string>
#include <vector>
#include "speech/xvibe.h"
#include "wav.h"
int main(int argc, char** argv)
{
    std::string err; xvibe::Options o; o.threads = 4; o.voiceCache = 2;
    if(!xvibe::Load(argv[1], argv[2], o, err)) { printf("load: %s\n", err.c_str()); return 1; }
    for(int i = 4; i < argc; ++i)
    {
        std::string a = argv[i]; size_t eq = a.find('=');
        std::string key = a.substr(0, eq), src = a.substr(eq + 1);
        bool ok;
        if(src.size() > 5 && src.substr(src.size() - 5) == ".xvst") ok = xvibe::LoadVoiceState(key, src, err);
        else { std::vector<float> r; int sr; ok = ReadWav(src.c_str(), r, sr) && xvibe::PrepareVoice(key, r.data(), (int)r.size(), sr, err); }
        if(ok) ok = xvibe::SaveVoiceState(key, std::string(argv[3]) + "/" + key + ".xvst", err);
        printf("%s: %s\n", key.c_str(), ok ? "ok" : err.c_str());
        xvibe::DropVoice(key);
        if(!ok) return 1;
    }
    return 0;
}
