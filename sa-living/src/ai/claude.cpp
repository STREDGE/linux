#include "claude.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <time.h>

#include <map>

#include "net/link.h"
#include "platform/bridge.h"
#include "settings.h"
#include "util/log.h"
#include "trace/trace.h"

namespace claude
{
namespace
{
    Settings g_s;

    struct Block { std::string type, id, name, text, partialJson; };
    struct Pending
    {
        TextFn onText; DoneFn onDone;
        std::string tag;
        std::string event;
        std::vector<Block> blocks;
        Result r;
        bool cancelled = false;
        bool gotStop = false;
        double sentAt = 0;
        bool warm = false;
        std::string model;            // для цены и лога
        std::string body;             // тело (только с fallbacks — на случай повтора без них)
        int  timeoutMs = 45000;
    };
    bool g_noFallbacks = false;       // сервер не принял fallbacks — дальше без них
    std::map<int, int> g_alias;       // номер для вызывающего ← номер повторного запроса в мосту
    std::map<int, Pending> g_pending;
    struct Proxied { TextFn onText; DoneFn onDone; std::string tag; double at = 0; };
    double Mono() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
    std::map<int, Proxied> g_proxied;
    int g_nextProxy = 1000000;

    Usage g_today, g_total;
    int   g_day = -1;
    int   g_dailyLimit = 400;

    int Today_()
    {
        time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
        return tm.tm_year * 400 + tm.tm_yday;
    }
    void RollDay() { int d = Today_(); if(d != g_day) { g_day = d; g_today = Usage(); } }

    // Цена за 1 млн токенов: вход, выход, чтение кэша, запись кэша (примерно — для счётчика в меню)
    void Price(const std::string& model, double& in, double& out, double& cr, double& cw)
    {
        if(model.find("sonnet") != std::string::npos) { in = 2; out = 10; cr = 0.10; cw = 2.5; }
        else                                           { in = 0.10; out = 0.50; cr = 0.01; cw = 0.125; }
    }
    void AddUsage(const Result& r, bool err, const std::string& model)
    {
        RollDay();
        double pi, po, pr, pw; Price(model, pi, po, pr, pw);
        double usd = (r.inTok * pi + r.outTok * po + r.cacheRead * pr + r.cacheWrite * pw) / 1e6;
        for(Usage* u : { &g_today, &g_total })
        {
            u->requests++; if(err) u->errors++;
            u->inTok += r.inTok; u->outTok += r.outTok; u->cacheRead += r.cacheRead; u->cacheWrite += r.cacheWrite;
            u->usd += usd;
        }
    }

    std::string HumanError(int status, const std::string& body)
    {
        json::Value v; std::string msg;
        if(json::Parse(body, v)) msg = v["error"]["message"].str();
        switch(status)
        {
            case 401: return "Ключ не подходит (401). Проверь ключ в разделе «ИИ».";
            case 403: return "Нет доступа (403): " + msg;
            case 404: return "Модель не найдена (404): " + msg;
            case 429: return "Слишком много запросов (429) — подожди немного.";
            case 400:
                if(msg.find("credit") != std::string::npos) return "На счёте API нет денег (400). Забери кредиты подписки: claude.ai > Settings > Billing > API credits.";
                return "Ошибка запроса (400): " + msg;
            case 529: return "Сервис перегружен (529) — повторю позже.";
            default:
                if(status <= 0) return "Нет связи: " + body;
                if(status >= 500) return "Сервис недоступен (" + std::to_string(status) + ")";
                return "Ошибка " + std::to_string(status) + ": " + msg;
        }
    }

    void Finish(int id, Pending& p)
    {
        Result& r = p.r;
        // Собираем блоки ответа
        r.assistantContent = json::Value::Array();
        for(auto& b : p.blocks)
        {
            if(b.type == "text")
            {
                r.text += b.text;
                json::Value t = json::Value::Object(); t["type"] = "text"; t["text"] = b.text;
                if(!b.text.empty()) r.assistantContent.push(t);
            }
            else if(b.type == "tool_use")
            {
                ToolCall c; c.id = b.id; c.name = b.name;
                std::string src = b.partialJson.empty() ? "{}" : b.partialJson;
                c.inputOk = json::Parse(src, c.input) && c.input.isObject();
                if(!c.inputOk) c.input = json::Value::Object();
                json::Value t = json::Value::Object(); t["type"] = "tool_use"; t["id"] = c.id; t["name"] = c.name; t["input"] = c.input;
                r.assistantContent.push(t);
                r.tools.push_back(std::move(c));
            }
            // thinking-блоки (если вернутся) в историю не кладём: каждый ход — новый запрос
        }
        if(r.status == 200 && r.stopReason == "refusal") { r.ok = false; r.error = "Модель отказалась отвечать"; }
        AddUsage(r, !r.ok, p.model);
        LOG_INFO("[ИИ] %s: %s, ток. вход %d (кэш %d/%d), выход %d, инстр. %d, стоп %s",
                 p.tag.c_str(), r.ok ? "ok" : r.error.c_str(), r.inTok, r.cacheRead, r.cacheWrite, r.outTok,
                 (int)r.tools.size(), r.stopReason.c_str());
        {
            std::string tools;
            for(auto& t : r.tools) { std::string in = t.input.dump(); if(in.size() > 600) in = in.substr(0, 600) + "…"; tools += " " + t.name + in; }
            TRACE("ИИ<", "#%d %s [%s]: %s за %.2f с, вход %d (кэш %d/%d), выход %d, стоп %s%s | текст: %s | инструменты:%s", id, p.tag.c_str(),
                  p.model.c_str(),
                  r.ok ? "ok" : r.error.c_str(), p.sentAt > 0 ? Mono() - p.sentAt : 0.0, r.inTok, r.cacheRead, r.cacheWrite, r.outTok,
                  r.stopReason.c_str(), p.cancelled ? " (отменён)" : "", r.text.c_str(), tools.empty() ? " нет" : tools.c_str());
        }
        if(!p.cancelled && p.onDone) p.onDone(r);
    }

    void OnLine(Pending& p, const std::string& line)
    {
        if(line.compare(0, 6, "event:") == 0) { p.event = line.substr(line[6] == ' ' ? 7 : 6); return; }
        if(line.compare(0, 5, "data:") != 0) return;
        std::string data = line.substr(line.size() > 5 && line[5] == ' ' ? 6 : 5);
        json::Value v;
        if(!json::Parse(data, v)) return;
        const std::string& t = v["type"].str();
        if(t == "message_start")
        {
            const json::Value& u = v["message"]["usage"];
            p.r.inTok = u["input_tokens"].i(); p.r.cacheRead = u["cache_read_input_tokens"].i();
            p.r.cacheWrite = u["cache_creation_input_tokens"].i(); p.r.outTok = u["output_tokens"].i();
        }
        else if(t == "content_block_start")
        {
            size_t idx = (size_t)v["index"].i();
            if(p.blocks.size() <= idx) p.blocks.resize(idx + 1);
            const json::Value& cb = v["content_block"];
            Block& b = p.blocks[idx];
            b.type = cb["type"].str(); b.id = cb["id"].str(); b.name = cb["name"].str(); b.text = cb["text"].str();
        }
        else if(t == "content_block_delta")
        {
            size_t idx = (size_t)v["index"].i();
            if(p.blocks.size() <= idx) p.blocks.resize(idx + 1);
            const json::Value& d = v["delta"];
            const std::string& dt = d["type"].str();
            if(dt == "text_delta")
            {
                const std::string& s = d["text"].str();
                p.blocks[idx].text += s;
                if(!p.cancelled && p.onText) p.onText(s);
            }
            else if(dt == "input_json_delta") p.blocks[idx].partialJson += d["partial_json"].str();
        }
        else if(t == "message_delta")
        {
            p.r.stopReason = v["delta"]["stop_reason"].s();
            if(v["usage"].has("output_tokens")) p.r.outTok = v["usage"]["output_tokens"].i();
        }
        else if(t == "message_stop") p.gotStop = true;
        else if(t == "error") { p.r.error = "Ошибка сервиса: " + v["error"]["message"].s(); }
    }
}

void Configure(const Settings& s) { g_s = s; }
const Settings& Get() { return g_s; }
bool HasKey() { return g_s.key.size() > 10; }

std::string MaskedKey()
{
    if(!HasKey()) return "нет";
    const std::string& k = g_s.key;
    return k.substr(0, 7) + "…" + k.substr(k.size() - 4);
}

bool ViaHost() { return !HasKey() && coopnet::GetRole() == coopnet::CLIENT; }
bool Available() { return HasKey() || ViaHost(); }

static json::Value ReqToJ(const Request& r)
{
    json::Value v = json::Value::Object();
    v["system"] = r.system; v["tools"] = r.tools; v["messages"] = r.messages; v["maxTokens"] = r.maxTokens;
    v["effort"] = r.effort; v["format"] = r.format; v["cache"] = r.cache; v["tag"] = r.tag; v["noThinking"] = r.noThinking; v["role"] = r.role;
    return v;
}

int Send(const Request& req, TextFn onText, DoneFn onDone)
{
    if(ViaHost())
    {
        int id = g_nextProxy++;
        json::Value m = json::Value::Object(); m["t"] = "ai"; m["rid"] = id; m["req"] = ReqToJ(req);
        if(!coopnet::Send(m)) return 0;
        g_proxied[id] = Proxied{ onText, onDone, req.tag, Mono() };
        return id;
    }
    if(!HasKey() || !bridge::Ready()) return 0;
    RollDay();
    if(LimitReached()) return 0;

    json::Value body = json::Value::Object();
    std::string model = !req.model.empty() ? req.model : ModelFor(req.role);
    bool sonnet = model.find("sonnet") != std::string::npos;
    body["model"] = model;
    body["max_tokens"] = req.warmOnly ? 0 : req.maxTokens;
    body["stream"] = !req.warmOnly;       // max_tokens 0 с потоком API отклоняет
    if(req.system.size()) body["system"] = req.system;
    if(req.tools.size()) body["tools"] = req.tools;
    body["messages"] = req.messages;
    json::Value oc = json::Value::Object();
    std::string eff = req.effort.empty() ? g_s.effort : req.effort;
    oc["effort"] = eff;
    if(req.noThinking && (eff == "low" || eff == "medium" || eff == "high"))
    {   // «без размышления»: Haiku — disabled, Sonnet 5.5 — between_tools (disabled у него — ошибка 400)
        json::Value th = json::Value::Object();
        th["type"] = sonnet ? "between_tools" : "disabled"; body["thinking"] = th;
    }
    if(!req.format.isNull()) oc["format"] = req.format;
    body["output_config"] = oc;
    if(req.cache) { json::Value cc = json::Value::Object(); cc["type"] = "ephemeral"; body["cache_control"] = cc; }
    // Отказ фильтров Sonnet 5.5 — сервер сам повторит на запасной модели (у Haiku запасной нет)
    bool fallbacks = sonnet && !req.warmOnly && !g_noFallbacks;
    if(fallbacks) body["fallbacks"] = "default";

    std::vector<std::string> headers = {
        "content-type", "application/json",
        "x-api-key", g_s.key,
        "anthropic-version", g_s.apiVersion,
        "accept", "text/event-stream",
    };
    if(fallbacks) { headers.push_back("anthropic-beta"); headers.push_back("server-side-fallback-2026-07-01"); }
    int id = bridge::HttpPost(g_s.url.c_str(), headers, body.dump(), req.timeoutMs);
    if(!id) return 0;
    Pending& p = g_pending[id];
    p.onText = onText; p.onDone = onDone; p.tag = req.tag; p.sentAt = Mono(); p.warm = req.warmOnly;
    p.model = model; p.timeoutMs = req.timeoutMs;
    if(fallbacks) { json::Value b2 = body; b2.erase("fallbacks"); p.body = b2.dump(); }
    {   // в подробный лог: что спросили (последнее сообщение — обстановка и слова игрока / результаты инструментов)
        std::string last;
        if(req.messages.isArray() && req.messages.size())
        {
            const json::Value& m = req.messages[req.messages.size() - 1];
            last = m["content"].isString() ? m["content"].str() : m["content"].dump();
        }
        if(last.size() > 3000) last = last.substr(0, 3000) + "…";
        TRACE("ИИ>", "#%d %s [%s]: режим %s%s, сообщений %d, последнее: %s", id, req.tag, model.c_str(), (req.effort.empty() ? g_s.effort : req.effort).c_str(),
              req.noThinking ? " без размышления" : "", req.messages.isArray() ? (int)req.messages.size() : 0, last.c_str());
    }
    return id;
}

void Cancel(int id)
{
    auto it = g_pending.find(id); if(it != g_pending.end()) it->second.cancelled = true;
    for(auto& kv : g_alias) if(kv.second == id) { auto j = g_pending.find(kv.first); if(j != g_pending.end()) j->second.cancelled = true; }
    auto pt = g_proxied.find(id); if(pt != g_proxied.end()) { pt->second.onText = nullptr; pt->second.onDone = nullptr; }
}
int  InFlight() { return (int)(g_pending.size() + g_proxied.size()); }

// Загрузка другой игры / новая игра: ответы на старые запросы не должны попасть в новую память.
// Ждущим сообщаем об ошибке (чтобы сняли свои «жду ответа»), сами ответы потом молча отбрасываются.
void AbortAll(const char* why)
{
    std::vector<DoneFn> fns;
    for(auto& kv : g_pending)
    {
        if(!kv.second.cancelled && kv.second.onDone) fns.push_back(kv.second.onDone);
        kv.second.cancelled = true; kv.second.onText = nullptr; kv.second.onDone = nullptr;
    }
    for(auto& kv : g_proxied) if(kv.second.onDone) fns.push_back(kv.second.onDone);
    g_proxied.clear();
    Result r; r.error = why ? why : "отменено";
    for(auto& fn : fns) fn(r);
}

void HandleProxyRequest(const json::Value& m)
{
    int rid = m["rid"].i();
    auto reply = [rid](const json::Value& body) { json::Value r = body; r["t"] = "ai_res"; r["rid"] = rid; coopnet::Send(r); };
    if(!settings::Get().shareKeyWithFriend || !HasKey())
    {
        json::Value e = json::Value::Object(); e["ok"] = false;
        e["error"] = HasKey() ? "Хост запретил запросы через свой ключ" : "У хоста нет ключа ИИ";
        reply(e); return;
    }
    const json::Value& j = m["req"];
    Request r;
    r.system = j["system"]; r.tools = j["tools"]; r.messages = j["messages"]; r.maxTokens = j["maxTokens"].i(600);
    r.effort = j["effort"].str(); r.format = j["format"]; r.cache = j["cache"].b(true); r.noThinking = j["noThinking"].b(false); r.role = j["role"].i(ROLE_TALK);
    static std::string tagBuf; tagBuf = "друг: " + j["tag"].str(); r.tag = tagBuf.c_str();
    int id = Send(r, nullptr, [reply](Result& res) {
        json::Value o = json::Value::Object();
        o["ok"] = res.ok; o["error"] = res.error; o["text"] = res.text; o["stop"] = res.stopReason; o["content"] = res.assistantContent;
        reply(o);
    });
    if(!id) { json::Value e = json::Value::Object(); e["ok"] = false; e["error"] = LimitReached() ? "У хоста кончился дневной лимит ИИ" : "ИИ у хоста недоступен"; reply(e); }
}

void HandleProxyResult(const json::Value& m)
{
    auto it = g_proxied.find(m["rid"].i());
    if(it == g_proxied.end()) return;
    Proxied p = it->second; g_proxied.erase(it);
    Result r;
    r.ok = m["ok"].b(); r.error = m["error"].str(); r.text = m["text"].str(); r.stopReason = m["stop"].str(); r.status = r.ok ? 200 : 0;
    r.assistantContent = m["content"].isArray() ? m["content"] : json::Value::Array();
    for(auto& b : r.assistantContent.arr())
        if(b["type"].str() == "tool_use") { ToolCall c; c.id = b["id"].str(); c.name = b["name"].str(); c.input = b["input"]; c.inputOk = c.input.isObject(); r.tools.push_back(c); }
    if(p.onText && !r.text.empty()) p.onText(r.text);
    if(p.onDone) p.onDone(r);
}

void Pump()
{
    // друг в коопе: хост не ответил за минуту (отключился) — не ждать вечно
    if(!g_proxied.empty())
    {
        double now = Mono();
        std::vector<DoneFn> late;
        for(auto it = g_proxied.begin(); it != g_proxied.end();)
            if(now - it->second.at > 60) { if(it->second.onDone) late.push_back(it->second.onDone); it = g_proxied.erase(it); }
            else ++it;
        if(!late.empty()) { Result r; r.error = "Хост не ответил"; for(auto& fn : late) fn(r); }
    }
    bridge::HttpEvent e;
    int guard = 0;
    while(guard++ < 2000 && bridge::PollHttp(e))
    {
        auto it = g_pending.find(e.id);
        if(it == g_pending.end()) continue;
        Pending& p = it->second;
        if(!e.done) { if(!p.warm) OnLine(p, e.text); continue; }
        if(p.warm)
        {   // прогрев: ответа нет, нужен только код
            TRACE("ИИ<", "#%d %s: HTTP %d за %.2f с", e.id, p.tag.c_str(), e.status, Mono() - p.sentAt);
            if(e.status != 200) LOG_ERROR("[ИИ] прогрев кэша: HTTP %d %s", e.status, HumanError(e.status, e.text).c_str());
            g_pending.erase(it);
            continue;
        }
        if(e.status == 400 && !p.body.empty() && e.text.find("allback") != std::string::npos)
        {   // параметр fallbacks не приняли — повторяем тот же запрос без него (номер для вызывающего тот же)
            g_noFallbacks = true;
            LOG_ERROR("[ИИ] сервер не принял fallbacks — дальше без запасной модели");
            std::vector<std::string> headers = { "content-type", "application/json", "x-api-key", g_s.key, "anthropic-version", g_s.apiVersion, "accept", "text/event-stream" };
            int nid = bridge::HttpPost(g_s.url.c_str(), headers, p.body, p.timeoutMs);
            if(nid)
            {
                int pub = e.id; auto al = g_alias.find(e.id); if(al != g_alias.end()) { pub = al->second; g_alias.erase(al); }
                Pending np = std::move(p); g_pending.erase(it);
                np.body.clear(); np.blocks.clear(); np.r = Result(); np.gotStop = false; np.event.clear();
                g_alias[nid] = pub;
                g_pending[nid] = std::move(np);
                continue;
            }
        }
        p.r.status = e.status;
        if(e.status == 200 && p.gotStop && p.r.error.empty()) p.r.ok = true;
        else if(e.status == 200 && p.r.error.empty()) { p.r.ok = !p.blocks.empty(); if(!p.r.ok) p.r.error = "Ответ оборвался"; }
        else if(p.r.error.empty()) p.r.error = HumanError(e.status, e.text);
        if(!p.r.ok) LOG_ERROR("[ИИ] %s: HTTP %d %s", p.tag.c_str(), e.status, p.r.error.c_str());
        Pending done = std::move(p);
        g_pending.erase(it);
        int pub = e.id; { auto al = g_alias.find(e.id); if(al != g_alias.end()) { pub = al->second; g_alias.erase(al); } }
        Finish(pub, done);
    }
}

Usage Today() { RollDay(); return g_today; }
Usage Total() { return g_total; }

double CostUsd(const Usage& u) { return u.usd; }

// Уровень 1 (по умолчанию): разговоры, Джарвис и мир — Sonnet 5.5 (умнее, живее), а мгновенные реакции на событие
// (ударили, навели ствол, напали) — Haiku 5.5: там важнее скорость, слова короткие. Уровень 0 — всё на Haiku (быстрее всего).
std::string ModelFor(int role)
{
    if(g_s.tier <= 0 || role == ROLE_REACT) return "claude-haiku-5-5";
    return "claude-sonnet-5-5";
}

void SetDailyLimit(int r) { g_dailyLimit = r; }
bool LimitReached() { RollDay(); return g_dailyLimit > 0 && g_today.requests >= g_dailyLimit; }

static json::Value UsageToJson(const Usage& u)
{
    json::Value v = json::Value::Object();
    v["requests"] = u.requests; v["errors"] = u.errors; v["in"] = (double)u.inTok; v["out"] = (double)u.outTok;
    v["cr"] = (double)u.cacheRead; v["cw"] = (double)u.cacheWrite; v["usd"] = u.usd;
    return v;
}
static Usage UsageFromJson(const json::Value& v)
{
    Usage u; u.requests = v["requests"].i(); u.errors = v["errors"].i(); u.inTok = (long long)v["in"].num();
    u.outTok = (long long)v["out"].num(); u.cacheRead = (long long)v["cr"].num(); u.cacheWrite = (long long)v["cw"].num();
    // старые записи без цены — по ценам Haiku (тогда другой модели не было)
    u.usd = v.has("usd") ? v["usd"].num() : (u.inTok * 0.10 + u.outTok * 0.50 + u.cacheRead * 0.01 + u.cacheWrite * 0.125) / 1e6;
    return u;
}

void LoadUsage(const json::Value& v)
{
    g_total = UsageFromJson(v["total"]);
    g_day = v["day"].i(-1);
    g_today = UsageFromJson(v["today"]);
    RollDay();
}

json::Value SaveUsage()
{
    json::Value v = json::Value::Object();
    v["total"] = UsageToJson(g_total); v["today"] = UsageToJson(g_today); v["day"] = g_day;
    return v;
}

} // namespace claude
