#include "coop/logshare.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <string>
#include <thread>

#include "net/protocol.h"
#include "trace/ctrace.h"

namespace logshare
{
namespace
{
    net::Session* g_session = nullptr;

    // ---- друг: отправка ----
    struct Chunk { uint32_t seq; int64_t off; std::string data; uint64_t sentUs; };
    int      g_fd = -1;
    std::atomic<int> g_scan{ 0 };          // 0 — не искали начало, 1 — ищем (фоном), 2 — готово
    std::atomic<int64_t> g_startOff{ 0 };
    int64_t  g_off = 0;                    // следующий байт файла к отправке
    uint32_t g_run = 0, g_nextSeq = 0;
    std::deque<Chunk> g_inflight;
    bool     g_quiet = false; uint64_t g_quietTickUs = 0;
    bool     g_wasConnected = false;
    uint64_t g_nextTickUs = 0;

    void Send(const Chunk& c)
    {
        if(!g_session) return;
        net::LogChunkPacket p{};
        p.hdr = net::MakeHeader(net::PKT_LOG_CHUNK);
        p.run = g_run; p.seq = c.seq; p.len = (uint16_t)c.data.size();
        memcpy(p.data, c.data.data(), c.data.size());
        g_session->SendGame(&p, net::kLogChunkHeaderSize + (int)c.data.size());
    }

    // Откуда слать: начало этого запуска игры (последняя строка «ЗАПУСК ИГРЫ»), а если прошлый запуск кончился
    // вылетом — с начала прошлого. Файл до 12 МБ — читаем фоном, чтобы не дёргать игру.
    void ScanStart(std::string path)
    {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        int64_t last = 0, prev = -1, crashAfterPrev = 0;
        if(fd >= 0)
        {
            static const char kSep[] = "=============== ЗАПУСК ИГРЫ";
            const size_t sl = sizeof(kSep) - 1;
            std::string buf; char tmp[65536]; int64_t base = 0; ssize_t n;
            bool crashInLast = false;
            while((n = read(fd, tmp, sizeof(tmp))) > 0)
            {
                buf.append(tmp, (size_t)n);
                size_t pos = 0, nl;
                while((nl = buf.find('\n', pos)) != std::string::npos)
                {
                    if(buf.compare(pos, sl, kSep) == 0) { prev = last; crashAfterPrev = crashInLast; last = base + (int64_t)pos; crashInLast = false; }
                    else if(buf.find("[ВЫЛЕТ]", pos) < nl) crashInLast = true;
                    pos = nl + 1;
                }
                base += (int64_t)pos; buf.erase(0, pos);
            }
            close(fd);
        }
        g_startOff = (prev >= 0 && crashAfterPrev) ? prev : last;
        g_scan = 2;
    }

    void ClientTick(bool connected, uint64_t now)
    {
        if(!connected)
        {
            if(g_wasConnected && !g_inflight.empty()) { g_off = g_inflight.front().off; g_inflight.clear(); }   // продолжим отсюда
            g_wasConnected = false;
            return;
        }
        if(g_scan == 0)
        {
            const char* p = ctrace::Path();
            if(!p || !p[0]) return;
            g_fd = open(p, O_RDONLY | O_CLOEXEC);
            if(g_fd < 0) return;
            g_scan = 1;
            std::thread(ScanStart, std::string(p)).detach();
            return;
        }
        if(g_scan == 1) return;
        if(!g_wasConnected)
        {   // новое подключение — новая передача (у хоста счёт с нуля)
            if(g_run == 0) { g_off = g_startOff; CTRACE("ЛОГ", "лог этого телефона уходит хосту (с байта %lld)", (long long)g_off); }
            g_wasConnected = true;
            g_run = (uint32_t)((now ^ (uint64_t)rand()) | 1); g_nextSeq = 0;
        }
        if(now < g_nextTickUs) return;
        g_nextTickUs = now + 100000;
        // 1.6.6: в игре лог идёт тихо (≈5 КБ/с, 4 куска в пути), в меню — быстро (≈60 КБ/с): связь через сервер Tailscale
        // медленная, большой поток лога мешал игровым пакетам (задержки между игроками). Повтор — только первых кусков.
        if(!g_inflight.empty() && net::Since(now, g_inflight.front().sentUs) > 1200000)
        {
            int n = 0;
            for(auto& c : g_inflight) { if(n++ >= (g_quiet ? 2 : 8)) break; Send(c); }
            for(auto& c : g_inflight) c.sentUs = now;
        }
        if(g_quiet && net::Since(now, g_quietTickUs) < 200000) return;
        g_quietTickUs = now;
        for(int k = 0; k < (g_quiet ? 1 : 6) && (int)g_inflight.size() < (g_quiet ? 4 : 32); ++k)
        {
            char buf[net::kLogChunkMax];
            ssize_t n = pread(g_fd, buf, sizeof(buf), (off_t)g_off);
            if(n <= 0) break;
            ssize_t cut = n;
            while(cut > 0 && buf[cut - 1] != '\n') --cut;          // только целые строки
            if(cut == 0) { if(n < (ssize_t)sizeof(buf)) break; cut = n; }   // строка не дописана — подождать
            Chunk c{ g_nextSeq++, g_off, std::string(buf, (size_t)cut), now };
            g_off += cut;
            Send(c);
            g_inflight.push_back(std::move(c));
        }
    }

    // ---- хост: приём ----
    uint32_t g_hostRun = 0, g_expect = 0;
    bool     g_hostHaveRun = false;
}

void SetSession(net::Session* s) { g_session = s; }

void Frame(net::Role role, bool connected, uint64_t now, bool inGame)
{
    g_quiet = inGame;
    if(role == net::Role::Client) ClientTick(connected, now);
}

void OnPacket(uint8_t type, const uint8_t* data, int len)
{
    if(type == net::PKT_LOG_ACK && len == (int)sizeof(net::LogAckPacket))
    {
        const auto* a = (const net::LogAckPacket*)data;
        if(a->run != g_run) return;
        while(!g_inflight.empty() && (int32_t)(a->seq - g_inflight.front().seq) >= 0) g_inflight.pop_front();
        return;
    }
    if(type != net::PKT_LOG_CHUNK || len < net::kLogChunkHeaderSize) return;
    const auto* p = (const net::LogChunkPacket*)data;
    if(p->len > net::kLogChunkMax || net::kLogChunkHeaderSize + (int)p->len != len) return;
    if(!g_hostHaveRun || p->run != g_hostRun)
    {
        g_hostHaveRun = true; g_hostRun = p->run; g_expect = 0;
        ctrace::FriendHeader(g_session ? g_session->PeerNick() : "");
    }
    if(p->seq == g_expect) { ctrace::FriendRaw((const char*)p->data, p->len); ++g_expect; }
    if(g_expect && g_session)
    {   // подтверждаем всё принятое по порядку (дубли и «забежавшие вперёд» — тоже: друг повторит с нужного)
        net::LogAckPacket a{ net::MakeHeader(net::PKT_LOG_ACK), g_hostRun, g_expect - 1 };
        g_session->SendGame(&a, sizeof(a));
    }
}
}
