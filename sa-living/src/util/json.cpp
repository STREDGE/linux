#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace json
{

static const Value kNull;

const Value& Value::operator[](const std::string& k) const
{
    if(m_type != Type::Object) return kNull;
    auto it = m_obj.find(k);
    return it == m_obj.end() ? kNull : m_items[it->second].second;
}

Value& Value::operator[](const std::string& k)
{
    if(m_type != Type::Object) { *this = Object(); }
    auto it = m_obj.find(k);
    if(it != m_obj.end()) return m_items[it->second].second;
    m_obj[k] = m_items.size();
    m_items.emplace_back(k, Value());
    return m_items.back().second;
}

void Value::erase(const std::string& k)
{
    if(m_type != Type::Object) return;
    auto it = m_obj.find(k);
    if(it == m_obj.end()) return;
    m_items.erase(m_items.begin() + it->second);
    m_obj.clear();
    for(size_t i = 0; i < m_items.size(); ++i) m_obj[m_items[i].first] = i;
}

const Value& Value::operator[](size_t i) const
{
    if(m_type != Type::Array || i >= m_arr.size()) return kNull;
    return m_arr[i];
}

Value& Value::push(const Value& v)
{
    if(m_type != Type::Array) { *this = Array(); }
    m_arr.push_back(v);
    return m_arr.back();
}

void Escape(const std::string& s, std::string& out)
{
    out += '"';
    for(unsigned char c : s)
    {
        switch(c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out) const
{
    switch(m_type)
    {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number:
        {
            char b[32];
            if(isfinite(m_num) && m_num == floor(m_num) && fabs(m_num) < 1e15) snprintf(b, sizeof(b), "%lld", (long long)m_num);
            else if(isfinite(m_num)) snprintf(b, sizeof(b), "%.6g", m_num);
            else snprintf(b, sizeof(b), "0");
            out += b; break;
        }
        case Type::String: Escape(m_str, out); break;
        case Type::Array:
            out += '[';
            for(size_t i = 0; i < m_arr.size(); ++i) { if(i) out += ','; m_arr[i].dumpTo(out); }
            out += ']'; break;
        case Type::Object:
            out += '{';
            for(size_t i = 0; i < m_items.size(); ++i)
            {
                if(i) out += ',';
                Escape(m_items[i].first, out); out += ':'; m_items[i].second.dumpTo(out);
            }
            out += '}'; break;
    }
}

std::string Value::dump() const { std::string s; dumpTo(s); return s; }

// ---------------- разбор ----------------
namespace
{
    struct Parser
    {
        const char* p; const char* e; std::string err;
        void ws() { while(p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
        bool fail(const char* m) { if(err.empty()) err = m; return false; }

        static void put(std::string& o, uint32_t cp)
        {
            if(cp < 0x80) o += (char)cp;
            else if(cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else if(cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        }
        bool hex4(uint32_t& v)
        {
            if(e - p < 4) return false;
            v = 0;
            for(int i = 0; i < 4; ++i)
            {
                char c = p[i]; v <<= 4;
                if(c >= '0' && c <= '9') v |= c - '0'; else if(c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                else if(c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return false;
            }
            p += 4; return true;
        }
        bool str(std::string& o)
        {
            ++p; // "
            while(p < e && *p != '"')
            {
                if(*p == '\\')
                {
                    ++p; if(p >= e) return fail("строка оборвана");
                    char c = *p++;
                    switch(c)
                    {
                        case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
                        case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                        case 'u':
                        {
                            uint32_t cp; if(!hex4(cp)) return fail("\\u");
                            if(cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u')
                            { p += 2; uint32_t lo; if(!hex4(lo)) return fail("\\u"); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
                            put(o, cp); break;
                        }
                        default: o += c;
                    }
                }
                else o += *p++;
            }
            if(p >= e) return fail("нет закрывающей кавычки");
            ++p; return true;
        }
        bool val(Value& v, int depth)
        {
            if(depth > 64) return fail("слишком глубоко");
            ws(); if(p >= e) return fail("пусто");
            char c = *p;
            if(c == '{')
            {
                ++p; v = Value::Object(); ws();
                if(p < e && *p == '}') { ++p; return true; }
                for(;;)
                {
                    ws(); if(p >= e || *p != '"') return fail("ожидался ключ");
                    std::string k; if(!str(k)) return false;
                    ws(); if(p >= e || *p != ':') return fail("ожидалось :"); ++p;
                    Value item; if(!val(item, depth + 1)) return false;
                    v[k] = std::move(item);
                    ws(); if(p < e && *p == ',') { ++p; continue; }
                    if(p < e && *p == '}') { ++p; return true; }
                    return fail("ожидалось , или }");
                }
            }
            if(c == '[')
            {
                ++p; v = Value::Array(); ws();
                if(p < e && *p == ']') { ++p; return true; }
                for(;;)
                {
                    Value item; if(!val(item, depth + 1)) return false;
                    v.push(item);
                    ws(); if(p < e && *p == ',') { ++p; continue; }
                    if(p < e && *p == ']') { ++p; return true; }
                    return fail("ожидалось , или ]");
                }
            }
            if(c == '"') { std::string s; if(!str(s)) return false; v = Value(s); return true; }
            if(!strncmp(p, "true", 4) && e - p >= 4) { p += 4; v = Value(true); return true; }
            if(!strncmp(p, "false", 5) && e - p >= 5) { p += 5; v = Value(false); return true; }
            if(!strncmp(p, "null", 4) && e - p >= 4) { p += 4; v = Value(); return true; }
            if(c == '-' || (c >= '0' && c <= '9'))
            {
                char buf[64]; size_t n = 0;
                while(p < e && n < 63 && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) buf[n++] = *p++;
                buf[n] = 0; v = Value(strtod(buf, nullptr)); return true;
            }
            return fail("неизвестный символ");
        }
    };
}

bool Parse(const std::string& text, Value& out, std::string* err)
{
    Parser ps{ text.data(), text.data() + text.size(), "" };
    Value v;
    bool ok = ps.val(v, 0);
    if(ok) { ps.ws(); if(ps.p != ps.e) { ok = false; ps.err = "лишнее после значения"; } }
    if(ok) out = std::move(v);
    else if(err) *err = ps.err;
    return ok;
}

} // namespace json
