#include "link.h"

#include <mod/amlmod.h>
#include <mod/interface.h>
#include <string.h>

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "net/sacoop_api.h"
#include "talk/talk.h"
#include "util/log.h"
#include "trace/trace.h"

namespace coopnet
{
// обработчики сообщений — в main.cpp (там же разбор типов)
void OnMessage(const json::Value& msg);

namespace
{
    constexpr uint8_t kChannel = 0x4C;            // 'L'
    constexpr int     kChunk   = 1000;
    SACoopNetApi* g_api = nullptr;
    Role g_lastRole = NONE;

    #pragma pack(push, 1)
    struct Hdr { uint8_t kind; uint32_t msg; uint16_t idx, count; };   // kind 0 — данные, 1 — подтверждение
    #pragma pack(pop)

    struct Out { uint32_t id; std::vector<std::string> chunks; std::vector<bool> acked; double next; int tries; };
    std::deque<Out> g_out;
    uint32_t g_nextId = 1;

    struct In { uint16_t count = 0; std::vector<std::string> parts; std::vector<bool> have; int got = 0; };
    std::map<uint32_t, In> g_in;
    std::deque<uint32_t> g_done;                 // недавно собранные (защита от повторов)
    std::deque<std::string> g_ready;             // собранные сообщения → разбор в Frame

    void SendRaw(const Hdr& h, const char* data, int len)
    {
        if(!g_api) return;
        char buf[sizeof(Hdr) + kChunk];
        memcpy(buf, &h, sizeof(h));
        if(len > 0) memcpy(buf + sizeof(h), data, len);
        g_api->Send(kChannel, buf, (int)sizeof(h) + len);
    }

    void Receive(const void* data, int len)
    {
        if(len < (int)sizeof(Hdr)) return;
        Hdr h; memcpy(&h, data, sizeof(h));
        const char* body = (const char*)data + sizeof(h);
        int blen = len - (int)sizeof(h);
        if(h.kind == 1)
        {
            for(auto& o : g_out) if(o.id == h.msg && h.idx < o.acked.size()) o.acked[h.idx] = true;
            return;
        }
        Hdr ack{ 1, h.msg, h.idx, h.count };
        SendRaw(ack, nullptr, 0);
        for(uint32_t d : g_done) if(d == h.msg) return;
        if(!h.count || h.idx >= h.count || h.count > 200) return;
        In& in = g_in[h.msg];
        if(!in.count) { in.count = h.count; in.parts.resize(h.count); in.have.resize(h.count); }
        if(in.have[h.idx]) return;
        in.have[h.idx] = true; in.parts[h.idx].assign(body, blen); in.got++;
        if(in.got == in.count)
        {
            std::string all; for(auto& p : in.parts) all += p;
            g_ready.push_back(std::move(all));
            g_in.erase(h.msg);
            g_done.push_back(h.msg); if(g_done.size() > 128) g_done.pop_front();
        }
    }
}

void Init()
{
    g_api = (SACoopNetApi*)GetInterface(SACOOP_NET_INTERFACE);
    if(!g_api || g_api->version < 1) { g_api = nullptr; LOG_INFO("Кооп (SA Coop) не найден — только одиночная игра"); return; }
    g_api->SetReceiver(kChannel, Receive);
    LOG_INFO("Кооп найден: при подключении друга мир станет общим автоматически");
}

Role GetRole()
{
    if(!g_api || !g_api->Connected()) return NONE;
    int r = g_api->GetRole();
    return r == 2 ? HOST : (r == 3 ? CLIENT : NONE);
}

bool Connected() { return GetRole() != NONE; }
const char* PeerNick() { return g_api ? g_api->PeerNick() : ""; }
const char* MyNick() { return g_api && g_api->MyNick ? g_api->MyNick() : ""; }

bool Send(const json::Value& msg)
{
    if(!Connected()) { TRACE("КООП>", "не отправлено (нет связи): %.200s", msg.dump().c_str()); return false; }
    std::string s = msg.dump();
    TRACE("КООП>", "%s", s.size() > 1500 ? (s.substr(0, 1500) + "…").c_str() : s.c_str());
    Out o; o.id = g_nextId++; o.next = 0; o.tries = 0;
    for(size_t i = 0; i < s.size(); i += kChunk) o.chunks.push_back(s.substr(i, kChunk));
    if(o.chunks.empty()) o.chunks.push_back("");
    if(o.chunks.size() > 200) { LOG_ERROR("Связь: сообщение слишком большое (%d байт)", (int)s.size()); return false; }
    o.acked.assign(o.chunks.size(), false);
    g_out.push_back(std::move(o));
    return true;
}

void Frame()
{
    Role r = GetRole();
    if(r != g_lastRole)
    {
        g_lastRole = r;
        if(r == HOST) { talk::Notify(std::string("ИИ: общий мир с ") + PeerNick() + " — запросы друга идут через твой ключ"); LOG_INFO("ИИ-кооп: мы хост"); }
        else if(r == CLIENT) { talk::Notify(std::string("ИИ: общий мир хоста ") + PeerNick()); LOG_INFO("ИИ-кооп: мы друг (ИИ через хоста)"); }
        else { LOG_INFO("ИИ-кооп: выключен — одиночная игра"); g_out.clear(); g_in.clear(); }
    }
    double now = talk::Now();
    for(auto it = g_out.begin(); it != g_out.end();)
    {
        bool all = true;
        for(bool a : it->acked) all = all && a;
        if(all || it->tries > 20) { if(!all) LOG_ERROR("Связь: сообщение %u не дошло", it->id); it = g_out.erase(it); continue; }
        if(now >= it->next)
        {
            for(size_t i = 0; i < it->chunks.size(); ++i)
                if(!it->acked[i]) { Hdr h{ 0, it->id, (uint16_t)i, (uint16_t)it->chunks.size() }; SendRaw(h, it->chunks[i].data(), (int)it->chunks[i].size()); }
            it->tries++; it->next = now + 0.35;
        }
        ++it;
    }
    while(!g_ready.empty())
    {
        std::string s = std::move(g_ready.front()); g_ready.pop_front();
        json::Value v;
        if(json::Parse(s, v)) { TRACE("КООП<", "%s", s.size() > 1500 ? (s.substr(0, 1500) + "…").c_str() : s.c_str()); OnMessage(v); } else LOG_ERROR("Связь: битое сообщение");
    }
}

} // namespace coopnet
