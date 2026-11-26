#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <vector>
inline bool ReadWav(const char* p, std::vector<float>& x, int& sr)
{
    FILE* f = fopen(p, "rb"); if(!f) return false;
    std::vector<unsigned char> d; unsigned char b[65536]; size_t n;
    while((n = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + n);
    fclose(f);
    if(d.size() < 44) return false;
    size_t pos = 12; int ch = 1, bits = 16, fmt = 1; sr = 16000;
    while(pos + 8 <= d.size())
    {
        uint32_t len; memcpy(&len, &d[pos + 4], 4);
        if(!memcmp(&d[pos], "fmt ", 4)) { uint16_t a, c, bb; uint32_t r; memcpy(&a, &d[pos + 8], 2); memcpy(&c, &d[pos + 10], 2); memcpy(&r, &d[pos + 12], 4); memcpy(&bb, &d[pos + 22], 2); fmt = a; ch = c; sr = r; bits = bb; }
        else if(!memcmp(&d[pos], "data", 4))
        {
            size_t bytes = len; if(pos + 8 + bytes > d.size()) bytes = d.size() - pos - 8;
            int bps = bits / 8; size_t fr = bytes / (bps * ch); x.resize(fr);
            for(size_t i = 0; i < fr; ++i) { double acc = 0; for(int c = 0; c < ch; ++c) { const unsigned char* q = &d[pos + 8 + (i * ch + c) * bps];
                if(fmt == 3) { float v; memcpy(&v, q, 4); acc += v; } else if(bps == 2) { int16_t v; memcpy(&v, q, 2); acc += v / 32768.0; } else if(bps == 4) { int32_t v; memcpy(&v, q, 4); acc += v / 2147483648.0; } }
                x[i] = (float)(acc / ch); }
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}
inline void WriteWav(const char* p, const std::vector<float>& x, int sr)
{
    FILE* f = fopen(p, "wb"); uint32_t n = (uint32_t)x.size() * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + n; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t s = 1; fwrite(&s, 2, 1, f); fwrite(&s, 2, 1, f); v = sr; fwrite(&v, 4, 1, f); v = sr * 2; fwrite(&v, 4, 1, f); s = 2; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f);
    for(float q : x) { if(q > 1) q = 1; if(q < -1) q = -1; int16_t w = (int16_t)(q * 32767); fwrite(&w, 2, 1, f); }
    fclose(f);
}
