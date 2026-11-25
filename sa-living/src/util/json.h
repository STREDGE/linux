#pragma once
// Маленький JSON: разбор и запись. Хватает для Claude API и файла мира.

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace json
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    class Value
    {
    public:
        Value() {}
        Value(std::nullptr_t) {}
        Value(bool b) : m_type(Type::Bool), m_bool(b) {}
        Value(int v) : m_type(Type::Number), m_num(v) {}
        Value(unsigned v) : m_type(Type::Number), m_num(v) {}
        Value(long long v) : m_type(Type::Number), m_num((double)v) {}
        Value(double v) : m_type(Type::Number), m_num(v) {}
        Value(float v) : m_type(Type::Number), m_num(v) {}
        Value(const char* s) : m_type(Type::String), m_str(s ? s : "") {}
        Value(const std::string& s) : m_type(Type::String), m_str(s) {}
        static Value Array()  { Value v; v.m_type = Type::Array; return v; }
        static Value Object() { Value v; v.m_type = Type::Object; return v; }

        Type type() const { return m_type; }
        bool isNull() const   { return m_type == Type::Null; }
        bool isObject() const { return m_type == Type::Object; }
        bool isArray() const  { return m_type == Type::Array; }
        bool isString() const { return m_type == Type::String; }
        bool isNumber() const { return m_type == Type::Number; }
        bool isBool() const   { return m_type == Type::Bool; }

        // чтение с запасным значением
        double num(double def = 0) const { return m_type == Type::Number ? m_num : (m_type == Type::Bool ? (m_bool ? 1 : 0) : def); }
        int    i(int def = 0) const { return m_type == Type::Number ? (int)m_num : def; }
        bool   b(bool def = false) const { return m_type == Type::Bool ? m_bool : (m_type == Type::Number ? m_num != 0 : def); }
        const std::string& str() const { static const std::string e; return m_type == Type::String ? m_str : e; }
        std::string s(const char* def = "") const { return m_type == Type::String ? m_str : std::string(def); }

        // объект
        const Value& operator[](const std::string& k) const;
        Value& operator[](const std::string& k);           // создаёт объект/ключ
        bool has(const std::string& k) const { return m_type == Type::Object && m_obj.count(k); }
        const std::vector<std::pair<std::string, Value>>& items() const { return m_items; } // в порядке вставки
        void erase(const std::string& k);

        // массив
        size_t size() const { return m_type == Type::Array ? m_arr.size() : (m_type == Type::Object ? m_items.size() : 0); }
        const Value& operator[](size_t i) const;
        Value& at(size_t i) { return m_arr[i]; }
        Value& push(const Value& v);                         // превращает в массив при необходимости
        std::vector<Value>& arr() { return m_arr; }
        const std::vector<Value>& arr() const { return m_arr; }

        std::string dump() const;
        void dumpTo(std::string& out) const;

    private:
        Type m_type = Type::Null;
        bool m_bool = false;
        double m_num = 0;
        std::string m_str;
        std::vector<Value> m_arr;
        std::vector<std::pair<std::string, Value>> m_items;
        std::map<std::string, size_t> m_obj;                 // ключ -> индекс в m_items
    };

    bool Parse(const std::string& text, Value& out, std::string* err = nullptr);
    void Escape(const std::string& s, std::string& out);    // с кавычками
}
