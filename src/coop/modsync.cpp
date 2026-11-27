#include "coop/modsync.h"

#include <mod/amlmod.h>
#include <mod/logger.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "net/protocol.h"
#include "trace/ctrace.h"

namespace modsync
{
namespace
{
    // kind: 'a' — AML-мод (.so), 'c' — CLEO (SA/cleo, с папками), 'g' — файлы игры и данных модов (папка files игры)
    struct Entry { char kind = 0; std::string rel; int64_t size = 0; uint64_t hash = 0; std::string abs; bool secret = false; };

    net::Session* g_session = nullptr;
    std::mutex g_mx;                          // строки/кнопки лобби читает меню
    std::vector<Line> g_lines;
    std::string g_notice;

    std::string g_root, g_modsDir, g_cleoDir, g_filesDir, g_offDir, g_bakDir;
    bool g_pathsDone = false;

    // свой список (считается фоном: контрольные суммы)
    std::vector<Entry> g_mine;
    std::string g_mineText; uint32_t g_mineHash = 0;
    bool g_mineDirty = true, g_mineReady = false;
    std::atomic<int> g_scanState{ 0 };        // 0 — нет, 1 — идёт, 2 — готово (забрать)
    std::vector<Entry> g_scanOut;             // под g_mx

    // список второго телефона
    std::vector<Entry> g_peer;
    bool g_peerReady = false, g_peerRestart = false, g_peerNoAml = false, g_peerNoFiles = false, g_peerKnown = false;
    uint32_t g_peerHash = 0, g_rxHash = 0;
    std::vector<std::string> g_rxParts; std::vector<bool> g_rxHave;

    bool g_restartNeeded = false;             // друг: что-то убрали/поставили — моды AML сменятся после перезапуска
    std::vector<std::string> g_changedNames;
    uint32_t g_actedOnPeer = 0;
    std::vector<int> g_installIdx;            // друг: что взять у хоста (номера в списке хоста)
    int64_t g_installBytes = 0;
    bool g_matched = false, g_skip = false;   // хост: моды совпали / «Играть без сверки»
    uint64_t g_nextSendUs = 0, g_offlineSinceUs = 0;
    bool g_wasConnected = false;
    net::Role g_role = net::Role::Off;

    // передача файлов: хост
    struct Pending { uint32_t seq; std::string pkt; uint64_t sentUs; };
    struct HostSend
    {
        bool active = false; uint32_t req = 0;
        std::vector<Entry> files; size_t fi = 0; int64_t off = 0; int fd = -1;
        uint32_t nextSeq = 0; std::deque<Pending> inflight;
        int64_t total = 0, sent = 0; uint64_t nextTickUs = 0;
    } g_hs;
    // передача файлов: друг
    struct ClientRecv
    {
        bool active = false, started = false; uint32_t req = 0, expect = 0;
        std::vector<Entry> files; int fd = -1; std::string tmp; int cur = -1;
        int64_t total = 0, done = 0; uint64_t lastReqUs = 0; int ok = 0, fail = 0;
    } g_cr;

    void Log(bool error, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void Log(bool error, const char* fmt, ...)
    {
        char buf[700]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
        if(error) logger->Error("%s", buf); else logger->Info("%s", buf);
        ctrace::Line("МОДЫ", "%s%s", error ? "ОШИБКА: " : "", buf);
    }
    void Notice(const std::string& s) { std::lock_guard<std::mutex> l(g_mx); g_notice = s; }

    std::string Lower(std::string s) { for(auto& c : s) c = (char)tolower((unsigned char)c); return s; }
    bool Exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
    bool IsDir(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
    void MkDirs(const std::string& p) { for(size_t i = 1; i < p.size(); ++i) if(p[i] == '/') mkdir(p.substr(0, i).c_str(), 0777); mkdir(p.c_str(), 0777); }
    void MkParent(const std::string& p) { size_t s = p.rfind('/'); if(s != std::string::npos && s > 0) MkDirs(p.substr(0, s)); }
    bool EndsWith(const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
    std::string Mb(int64_t b) { char t[32]; snprintf(t, sizeof(t), "%.1f МБ", b / 1048576.0); return t; }

    bool ProtectedAml(const std::string& name) { return Lower(name).rfind("libsacoop", 0) == 0; }   // свой мод — никогда

    // ---------- пути ----------
    std::string Pkg()
    {
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
        if(DIR* d = opendir(dir.c_str())) { while(dirent* e = readdir(d)) if(Lower(e->d_name).rfind("libsacoop", 0) == 0) { found = true; break; } closedir(d); }
        return found;
    }
    void FindPaths()
    {
        if(g_pathsDone) return;
        g_pathsDone = true;
        const char* r = aml->GetInternalPath();
        g_root = r && r[0] ? r : "/storage/emulated/0";
        while(g_root.size() > 1 && g_root.back() == '/') g_root.pop_back();
        std::string pkg = Pkg();
        std::vector<std::string> cand = { g_root + "/Android_unprotected/data/" + pkg + "/mods", g_root + "/Android/data/" + pkg + "/mods",
                                          g_root + "/Android/data/" + pkg + "/files/mods" };
        if(DIR* d = opendir((g_root + "/AMLMods").c_str())) { while(dirent* e = readdir(d)) if(e->d_name[0] != '.') cand.push_back(g_root + "/AMLMods/" + e->d_name); closedir(d); }
        for(auto& c : cand) if(HasOurMod(c)) { g_modsDir = c; break; }
        // папка files игры — рядом с mods (там лежат замены файлов и данные модов)
        if(!g_modsDir.empty())
        {
            std::string parent = g_modsDir.substr(0, g_modsDir.rfind('/'));
            if(IsDir(parent + "/files")) g_filesDir = parent + "/files";
        }
        if(g_filesDir.empty())
            for(const char* b : { "/Android_unprotected/data/", "/Android/data/" })
                if(IsDir(g_root + b + pkg + "/files")) { g_filesDir = g_root + b + pkg + "/files"; break; }
        g_cleoDir = g_root + "/SA/cleo";
        g_offDir = g_root + "/SA/SACoop/Отключённые моды";
        g_bakDir = g_root + "/SA/SACoop/Резерв модов";
        Log(g_modsDir.empty(), "Папки: AML %s; CLEO %s; файлы игры %s", g_modsDir.empty() ? "НЕ НАЙДЕНА (AML не сверяются)" : g_modsDir.c_str(),
            g_cleoDir.c_str(), g_filesDir.empty() ? "не найдена" : g_filesDir.c_str());
    }
    std::string BaseOf(char kind) { return kind == 'a' ? g_modsDir : kind == 'c' ? g_cleoDir : kind == 'g' ? g_filesDir : std::string(); }
    bool SafeRel(const std::string& rel) { return !rel.empty() && rel[0] != '/' && rel.find("..") == std::string::npos && rel.find('\\') == std::string::npos && rel.find('|') == std::string::npos; }

    // Что из папки files не сверяем: личное, сохранения, настройки, кэши. SA Living — только speech (голоса, распознавание).
    bool SkipGameFile(const std::string& rel)
    {
        std::string l = Lower(rel);
        size_t sl = l.find('/');
        std::string top = sl == std::string::npos ? l : l.substr(0, sl);
        if(top == "saliving") return l.rfind("saliving/speech/", 0) != 0;
        static const char* kSkipTop[] = { "sacoop", "cache", "tmp", "temp", "logs", "documents", "amlmodlogs", "configs", "mods", "files" };
        for(const char* s : kSkipTop) if(top == s) return true;
        static const char* kSkipExt[] = { ".b", ".set", ".log", ".tmp", ".part", ".sacoop_part", ".ini", ".json", ".txt", ".bak" };
        for(const char* e : kSkipExt) if(EndsWith(l, e)) return true;
        return false;
    }

    // ---------- контрольные суммы (фоном, с кэшем) ----------
    struct CacheRow { int64_t size; int64_t mtime; uint64_t hash; bool secret; };
    std::string CachePath() { return g_root + "/SA/SACoop/моды_контрольные_суммы.txt"; }

    // FNV-1a 64 по содержимому + поиск ключей внутри (sk-ant-, tskey-auth-)
    bool HashFile(const std::string& path, uint64_t& h, bool& secret)
    {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0) return false;
        h = 1469598103934665603ull; secret = false;
        static const char* kSecret[] = { "sk-ant-", "tskey-auth-" };
        char buf[65536 + 16]; size_t keep = 0; ssize_t n;
        while((n = read(fd, buf + keep, 65536)) > 0)
        {
            for(ssize_t i = 0; i < n; ++i) { h ^= (unsigned char)buf[keep + i]; h *= 1099511628211ull; }
            size_t tot = keep + (size_t)n;
            if(!secret) for(const char* k : kSecret) if(memmem(buf, tot, k, strlen(k))) { secret = true; break; }
            keep = std::min<size_t>(15, tot);
            memmove(buf, buf + tot - keep, keep);
        }
        close(fd);
        return n == 0;
    }

    void Walk(const std::string& base, const std::string& rel, char kind, int depth, std::vector<Entry>& out)
    {
        std::string dir = rel.empty() ? base : base + "/" + rel;
        DIR* d = opendir(dir.c_str());
        if(!d) return;
        while(dirent* e = readdir(d))
        {
            if(e->d_name[0] == '.') continue;
            std::string r = rel.empty() ? std::string(e->d_name) : rel + "/" + e->d_name;
            std::string abs = base + "/" + r;
            struct stat st; if(stat(abs.c_str(), &st) != 0) continue;
            if(S_ISDIR(st.st_mode))
            {
                if(kind == 'a' || depth >= 6) continue;                     // AML — только верхний уровень
                if(kind == 'g' && Lower(r) != "saliving" && SkipGameFile(r + "/x")) continue;   // SALiving — только ради speech
                Walk(base, r, kind, depth + 1, out);
                continue;
            }
            if(!S_ISREG(st.st_mode) || !SafeRel(r)) continue;
            if(kind == 'a' && !EndsWith(Lower(r), ".so")) continue;
            if(kind == 'g' && (SkipGameFile(r) || st.st_size > (600ll << 20))) continue;
            Entry en; en.kind = kind; en.rel = r; en.size = st.st_size; en.abs = abs; en.hash = (uint64_t)st.st_mtime;   // mtime — временно
            out.push_back(en);
        }
        closedir(d);
    }

    void ScanThread()
    {
        std::vector<Entry> list;
        if(!g_modsDir.empty()) Walk(g_modsDir, "", 'a', 0, list);
        Walk(g_cleoDir, "", 'c', 0, list);
        if(!g_filesDir.empty()) Walk(g_filesDir, "", 'g', 0, list);
        // кэш: путь → размер, время, сумма
        std::map<std::string, CacheRow> cache;
        if(FILE* f = fopen(CachePath().c_str(), "rb"))
        {
            char line[2048];
            while(fgets(line, sizeof(line), f))
            {
                char* p[5] = {}; int k = 0; p[k++] = line;
                for(char* c = line; *c && k < 5; ++c) if(*c == '\t') { *c = 0; p[k++] = c + 1; }
                if(k < 5) continue;
                cache[p[0]] = CacheRow{ atoll(p[1]), atoll(p[2]), strtoull(p[3], nullptr, 16), atoi(p[4]) != 0 };
            }
            fclose(f);
        }
        std::string out;
        int hashed = 0, secrets = 0;
        for(auto& e : list)
        {
            int64_t mt = (int64_t)e.hash;
            auto it = cache.find(e.abs);
            if(it != cache.end() && it->second.size == e.size && it->second.mtime == mt) { e.hash = it->second.hash; e.secret = it->second.secret; }
            else
            {
                uint64_t h; bool sec;
                if(!HashFile(e.abs, h, sec)) { e.hash = 0; continue; }
                e.hash = h; e.secret = sec; ++hashed;
            }
            if(e.secret) { ++secrets; Log(false, "Файл %s похож на секрет (ключ внутри) — в сверку и передачу не идёт", e.abs.c_str()); }
            char row[2200]; snprintf(row, sizeof(row), "%s\t%lld\t%lld\t%016llx\t%d\n", e.abs.c_str(), (long long)e.size, (long long)mt, (unsigned long long)e.hash, e.secret ? 1 : 0);
            out += row;
        }
        MkDirs(g_root + "/SA/SACoop");
        if(FILE* f = fopen(CachePath().c_str(), "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
        list.erase(std::remove_if(list.begin(), list.end(), [](const Entry& e) { return e.secret || (e.kind == 'a' && ProtectedAml(e.rel)); }), list.end());
        std::sort(list.begin(), list.end(), [](const Entry& x, const Entry& y) { return x.kind != y.kind ? x.kind < y.kind : Lower(x.rel) < Lower(y.rel); });
        Log(false, "Свои моды посчитаны: файлов %d (контрольных сумм посчитано заново %d, секретов %d)", (int)list.size(), hashed, secrets);
        { std::lock_guard<std::mutex> l(g_mx); g_scanOut.swap(list); }
        g_scanState = 2;
    }

    void StartScan()
    {
        if(g_scanState == 1) return;
        FindPaths();
        g_scanState = 1; g_mineDirty = false;
        std::thread(ScanThread).detach();
    }

    void TakeScan()
    {
        { std::lock_guard<std::mutex> l(g_mx); g_mine.swap(g_scanOut); g_scanOut.clear(); }
        g_scanState = 0;
        std::string t;
        if(g_restartNeeded) t += "r|1|0|0\n";
        if(g_modsDir.empty()) t += "n|aml|0|0\n";
        if(g_filesDir.empty()) t += "f|files|0|0\n";
        for(auto& e : g_mine)
        {
            char b[80]; snprintf(b, sizeof(b), "|%lld|%016llx\n", (long long)e.size, (unsigned long long)e.hash);
            t += std::string(1, e.kind) + "|" + e.rel + b;
        }
        uint32_t h = 2166136261u; for(unsigned char c : t) { h ^= c; h *= 16777619u; }
        g_mineText = t; g_mineHash = h ? h : 1; g_mineReady = true;
        g_nextSendUs = 0;
    }

    void ParsePeer(const std::string& t)
    {
        g_peer.clear(); g_peerRestart = false; g_peerNoAml = false; g_peerNoFiles = false;
        size_t pos = 0;
        while(pos < t.size())
        {
            size_t nl = t.find('\n', pos); if(nl == std::string::npos) nl = t.size();
            std::string l = t.substr(pos, nl - pos); pos = nl + 1;
            if(l.size() < 7 || l[1] != '|') continue;
            size_t b2 = l.rfind('|'); if(b2 == std::string::npos || b2 < 3) continue;
            size_t b1 = l.rfind('|', b2 - 1); if(b1 == std::string::npos || b1 < 2) continue;
            char k = l[0];
            if(k == 'r') { g_peerRestart = true; continue; }
            if(k == 'n') { g_peerNoAml = true; continue; }
            if(k == 'f') { g_peerNoFiles = true; continue; }
            if(k != 'a' && k != 'c' && k != 'g') continue;
            Entry e; e.kind = k; e.rel = l.substr(2, b1 - 2); e.size = atoll(l.c_str() + b1 + 1); e.hash = strtoull(l.c_str() + b2 + 1, nullptr, 16);
            if(SafeRel(e.rel)) g_peer.push_back(e);
        }
    }

    // ---------- отключить / вернуть / резерв ----------
    bool MoveFile(const std::string& from, const std::string& to)
    {
        MkParent(to);
        if(rename(from.c_str(), to.c_str()) == 0) return true;
        FILE* in = fopen(from.c_str(), "rb"); if(!in) return false;
        FILE* out = fopen(to.c_str(), "wb"); if(!out) { fclose(in); return false; }
        char buf[65536]; size_t n; bool ok = true;
        while((n = fread(buf, 1, sizeof(buf), in)) > 0) if(fwrite(buf, 1, n, out) != n) { ok = false; break; }
        fclose(in); if(fclose(out) != 0) ok = false;
        if(!ok) { remove(to.c_str()); return false; }
        return remove(from.c_str()) == 0;
    }
    const char* KindDir(char k) { return k == 'a' ? "aml" : k == 'c' ? "cleo" : "files"; }
    std::string ManifestPath() { return g_offDir + "/список.txt"; }

    void Disable(const std::vector<Entry>& extra)
    {
        if(extra.empty()) return;
        MkDirs(g_offDir);
        FILE* man = fopen(ManifestPath().c_str(), "ab");
        int moved = 0;
        for(auto& e : extra)
        {
            std::string to = g_offDir + "/" + KindDir(e.kind) + "/" + e.rel;
            if(Exists(to)) remove(to.c_str());
            if(!MoveFile(e.abs, to)) { Log(true, "Не смог убрать %s (%s)", e.abs.c_str(), strerror(errno)); continue; }
            if(man) fprintf(man, "%s\t%s\n", e.abs.c_str(), to.c_str());
            ++moved; g_changedNames.push_back(e.rel);
            Log(false, "Отключил на время коопа (нет у хоста): %s", e.abs.c_str());
        }
        if(man) fclose(man);
        if(moved)
        {
            g_restartNeeded = true; g_mineDirty = true;
            Notice("Отключил моды, которых нет у хоста (" + std::to_string(moved) + "). Перезапусти игру");
        }
    }

    int RestoreFrom(const std::string& manifest)
    {
        FILE* man = fopen(manifest.c_str(), "rb");
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
            if(!Exists(r.second)) continue;
            if(Exists(r.first)) { Log(false, "Не возвращаю %s: на месте уже есть такой файл (лежит в %s)", r.first.c_str(), r.second.c_str()); keep.push_back(r); continue; }
            if(MoveFile(r.second, r.first)) { ++back; Log(false, "Вернул: %s", r.first.c_str()); }
            else { keep.push_back(r); Log(true, "Не смог вернуть %s (%s)", r.first.c_str(), strerror(errno)); }
        }
        if(keep.empty()) remove(manifest.c_str());
        else if(FILE* w = fopen(manifest.c_str(), "wb")) { for(auto& r : keep) fprintf(w, "%s\t%s\n", r.first.c_str(), r.second.c_str()); fclose(w); }
        return back;
    }

    int Restore(const char* why)
    {
        FindPaths();
        int back = RestoreFrom(ManifestPath()) + RestoreFrom(g_root + "/SA/SACoop/Моды_выключены_для_коопа/список.txt");   // папка 1.5.0
        if(back)
        {
            Log(false, "Вернул свои моды (%d) — %s. Заработают после перезапуска игры", back, why);
            g_mineDirty = true;
            Notice("Вернул твои моды (" + std::to_string(back) + "): заработают после перезапуска игры");
        }
        return back;
    }

    // ---------- сравнение ----------
    std::string Join(const std::vector<std::string>& v, size_t max = 5)
    {
        std::string s;
        for(size_t i = 0; i < v.size() && i < max; ++i) s += (i ? ", " : "") + v[i];
        if(v.size() > max) s += " и ещё " + std::to_string(v.size() - max);
        return s;
    }
    std::string Key(const Entry& e) { return std::string(1, e.kind) + Lower(e.rel); }

    void Compare(bool isHost)
    {
        bool skipAml = g_peerNoAml || g_modsDir.empty();
        std::map<std::string, int> mineIdx, peerIdx;
        for(int i = 0; i < (int)g_mine.size(); ++i) mineIdx[Key(g_mine[i])] = i;
        for(int i = 0; i < (int)g_peer.size(); ++i) peerIdx[Key(g_peer[i])] = i;
        const std::vector<Entry>& hostL = isHost ? g_mine : g_peer;
        const std::vector<Entry>& mateL = isHost ? g_peer : g_mine;
        const auto& hostI = isHost ? mineIdx : peerIdx;
        const auto& mateI = isHost ? peerIdx : mineIdx;
        auto skip = [&](const Entry& e) { return (skipAml && e.kind == 'a') || (e.kind == 'a' && ProtectedAml(e.rel)); };
        bool skipG = g_peerNoFiles || g_filesDir.empty();        // папку files игры у кого-то не нашли — эту часть не сверяем

        std::vector<Entry> extraMine;
        std::vector<std::string> extra, missing, differ;
        std::vector<int> want; int64_t wantBytes = 0;
        for(auto& e : mateL)
        {
            if(skip(e) || (skipG && e.kind == 'g')) continue;
            auto h = hostI.find(Key(e));
            if(h == hostI.end()) { extra.push_back(e.rel); if(!isHost) extraMine.push_back(e); }
            else if(hostL[h->second].size != e.size || hostL[h->second].hash != e.hash)
            { differ.push_back(e.rel); if(!isHost) { want.push_back(h->second); wantBytes += hostL[h->second].size; } }
        }
        for(int i = 0; i < (int)hostL.size(); ++i)
        {
            const Entry& e = hostL[i];
            if(skip(e) || (skipG && e.kind == 'g')) continue;
            if(mateI.find(Key(e)) == mateI.end()) { missing.push_back(e.rel); if(!isHost) { want.push_back(i); wantBytes += e.size; } }
        }

        if(!isHost && !extraMine.empty() && g_actedOnPeer != g_peerHash)
        {   // решение игрока: лишнее у друга — сразу в «Отключённые моды»
            g_actedOnPeer = g_peerHash;
            Disable(extraMine);
            return;
        }
        if(!isHost) { g_installIdx = want; g_installBytes = wantBytes; }

        bool restart = isHost ? g_peerRestart : g_restartNeeded;
        g_matched = isHost && extra.empty() && missing.empty() && differ.empty() && !restart;
        std::vector<Line> lines;
        int nA = 0, nC = 0, nG = 0; for(auto& e : g_mine) (e.kind == 'a' ? nA : e.kind == 'c' ? nC : nG)++;
        if(extra.empty() && missing.empty() && differ.empty() && !restart)
            lines.push_back({ "Моды: совпадают (AML " + std::to_string(nA) + ", CLEO " + std::to_string(nC) + ", файлы " + std::to_string(nG) + ")" + (skipAml ? " — AML не сверялись" : ""), 0 });
        if(restart)
            lines.push_back({ isHost ? "Другу нужно перезапустить игру — моды у него сменились, включатся после перезапуска"
                                     : "Моды сменились (" + Join(g_changedNames) + "). ПЕРЕЗАПУСТИ ИГРУ и подключись снова", 2 });
        if(!missing.empty()) lines.push_back({ std::string(isHost ? "У друга не хватает " : "Не хватает ") + std::to_string(missing.size()) + ": " + Join(missing), 1 });
        if(!differ.empty())  lines.push_back({ std::string(isHost ? "У друга другие версии " : "Другие версии ") + std::to_string(differ.size()) + ": " + Join(differ), 1 });
        if(!extra.empty())   lines.push_back({ std::string(isHost ? "У друга лишние " : "Лишние, не смог отключить ") + std::to_string(extra.size()) + ": " + Join(extra), 1 });
        if(!isHost && !want.empty()) lines.push_back({ "Нажми «Установить от хоста» — файлы придут с его телефона (" + Mb(wantBytes) + ")", 1 });
        if(isHost && !g_matched && !g_skip) lines.push_back({ "«Запуск» — когда моды совпадут (или «Играть без сверки»)", 1 });
        { std::lock_guard<std::mutex> l(g_mx); g_lines = lines; }
        std::string all; for(auto& ln : lines) all += " | " + ln.text;
        static std::string last;
        if(all != last) { last = all; Log(false, "Сверка с %s:%s", isHost ? "другом" : "хостом", all.c_str()); }
    }

    void SendList()
    {
        if(!g_session || !g_mineReady) return;
        const std::string& t = g_mineText;
        int parts = (int)((t.size() + net::kModsChunkMax - 1) / net::kModsChunkMax); if(parts < 1) parts = 1;
        if(parts > 1000) { Log(true, "Список модов слишком большой (%d частей) — сверка выключена", parts); return; }
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

    // ---------- передача файлов: хост ----------
    void HostStop() { if(g_hs.fd >= 0) close(g_hs.fd); g_hs = HostSend{}; }

    void HostTick(uint64_t now)
    {
        if(!g_hs.active || !g_session || now < g_hs.nextTickUs) return;
        g_hs.nextTickUs = now + 50000;
        if(!g_hs.inflight.empty() && net::Since(now, g_hs.inflight.front().sentUs) > 1500000)
            for(auto& p : g_hs.inflight) { g_session->SendGame(p.pkt.data(), (int)p.pkt.size()); p.sentUs = now; }
        for(int k = 0; k < 16 && g_hs.inflight.size() < 128 && g_hs.fi < g_hs.files.size(); ++k)
        {
            const Entry& e = g_hs.files[g_hs.fi];
            net::ModFileChunkPacket p{};
            p.hdr = net::MakeHeader(net::PKT_MODFILE_CHUNK);
            p.req = g_hs.req; p.seq = g_hs.nextSeq++; p.fileNo = (uint16_t)g_hs.fi; p.offset = (uint32_t)g_hs.off; p.total = (uint32_t)e.size;
            if(g_hs.fd < 0) g_hs.fd = open(e.abs.c_str(), O_RDONLY | O_CLOEXEC);
            ssize_t n = 0;
            if(g_hs.fd < 0) { p.flags = 3; Log(true, "Не открыть %s для отправки (%s)", e.abs.c_str(), strerror(errno)); }
            else
            {
                n = pread(g_hs.fd, p.data, net::kModFileChunkMax, (off_t)g_hs.off);
                if(n < 0) { n = 0; p.flags = 3; }
                else if(g_hs.off + n >= e.size || n == 0) p.flags = 1;
            }
            p.len = (uint16_t)n;
            g_hs.off += n; g_hs.sent += n;
            if(p.flags & 1) { if(g_hs.fd >= 0) close(g_hs.fd); g_hs.fd = -1; g_hs.fi++; g_hs.off = 0; }
            Pending pd{ p.seq, std::string((const char*)&p, (size_t)net::kModFileChunkHeaderSize + (size_t)n), now };
            g_session->SendGame(pd.pkt.data(), (int)pd.pkt.size());
            g_hs.inflight.push_back(std::move(pd));
        }
        if(g_hs.fi >= g_hs.files.size() && g_hs.inflight.empty())
        {
            Log(false, "Моды другу отправлены: файлов %d, %s", (int)g_hs.files.size(), Mb(g_hs.sent).c_str());
            HostStop();
        }
    }

    // ---------- передача файлов: друг ----------
    void ClientStop(bool removeTmp)
    {
        if(g_cr.fd >= 0) close(g_cr.fd);
        if(removeTmp && !g_cr.tmp.empty()) remove(g_cr.tmp.c_str());
        g_cr = ClientRecv{};
    }

    void SendReq()
    {
        if(!g_session) return;
        net::ModFileReqPacket r{};
        r.hdr = net::MakeHeader(net::PKT_MODFILE_REQ);
        r.req = g_cr.req; r.listHash = g_peerHash;
        int n = 0;
        for(int idx : g_installIdx) { if(n >= net::kModFileReqMax) break; r.idx[n++] = (uint16_t)idx; }
        r.count = (uint16_t)n;
        g_session->SendGame(&r, net::kModFileReqHeaderSize + n * 2);
    }

    void FinishFile(const Entry& e, bool error)
    {
        if(g_cr.fd >= 0) { close(g_cr.fd); g_cr.fd = -1; }
        std::string dest = BaseOf(e.kind).empty() ? std::string() : BaseOf(e.kind) + "/" + e.rel;
        uint64_t h = 0; bool sec = false;
        if(error || dest.empty() || !HashFile(g_cr.tmp, h, sec) || h != e.hash)
        {
            ++g_cr.fail; remove(g_cr.tmp.c_str());
            Log(true, "Файл от хоста не принят: %s (%s)", e.rel.c_str(), error ? "хост не смог отправить" : dest.empty() ? "нет папки" : "контрольная сумма не совпала");
        }
        else
        {
            if(Exists(dest))
            {   // старая версия — в резерв (можно вернуть руками)
                std::string bak = g_bakDir + "/" + KindDir(e.kind) + "/" + e.rel;
                if(Exists(bak)) remove(bak.c_str());
                if(MoveFile(dest, bak)) Log(false, "Старая версия %s — в «Резерв модов»", e.rel.c_str());
            }
            if(MoveFile(g_cr.tmp, dest)) { ++g_cr.ok; g_changedNames.push_back(e.rel); Log(false, "Установлен от хоста: %s (%s)", dest.c_str(), Mb(e.size).c_str()); }
            else { ++g_cr.fail; Log(true, "Не смог положить %s (%s)", dest.c_str(), strerror(errno)); remove(g_cr.tmp.c_str()); }
        }
        g_cr.tmp.clear(); g_cr.cur = -1;
    }

    void OnChunk(const net::ModFileChunkPacket* p)
    {
        if(!g_cr.active || p->req != g_cr.req || !g_session) return;
        net::ModFileAckPacket a{ net::MakeHeader(net::PKT_MODFILE_ACK), g_cr.req, 0 };
        if(p->seq != g_cr.expect)
        {
            if(g_cr.expect) { a.seq = g_cr.expect - 1; g_session->SendGame(&a, sizeof(a)); }
            return;
        }
        g_cr.started = true; ++g_cr.expect;
        if(p->fileNo < g_cr.files.size())
        {
            const Entry& e = g_cr.files[p->fileNo];
            if(g_cr.cur != (int)p->fileNo)
            {
                if(g_cr.fd >= 0) { close(g_cr.fd); g_cr.fd = -1; }
                std::string base = BaseOf(e.kind);
                g_cr.tmp = base.empty() ? std::string() : base + "/" + e.rel + ".sacoop_part";
                if(!g_cr.tmp.empty()) { MkParent(g_cr.tmp); g_cr.fd = open(g_cr.tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); }
                g_cr.cur = p->fileNo;
            }
            if(g_cr.fd >= 0 && p->len) { if(pwrite(g_cr.fd, p->data, p->len, (off_t)p->offset) != (ssize_t)p->len) Log(true, "Не записать %s (%s)", g_cr.tmp.c_str(), strerror(errno)); }
            g_cr.done += p->len;
            if(p->flags & 1) FinishFile(e, (p->flags & 2) != 0);
            if((p->flags & 1) && p->fileNo + 1u >= g_cr.files.size())
            {
                Log(false, "Моды от хоста: установлено %d, не вышло %d", g_cr.ok, g_cr.fail);
                if(g_cr.ok) { g_restartNeeded = true; Notice("Моды от хоста установлены (" + std::to_string(g_cr.ok) + "). Перезапусти игру и подключись снова"); }
                else Notice("Моды от хоста не установились — смотри лог");
                a.seq = g_cr.expect - 1; g_session->SendGame(&a, sizeof(a));
                ClientStop(false);
                g_mineDirty = true;
                return;
            }
        }
        a.seq = g_cr.expect - 1;
        g_session->SendGame(&a, sizeof(a));
    }
}

void Init(net::Role role)
{
    if(role != net::Role::Client) Restore("режим не «Подключиться»");
}

void SetSession(net::Session* s) { g_session = s; }

void OnRoleChanged(net::Role role)
{
    g_peerReady = false; g_peerKnown = false; g_rxHash = 0; g_peerHash = 0; g_matched = false;
    HostStop(); ClientStop(true);
    { std::lock_guard<std::mutex> l(g_mx); g_lines.clear(); }
    if(role != net::Role::Client) Restore("кооп выключен или ты хост");
}

void Frame(net::Role role, bool connected, bool inGame, uint64_t now)
{
    g_role = role;
    if(role != net::Role::Host && role != net::Role::Client) return;
    if(g_scanState == 2) { TakeScan(); if(g_peerKnown) g_peerReady = true; }
    if(!connected)
    {
        if(g_wasConnected)
        {
            g_peerReady = false; g_peerKnown = false; g_rxHash = 0; g_matched = false;
            if(g_hs.active) Log(false, "Связь пропала — отправка модов прервана");
            if(g_cr.active) Log(false, "Связь пропала — установка модов прервана (нажми ещё раз после подключения)");
            HostStop(); ClientStop(true);
            std::lock_guard<std::mutex> l(g_mx); g_lines.clear();
        }
        g_wasConnected = false;
        if(role == net::Role::Client && inGame)
        {   // друг играет один: в игре без связи дольше минуты — вернуть его моды
            if(!g_offlineSinceUs) g_offlineSinceUs = now;
            else if(net::Since(now, g_offlineSinceUs) > 60000000) { g_offlineSinceUs = now; Restore("в игре без коопа больше минуты"); }
        }
        else g_offlineSinceUs = 0;
        return;
    }
    g_offlineSinceUs = 0;
    if(!g_wasConnected) { g_wasConnected = true; g_mineDirty = true; g_peerHash = 0; g_rxHash = 0; }
    if(g_mineDirty && g_scanState == 0) StartScan();
    if(g_mineReady && now >= g_nextSendUs) { g_nextSendUs = now + 3000000; SendList(); }
    if(g_peerReady && g_mineReady && g_scanState == 0) { g_peerReady = false; Compare(role == net::Role::Host); }
    else if(!g_peerKnown || !g_mineReady)
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_lines = { { g_scanState == 1 ? "Моды: считаю контрольные суммы…" : "Моды: жду список с другого телефона…", 1 } };
    }
    HostTick(now);
    if(g_cr.active)
    {
        if(!g_cr.started && net::Since(now, g_cr.lastReqUs) > 2000000) { g_cr.lastReqUs = now; SendReq(); }
        int pct = g_cr.total > 0 ? (int)(g_cr.done * 100 / g_cr.total) : 0;
        char t[200]; snprintf(t, sizeof(t), "Получаю моды от хоста: %d%% (%s из %s), файл %d из %d", pct, Mb(g_cr.done).c_str(), Mb(g_cr.total).c_str(),
                              g_cr.cur >= 0 ? g_cr.cur + 1 : g_cr.ok + g_cr.fail, (int)g_cr.files.size());
        std::lock_guard<std::mutex> l(g_mx);
        g_lines = { { t, 1 } };
    }
    else if(g_hs.active)
    {
        int pct = g_hs.total > 0 ? (int)(g_hs.sent * 100 / g_hs.total) : 0;
        char t[200]; snprintf(t, sizeof(t), "Отправляю моды другу: %d%% (%s из %s)", pct, Mb(g_hs.sent).c_str(), Mb(g_hs.total).c_str());
        std::lock_guard<std::mutex> l(g_mx);
        g_lines = { { t, 1 } };
    }
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(type == net::PKT_MODS && len >= net::kModsHeaderSize)
    {
        const auto* p = (const net::ModsPacket*)data;
        if(p->len > net::kModsChunkMax || net::kModsHeaderSize + (int)p->len != len || !p->parts || p->part >= p->parts) return;
        if(p->hash == g_peerHash && g_rxHash == 0) return;                  // этот список уже разобран
        if(p->hash != g_rxHash || g_rxParts.size() != p->parts) { g_rxHash = p->hash; g_rxParts.assign(p->parts, std::string()); g_rxHave.assign(p->parts, false); }
        g_rxParts[p->part].assign((const char*)p->data, p->len); g_rxHave[p->part] = true;
        for(bool h : g_rxHave) if(!h) return;
        std::string all; for(auto& s : g_rxParts) all += s;
        g_peerHash = g_rxHash; g_rxHash = 0;
        ParsePeer(all);
        g_peerReady = true; g_peerKnown = true;
        return;
    }
    if(type == net::PKT_MODFILE_REQ && len >= net::kModFileReqHeaderSize && g_role == net::Role::Host)
    {
        const auto* r = (const net::ModFileReqPacket*)data;
        if(r->count > net::kModFileReqMax || net::kModFileReqHeaderSize + r->count * 2 != len) return;
        if(g_hs.active && g_hs.req == r->req) return;                        // уже отправляем
        if(r->listHash != g_mineHash) { Log(false, "Друг просит моды по старому списку — пошлю новый список"); g_nextSendUs = 0; return; }
        HostStop();
        g_hs.active = true; g_hs.req = r->req;
        for(int i = 0; i < r->count; ++i)
        {
            int idx = r->idx[i];
            if(idx < 0 || idx >= (int)g_mine.size()) continue;
            g_hs.files.push_back(g_mine[idx]); g_hs.total += g_mine[idx].size;
        }
        Log(false, "Друг просит моды: файлов %d, %s", (int)g_hs.files.size(), Mb(g_hs.total).c_str());
        if(g_hs.files.empty()) HostStop();
        return;
    }
    if(type == net::PKT_MODFILE_ACK && len == (int)sizeof(net::ModFileAckPacket))
    {
        const auto* a = (const net::ModFileAckPacket*)data;
        if(!g_hs.active || a->req != g_hs.req) return;
        while(!g_hs.inflight.empty() && (int32_t)(a->seq - g_hs.inflight.front().seq) >= 0) g_hs.inflight.pop_front();
        return;
    }
    if(type == net::PKT_MODFILE_CHUNK && len >= net::kModFileChunkHeaderSize)
    {
        const auto* p = (const net::ModFileChunkPacket*)data;
        if(p->len > net::kModFileChunkMax || net::kModFileChunkHeaderSize + (int)p->len != len) return;
        OnChunk(p);
    }
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

Actions GetActions()
{
    Actions a;
    if(g_role == net::Role::Client && g_peerKnown && !g_cr.active && !g_installIdx.empty())
    {
        a.canInstall = true;
        a.installLabel = "Установить от хоста (" + std::to_string(g_installIdx.size()) + ", " + Mb(g_installBytes) + ")";
    }
    a.canRestore = Exists(ManifestPath());
    a.hostCanSkip = g_role == net::Role::Host && g_peerKnown && !g_matched;
    a.skipping = g_skip;
    return a;
}

void StartInstall()
{
    if(g_role != net::Role::Client || g_cr.active || g_installIdx.empty()) return;
    ClientStop(true);
    g_cr.active = true; g_cr.req = (uint32_t)(rand() | 1);
    std::vector<int> req;                                   // номера в списке хоста — в том же порядке, что g_cr.files
    for(int idx : g_installIdx)
    {
        if((int)req.size() >= net::kModFileReqMax) break;  // остальное — следующим нажатием
        if(idx < 0 || idx >= (int)g_peer.size()) continue;
        req.push_back(idx); g_cr.files.push_back(g_peer[idx]); g_cr.total += g_peer[idx].size;
    }
    g_installIdx = req;
    Log(false, "Установка модов от хоста: файлов %d, %s", (int)g_cr.files.size(), Mb(g_cr.total).c_str());
    g_cr.lastReqUs = 0;
}

void RestoreNow() { Restore("кнопка «Вернуть мои моды»"); }

void SetSkip(bool on) { g_skip = on; Log(false, on ? "Хост: «Играть без сверки модов»" : "Хост: сверка модов снова нужна для «Запуска»"); }

bool LaunchAllowed() { return g_role != net::Role::Host || g_skip || g_matched; }
}
