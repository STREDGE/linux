#pragma once
// Подробный лог игровой сессии («супер-лог»): всегда включён, обычный текст, один файл на запуск игры.
//   Documents/AMLModLogs/net.saliving.mod/trace/trace_ГГГГ-ММ-ДД_ЧЧ-ММ-СС.txt
// Что пишется: шапка (версии, телефон, моды, настройки — без ключа), игрок каждые 0,5 с, все педы и машины
// раз в 2 с (изменения; полный снимок раз в 30 с), события сразу (розыск, урон, смерть, машины, миссии, катсцены,
// телепорты, задачи модa педам, команды игры, ИИ, речь, кнопки), FPS и подвисания, заполненность пулов,
// отметки игрока («Метка» / «Джарвис, отметь баг …»), вылет.
// Запись — в фоновом потоке; из игрового потока только складываем строки в очередь.
//
// Формат строки:  ЧЧ:ММ:СС.ммм [ТЕГ] текст
// Педы:   P#ref m=модель t=тип st=состояние hp=… task=… car=V#… (x,y,z) h=курс v=скорость [наш#id Имя] [скрипт]
// Машины: V#ref m=модель (x,y,z) v=скорость hp=… drv=P#… пас=n/max [игрок]

#include <stdarg.h>
#include <string>

namespace trace
{
    void Init();                                   // один раз при загрузке мода (после settings::Load)
    void Line(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LineV(const char* tag, const char* fmt, va_list ap);
    void Frame();                                  // раз в кадр игры (игровой поток)
    void AddModTime(double sec);                   // 0.5.11: сколько кадра занял мод (для строки FPS)
    void Mark(const std::string& text);            // отметка игрока + полный снимок мира
    void Crash(int sig, unsigned long pc, unsigned long lr, const char* lib, unsigned long off, const char* where);
    std::string Path();
}

// Коротко для событий из модулей
#define TRACE(tag, ...) trace::Line(tag, __VA_ARGS__)
