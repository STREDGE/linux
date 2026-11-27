#include "coop/modsync.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include "net/protocol.h"
#include "trace/ctrace.h"

namespace modsync
{
namespace
{
    struct Entry { char kind; std::string name; long size; std::string abs; };   // kind: 'a' AML, 'c' CLEO

    net::Session* g_session = nullptr;
    std::mutex g_mx;                          // LobbyLines() зовёт меню, остальное — игровой поток
    std::vector<Line> g_lines;
    std::string g_notice;                     // одно сообщение для ленты меню

    std::string g_modsDir, g_cleoDir, g_offDir;
    bool g_pathsDone = false;

    std::vector<Entry> g_mine;
    std::string g_mineText; uint32_t g_mineHash = 0;
    bool g_mineDirty = true;

    std::vector<Entry> g_peer;
    bool g_peerReady = false, g_peerRestart = false;
    uint32_t g_peerHash = 0, g_rxHash = 0;
    std::vector<std::string> g_rxParts; std::vector<bool> g_rxHave;

    bool g_movedThisRun = false;              // что-то выключили — AML-моды уйдут только после перезапуска
    std::vector<std::string> g_movedNames;
    uint32_t g_actedOnPeer = 0;               // по какому списку хоста уже переносили
    uint64_t g_nextSendUs = 0, g_offlineSinceUs = 0;
    bool g_wasConnected = false;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[600]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        ctrace::Line("МОДЫ", "%s%s", error ? "ОШИБКА: " : "", buf);
    }

    std::string Lower(std::string s) { for(auto& c : s) c = (char)tolower((unsigned char)c); return s; }
    bool Exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
    bool IsDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
    long FileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }
    void MkDirs(const std::string& p) { for(size_t i = 1; i < p.size(); ++i) if(p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777); mkdir(p.c_str(), 0777); }

    // Свои и обязательные моды не трогаем никогда
    bool Protected(const Entry& e)
    {
        if(e.kind != 'a') return false;
        std::string n = Lower(e.name);
        return n.rfind("libsacoop", 0) == 0 || n.rfind("libsaliving", 0) == 0 || n.rfind("libsautils", 0) == 0;
    }

    std::string Root()
    {
        const char* r = aml->GetInternalPath();
        std::string s = r && r[0] ? r : "/storage/emulated/0";
        while(s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    }

    std::string Pkg()
    {   // /data/data/<пакет>/… или /data/user/0/<пакет>/…
        const char* d = aml->GetDataPath();
        std::string s = d ? d : "";
        for(const char* pre : { "/data/data/", "/data/user/0/" })
        {
            size_t p = s.find(pre);
            if(p == std::string::npos) continue;
            size_t a = p + strlen(pre), b = s.find('/', a);
            std::string pk = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if(!pk.empty()) return pk;
        }
        return "com.rockstargames.gtasa";
    }

    bool HasOurMod(const std::string& dir)
    {
        bool found = false;
        if(DIR* d = opendir(dir.c_str()))
        {
            while(dirent* e = readdir(d)) if(Lower(e->d_name).rfind("libsacoop", 0) == 0) { found = true; break; }
            closedir(d);
        }
        return found;
    }

    void FindPaths()
    {
        if(g_pathsDone) return;
        g_pathsDone = true;
        std::string root = Root(), pkg = Pkg();
        std::vector<std::string> cand = { root + "/Android_unprotected/data/" + pkg + "/mods", root + "/Android/data/" + pkg + "/mods",
                                          root + "/Android/data/" + pkg + "/files/mods" };
        if(DIR* d = opendir((root + "/AMLMods").c_str()))
        {
            while(dirent* e = readdir(d)) if(e->d_name[0] != '.') cand.push_back(root + "/AMLMods/" + e->d_name);
            closedir(d);
        }
        for(auto& c : cand) if(HasOurMod(c)) { g_modsDir = c; break; }
        g_cleoDir = root + "/SA/cleo";
        g_offDir = root + "/SA/SACoop/Моды_выключены_для_коопа";
        Log(g_modsDir.empty(), "Папка AML-модов: %s; CLEO: %s", g_modsDir.empty() ? "НЕ НАЙДЕНА (сверка AML-модов выключена)" : g_modsDir.c_str(), g_cleoDir.c_str());
    }

    void ScanDir(const std::string& dir, char kind, std::vector<Entry>& out)
    {
        if(dir.empty()) return;
        DIR* d = opendir(dir.c_str());
        if(!d) return;
        while(dirent* e = readdir(d))
        {
            if(e->d_name[0] == '.') continue;
            std::string abs = dir + "/" + e->d_name;
            if(IsDir(abs)) continue;                                   // только файлы верхнего уровня
            if(kind == 'a' && Lower(e->d_name).size() > 3 && Lower(e->d_name).compare(Lower(e->d_name).size() - 3, 3, ".so") != 0) continue;
            out.push_back(Entry{ kind, e->d_name, FileSize(abs), abs });
        }
        closedir(d);
    }

    uint32_t Fnv(const std::string& s) { uint32_t h = 2166136261u; for(unsigned char c : s) { h ^= c; h *= 16777619u; } return h ? h : 1; }

    void ScanMine()
    {
        FindPaths();
        g_mine.clear();
        ScanDir(g_modsDir, 'a', g_mine);
        ScanDir(g_cleoDir, 'c', g_mine);
        std::sort(g_mine.begin(), g_mine.end(), [](const Entry& x, const Entry& y) { return x.kind != y.kind ? x.kind < y.kind : Lower(x.name) < Lower(y.name); });
        std::string t;
        if(g_movedThisRun) t += "r|1|0\n";                         // «перезапусти игру — выключенные моды ещё загружены»
        if(g_modsDir.empty()) t += "n|aml|0\n";                    // папку AML не нашли — AML не сравнивать
        for(auto& e : g_mine) { char b[64]; snprintf(b, sizeof(b), "|%ld\n", e.size); t += std::string(1, e.kind) + "|" + e.name + b; }
        g_mineText = t; g_mineHash = Fnv(t); g_mineDirty = false;
    }

    void ParsePeer(const std::string& t)
    {
        g_peer.clear(); g_peerRestart = false;
        bool noAml = false;
        size_t pos = 0;
        while(pos < t.size())
        {
            size_t nl = t.find('\n', pos); if(nl == std::string::npos) nl = t.size();
            std::string l = t.substr(pos, nl - pos); pos = nl + 1;
            size_t a = l.find('|'), b = l.rfind('|');
            if(a != 1 || b == a) continue;
            char k = l[0]; std::string name = l.substr(2, b - 2); long sz = atol(l.c_str() + b + 1);
            if(k == 'r') g_peerRestart = true;
            else if(k == 'n') noAml = true;
            else if(k == 'a' || k == 'c') g_peer.push_back(Entry{ k, name, sz, "" });
        }
        if(noAml) g_peer.push_back(Entry{ 'n', "", 0, "" });
    }

    // ---- выключить / вернуть ----
    std::string ManifestPath() { return g_offDir + "/список.txt"; }

    bool MoveFile(const std::string& from, const std::string& to)
    {
        if(rename(from.c_str(), to.c_str()) == 0) return true;
        FILE* in = fopen(from.c_str(), "rb"); if(!in) return false;          // другой раздел — копия и удаление
        FILE* out = fopen(to.c_str(), "wb"); if(!out) { fclose(in); return false; }
        char buf[65536]; size_t n; bool ok = true;
        while((n = fread(buf, 1, sizeof(buf), in)) > 0) if(fwrite(buf, 1, n, out) != n) { ok = false; break; }
        fclose(in); if(fclose(out) != 0) ok = false;
        if(!ok) { remove(to.c_str()); return false; }
        return remove(from.c_str()) == 0;
    }

    void Disable(const std::vector<Entry>& extra)
    {
        if(extra.empty()) return;
        MkDirs(g_offDir + "/aml"); MkDirs(g_offDir + "/cleo");
        FILE* man = fopen(ManifestPath().c_str(), "ab");
        int moved = 0;
        for(auto& e : extra)
        {
            std::string to = g_offDir + (e.kind == 'a' ? "/aml/" : "/cleo/") + e.name;
            if(Exists(to)) remove(to.c_str());
            if(!MoveFile(e.abs, to)) { Log(true, "Не смог убрать %s (%s)", e.abs.c_str(), strerror(errno)); continue; }
            if(man) fprintf(man, "%s\t%s\n", e.abs.c_str(), to.c_str());
            ++moved; g_movedNames.push_back(e.name);
            Log(false, "Выключил для коопа (нет у хоста): %s → %s", e.abs.c_str(), to.c_str());
        }
        if(man) fclose(man);
        if(moved)
        {
            g_movedThisRun = true; g_mineDirty = true;
            std::lock_guard<std::mutex> l(g_mx);
            g_notice = "Убрал моды, которых нет у хоста (" + std::to_string(moved) + "). Перезапусти игру, чтобы они выключились";
        }
    }

    int Restore(const char* why)
    {
        FindPaths();
        FILE* man = fopen(ManifestPath().c_str(), "rb");
        if(!man) return 0;
        std::vector<std::pair<std::string, std::string>> rows, keep;
        char line[2048];
        while(fgets(line, sizeof(line), man))
        {
            std::string l = line; while(!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            size_t t = l.find('\t'); if(t == std::string::npos) continue;
            rows.push_back({ l.substr(0, t), l.substr(t + 1) });
        }
        fclose(man);
        int back = 0;
        for(auto& r : rows)
        {
            if(!Exists(r.second)) continue;                                         // уже вернули
            if(Exists(r.first)) { Log(false, "Не возвращаю %s: на месте уже есть такой файл (лежит в %s)", r.first.c_str(), r.second.c_str()); keep.push_back(r); continue; }
            size_t sl = r.first.rfind('/'); if(sl != std::string::npos) MkDirs(r.first.substr(0, sl));
            if(MoveFile(r.second, r.first)) { ++back; Log(false, "Вернул: %s", r.first.c_str()); }
            else { keep.push_back(r); Log(true, "Не смог вернуть %s (%s)", r.first.c_str(), strerror(errno)); }
        }
        if(keep.empty()) remove(ManifestPath().c_str());
        else if(FILE* w = fopen(ManifestPath().c_str(), "wb")) { for(auto& r : keep) fprintf(w, "%s\t%s\n", r.first.c_str(), r.second.c_str()); fclose(w); }
        if(back)
        {
            Log(false, "Вернул свои моды (%d) — %s. Заработают после перезапуска игры", back, why);
            g_mineDirty = true;
            std::lock_guard<std::mutex> l(g_mx);
            g_notice = "Вернул твои моды (" + std::to_string(back) + "): заработают после перезапуска игры";
        }
        return back;
    }

    // ---- сравнение ----
    std::string Join(const std::vector<std::string>& v, size_t max = 6)
    {
        std::string s;
        for(size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
        if(v.size() > max) s += " и ещё " + std::to_string(v.size() - max);
        return s;
    }

    void Compare(bool isHost)
    {
        bool peerNoAml = false;
        std::map<std::string, const Entry*> mine, peer;
        for(auto& e : g_mine) mine[std::string(1, e.kind) + Lower(e.name)] = &e;
        for(auto& e : g_peer) { if(e.kind == 'n') { peerNoAml = true; continue; } peer[std::string(1, e.kind) + Lower(e.name)] = &e; }
        bool skipAml = peerNoAml || g_modsDir.empty();
        // «лишние» — у друга есть, у хоста нет; «не хватает» — у хоста есть, у друга нет
        const auto& hostSet = isHost ? mine : peer;
        const auto& mateSet = isHost ? peer : mine;
        std::vector<Entry> extraMine;
        std::vector<std::string> extra, missing, differ;
        for(auto& kv : mateSet)
        {
            if(skipAml && kv.first[0] == 'a') continue;
            if(Protected(*kv.second)) continue;
            auto h = hostSet.find(kv.first);
            if(h == hostSet.end()) { extra.push_back(kv.second->name); if(!isHost) extraMine.push_back(*kv.second); }
            else if(h->second->size != kv.second->size) differ.push_back(kv.second->name);
        }
        for(auto& kv : hostSet)
        {
            if(skipAml && kv.first[0] == 'a') continue;
            if(Protected(*kv.second)) continue;
            if(mateSet.find(kv.first) == mateSet.end()) missing.push_back(kv.second->name);
        }

        if(!isHost && !extraMine.empty() && g_actedOnPeer != g_peerHash)
        {
            g_actedOnPeer = g_peerHash;
            Disable(extraMine);
            return;                                                                 // пересчитаем по новому списку
        }

        std::vector<Line> lines;
        int nA = 0, nC = 0; for(auto& e : g_mine) (e.kind == 'a' ? nA : nC)++;
        bool restart = isHost ? g_peerRestart : g_movedThisRun;
        if(extra.empty() && missing.empty() && differ.empty() && !restart)
            lines.push_back({ "Моды совпадают с " + std::string(isHost ? "другом" : "хостом") + " (AML " + std::to_string(nA) + ", CLEO " + std::to_string(nC) + ")" + (skipAml ? " — AML не сверялись" : ""), 0 });
        if(restart)
            lines.push_back({ isHost ? "Друг убрал лишние моды — ему нужно перезапустить игру, чтобы они выключились"
                                     : "Убрал моды, которых нет у хоста: " + Join(g_movedNames) + ". ПЕРЕЗАПУСТИ ИГРУ, чтобы они выключились", 2 });
        if(!extra.empty())
            lines.push_back({ (isHost ? "У друга лишние (уберутся у него): " : "У тебя лишние, не смог убрать: ") + Join(extra), 1 });
        if(!missing.empty())
            lines.push_back({ (isHost ? "У друга нет: " : "У хоста есть, у тебя нет — поставь: ") + Join(missing), 1 });
        if(!differ.empty())
            lines.push_back({ "Разные версии (другой размер файла): " + Join(differ), 1 });
        {
            std::lock_guard<std::mutex> l(g_mx);
            g_lines = lines;
        }
        std::string all; for(auto& ln : lines) all += " | " + ln.text;
        static std::string last;
        if(all != last) { last = all; Log(false, "Сверка с %s:%s", isHost ? "другом" : "хостом", all.c_str()); }
    }

    void SendList()
    {
        if(!g_session) return;
        const std::string& t = g_mineText;
        int parts = (int)((t.size() + net::kModsChunkMax - 1) / net::kModsChunkMax); if(parts < 1) parts = 1;
        if(parts > 64) parts = 64;
        for(int i = 0; i < parts; ++i)
        {
            net::ModsPacket p{};
            p.hdr = net::MakeHeader(net::PKT_MODS);
            p.hash = g_mineHash; p.part = (uint16_t)i; p.parts = (uint16_t)parts;
            size_t off = (size_t)i * net::kModsChunkMax, n = off < t.size() ? std::min((size_t)net::kModsChunkMax, t.size() - off) : 0;
            p.len = (uint16_t)n;
            if(n) memcpy(p.data, t.data() + off, n);
            g_session->SendGame(&p, net::kModsHeaderSize + (int)n);
        }
    }
}

void Init(net::Role role)
{
    if(role != net::Role::Client) Restore("режим не «Подключиться»");
}

void SetSession(net::Session* s) { g_session = s; }

void OnRoleChanged(net::Role role)
{
    g_peerReady = false; g_rxHash = 0; g_peerHash = 0;
    { std::lock_guard<std::mutex> l(g_mx); g_lines.clear(); }
    if(role != net::Role::Client) Restore("кооп выключен или ты хост");
}

void Frame(net::Role role, bool connected, bool inGame, uint64_t now)
{
    if(role != net::Role::Host && role != net::Role::Client) return;
    if(!connected)
    {
        if(g_wasConnected) { g_peerReady = false; g_rxHash = 0; std::lock_guard<std::mutex> l(g_mx); g_lines.clear(); }
        g_wasConnected = false;
        // друг играет один: в игре без связи дольше минуты — вернуть его моды
        if(role == net::Role::Client && inGame)
        {
            if(!g_offlineSinceUs) g_offlineSinceUs = now;
            else if(net::Since(now, g_offlineSinceUs) > 60000000) { g_offlineSinceUs = now; Restore("в игре без коопа больше минуты"); }
        }
        else g_offlineSinceUs = 0;
        return;
    }
    g_offlineSinceUs = 0;
    if(!g_wasConnected) { g_wasConnected = true; g_mineDirty = true; g_nextSendUs = 0; g_peerHash = 0; g_rxHash = 0; }
    if(g_mineDirty) ScanMine();
    if(now >= g_nextSendUs) { g_nextSendUs = now + 3000000; SendList(); }
    if(g_peerReady) { g_peerReady = false; Compare(role == net::Role::Host); if(g_mineDirty) { ScanMine(); g_nextSendUs = 0; g_peerReady = true; } }
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(type != net::PKT_MODS || len < net::kModsHeaderSize) return;
    const auto* p = (const net::ModsPacket*)data;
    if(p->len > net::kModsChunkMax || net::kModsHeaderSize + (int)p->len != len || !p->parts || p->parts > 64 || p->part >= p->parts) return;
    if(p->hash == g_peerHash && !g_rxParts.empty() && g_rxHash == 0) return;            // этот список уже разобран
    if(p->hash != g_rxHash || g_rxParts.size() != p->parts)
    {
        g_rxHash = p->hash; g_rxParts.assign(p->parts, std::string()); g_rxHave.assign(p->parts, false);
    }
    g_rxParts[p->part].assign((const char*)p->data, p->len); g_rxHave[p->part] = true;
    for(bool h : g_rxHave) if(!h) return;
    std::string all; for(auto& s : g_rxParts) all += s;
    g_peerHash = g_rxHash; g_rxHash = 0;
    ParsePeer(all);
    g_peerReady = true;
}

std::vector<Line> LobbyLines()
{
    std::lock_guard<std::mutex> l(g_mx);
    return g_lines;
}

bool PopNotice(std::string& out)
{
    std::lock_guard<std::mutex> l(g_mx);
    if(g_notice.empty()) return false;
    out.swap(g_notice); g_notice.clear();
    return true;
}
}
