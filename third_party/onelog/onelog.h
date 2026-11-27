// Один общий лог для SA Coop, SA Living и Tailscale: <память>/Documents/SA_LOG.txt
// Игрок: «логи — в один файл, без прошлых версий». Все три пишут в один файл (дозапись, O_APPEND),
// каждая строка помечена: «К» — кооп, «И» — SA Living, «Т» — Tailscale. Новая игра не стирает
// прошлую: запись о вылете остаётся в том же файле. Файл больше 12 МБ — при запуске остаются
// последние 4 МБ. Старые файлы (sacoop*.txt, saliving*.txt, trace/, tailscale_*.txt) удаляются.
#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace onelog
{
    inline std::string FilePath(const char* root)
    {
        return std::string(root && root[0] ? root : "/storage/emulated/0") + "/Documents/SA_LOG.txt";
    }

    // Длинный файл — оставить последние keep байт (с начала строки)
    inline void Trim(int fd, off_t maxSize, off_t keep)
    {
        struct stat st;
        if(fstat(fd, &st) != 0 || st.st_size <= maxSize) return;
        std::string tail((size_t)keep, '\0');
        ssize_t n = pread(fd, &tail[0], (size_t)keep, st.st_size - keep);
        if(n <= 0) return;
        tail.resize((size_t)n);
        size_t nl = tail.find('\n');
        if(nl != std::string::npos) tail.erase(0, nl + 1);
        std::string head = "...... начало файла обрезано (оставлены последние ~4 МБ) ......\n";
        if(ftruncate(fd, 0) != 0) return;
        if(pwrite(fd, head.data(), head.size(), 0) < 0) return;
        (void)!pwrite(fd, tail.data(), tail.size(), (off_t)head.size());
    }

    // Открыть общий файл для дозаписи. Возвращает fd или -1.
    inline int Open(const char* root, std::string* pathOut = nullptr)
    {
        std::string dir = std::string(root && root[0] ? root : "/storage/emulated/0") + "/Documents";
        mkdir(dir.c_str(), 0777);
        std::string path = FilePath(root);
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if(fd < 0) return -1;
        flock(fd, LOCK_EX);
        Trim(fd, 12 << 20, 4 << 20);
        flock(fd, LOCK_UN);
        if(pathOut) *pathOut = path;
        return fd;
    }

    // Записать целые строки одним вызовом (дозапись атомарна — строки двух модов не перемешаются)
    inline void Write(int fd, const char* data, size_t len)
    {
        while(fd >= 0 && len > 0)
        {
            ssize_t n = write(fd, data, len);
            if(n < 0) { if(errno == EINTR) continue; return; }
            data += n; len -= (size_t)n;
        }
    }

    inline void RemoveDirFiles(const std::string& dir)
    {
        if(DIR* d = opendir(dir.c_str()))
        {
            while(dirent* e = readdir(d))
            {
                if(e->d_name[0] == '.') continue;
                remove((dir + "/" + e->d_name).c_str());
            }
            closedir(d);
        }
        rmdir(dir.c_str());
    }

    // Убрать старые логи этого мода (раньше: <name>.txt, <name>_prev.txt, trace/, tailscale_*.txt)
    inline void RemoveOld(const char* root, const char* guid, const char* name)
    {
        std::string d = std::string(root && root[0] ? root : "/storage/emulated/0") + "/Documents/AMLModLogs/" + guid;
        remove((d + "/" + name + ".txt").c_str());
        remove((d + "/" + name + "_prev.txt").c_str());
        RemoveDirFiles(d + "/trace");
        rmdir(d.c_str());   // пустую папку — тоже (если там есть что-то ещё — останется)
    }
}
