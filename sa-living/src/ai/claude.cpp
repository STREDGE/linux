#include "claude.h"

#include <mod/amlmod.h>
#include <mod/logger.h>
#include <time.h>

#include <map>

#include "net/link.h"
#include "platform/bridge.h"
#include "settings.h"
#include "util/log.h"

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
    };
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

    void AddUsage(const Result& r, bool err)
    {
        RollDay();
        for(Usage* u : { &g_today, &g_total })
        {
            u->requests++; if(err) u->errors++;
            u->inTok += r.inTok; u->outTok += r.outTok; u->cacheRead += r.cacheRead; u->cacheWrite += r.cacheWrite;
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
        AddUsage(r, !r.ok);
        LOG_INFO("[ИИ] %s: %s, ток. вход %d (кэш %d/%d), выход %d, инстр. %d, стоп %s",
                 p.tag.c_str(), r.ok ? "ok" : r.error.c_str(), r.inTok, r.cacheRead, r.cacheWrite, r.outTok,
                 (int)r.tools.size(), r.stopReason.c_str());
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
    v["effort"] = r.effort; v["format"] = r.format; v["cache"] = r.cache; v["tag"] = r.tag; v["noThinking"] = r.noThinking;
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
    body["model"] = g_s.model;
    body["max_tokens"] = req.maxTokens;
    body["stream"] = true;
    if(req.system.size()) body["system"] = req.system;
    if(req.tools.size()) body["tools"] = req.tools;
    body["messages"] = req.messages;
    json::Value oc = json::Value::Object();
    std::string eff = req.effort.empty() ? g_s.effort : req.effort;
    oc["effort"] = eff;
    if(req.noThinking && (eff == "low" || eff == "medium" || eff == "high")) { json::Value th = json::Value::Object(); th["type"] = "disabled"; body["thinking"] = th; }
    if(!req.format.isNull()) oc["format"] = req.format;
    body["output_config"] = oc;
    if(req.cache) { json::Value cc = json::Value::Object(); cc["type"] = "ephemeral"; body["cache_control"] = cc; }

    std::vector<std::string> headers = {
        "content-type", "application/json",
        "x-api-key", g_s.key,
        "anthropic-version", g_s.apiVersion,
        "accept", "text/event-stream",
    };
    int id = bridge::HttpPost(g_s.url.c_str(), headers, body.dump(), req.timeoutMs);
    if(!id) return 0;
    Pending& p = g_pending[id];
    p.onText = onText; p.onDone = onDone; p.tag = req.tag;
    return id;
}

void Cancel(int id)
{
    auto it = g_pending.find(id); if(it != g_pending.end()) it->second.cancelled = true;
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
    r.effort = j["effort"].str(); r.format = j["format"]; r.cache = j["cache"].b(true); r.noThinking = j["noThinking"].b(false);
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
        if(!e.done) { OnLine(p, e.text); continue; }
        p.r.status = e.status;
        if(e.status == 200 && p.gotStop && p.r.error.empty()) p.r.ok = true;
        else if(e.status == 200 && p.r.error.empty()) { p.r.ok = !p.blocks.empty(); if(!p.r.ok) p.r.error = "Ответ оборвался"; }
        else if(p.r.error.empty()) p.r.error = HumanError(e.status, e.text);
        if(!p.r.ok) LOG_ERROR("[ИИ] %s: HTTP %d %s", p.tag.c_str(), e.status, p.r.error.c_str());
        Pending done = std::move(p);
        g_pending.erase(it);
        Finish(e.id, done);
    }
}

Usage Today() { RollDay(); return g_today; }
Usage Total() { return g_total; }

double CostUsd(const Usage& u)
{
    // Haiku 5.5: $0.10 / 1M вход, $0.50 / 1M выход; чтение кэша ~в 10 раз дешевле, запись — ~на 25% дороже
    double in = (double)(u.inTok) * 0.10 + (double)u.cacheRead * 0.01 + (double)u.cacheWrite * 0.125;
    return (in + (double)u.outTok * 0.50) / 1e6;
}

void SetDailyLimit(int r) { g_dailyLimit = r; }
bool LimitReached() { RollDay(); return g_dailyLimit > 0 && g_today.requests >= g_dailyLimit; }

static json::Value UsageToJson(const Usage& u)
{
    json::Value v = json::Value::Object();
    v["requests"] = u.requests; v["errors"] = u.errors; v["in"] = (double)u.inTok; v["out"] = (double)u.outTok;
    v["cr"] = (double)u.cacheRead; v["cw"] = (double)u.cacheWrite;
    return v;
}
static Usage UsageFromJson(const json::Value& v)
{
    Usage u; u.requests = v["requests"].i(); u.errors = v["errors"].i(); u.inTok = (long long)v["in"].num();
    u.outTok = (long long)v["out"].num(); u.cacheRead = (long long)v["cr"].num(); u.cacheWrite = (long long)v["cw"].num();
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
