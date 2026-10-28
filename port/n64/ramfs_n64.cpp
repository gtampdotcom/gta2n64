// Sistema de arquivos em RAM para saves/perfis (player\*). O DragonFS (rom:/) e somente leitura;
// o motor grava e le de volta plyslot*.dat, hiscores etc.
// Persistence: the whole set is mirrored to the cartridge's 32 KB SRAM (ROM header: N64_ROM_SAVETYPE = sram256k).
// It is read on first use and written back whenever a file opened for writing is closed or a file is removed.
#include <libdragon.h>
#include <malloc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

namespace
{
struct RamFile
{
    std::vector<unsigned char> data;
};
std::map<std::string, RamFile> gFiles;

// ---- SRAM image: "G2SV", entry count, payload bytes, checksum, then per file: key length (1), key, size (4), data ----
const uint32_t kSramBase = 0x08000000u;
const uint32_t kSramSize = 32 * 1024;
const uint32_t kMagic = 0x47325356u; // "G2SV"
bool gLoaded;
uint8_t* gImg; // the 32 KB SRAM image buffer: allocated once at the first load (in the menu) and kept, because mid-game the
               // heap is too full for a 32 KB block and the church save was silently not written

void SramPi()
{
    // PI domain 2 timings for SRAM (the values every SRAM cartridge uses)
    *reinterpret_cast<volatile uint32_t*>(0xA4600024) = 0x05;
    *reinterpret_cast<volatile uint32_t*>(0xA4600028) = 0x0C;
    *reinterpret_cast<volatile uint32_t*>(0xA460002C) = 0x0D;
    *reinterpret_cast<volatile uint32_t*>(0xA4600030) = 0x02;
}

uint32_t Sum(const uint8_t* p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
    {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

uint32_t Rd32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Wr32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

void Load()
{
    gLoaded = true;
    SramPi();
    gImg = static_cast<uint8_t*>(memalign(16, kSramSize));
    uint8_t* img = gImg;
    if (!img)
    {
        return;
    }
    // raw PI DMA, as the write uses (dma_read() is meant for the ROM area and did not bring the SRAM back after a reset)
    data_cache_hit_writeback_invalidate(img, kSramSize);
    dma_read_raw_async(img, kSramBase, kSramSize);
    dma_wait();
    data_cache_hit_invalidate(img, kSramSize);
    const uint32_t count = Rd32(img + 4), bytes = Rd32(img + 8);
    if (Rd32(img) == kMagic && bytes <= kSramSize - 16 && Sum(img + 16, bytes) == Rd32(img + 12))
    {
        const uint8_t *p = img + 16, *end = p + bytes;
        for (uint32_t i = 0; i < count && p < end; ++i)
        {
            const uint32_t klen = *p++;
            if (p + klen + 4 > end)
            {
                break;
            }
            const std::string key(reinterpret_cast<const char*>(p), klen);
            p += klen;
            const uint32_t size = Rd32(p);
            p += 4;
            if (p + size > end)
            {
                break;
            }
            gFiles[key].data.assign(p, p + size);
            p += size;
        }
    }
}

void Flush()
{
    uint8_t* img = gImg;
    if (!img)
    {
        return;
    }
    memset(img, 0, kSramSize);
    uint8_t* p = img + 16;
    uint32_t count = 0;
    for (const auto& kv : gFiles)
    {
        const size_t need = 1 + kv.first.size() + 4 + kv.second.data.size();
        if (kv.first.size() > 255 || static_cast<size_t>(p - img) + need > kSramSize)
        {
            continue; // does not fit: kept in RAM only
        }
        *p++ = uint8_t(kv.first.size());
        memcpy(p, kv.first.data(), kv.first.size());
        p += kv.first.size();
        Wr32(p, uint32_t(kv.second.data.size()));
        p += 4;
        if (!kv.second.data.empty())
        {
            memcpy(p, kv.second.data.data(), kv.second.data.size());
        }
        p += kv.second.data.size();
        ++count;
    }
    const uint32_t bytes = uint32_t(p - img - 16);
    Wr32(img, kMagic);
    Wr32(img + 4, count);
    Wr32(img + 8, bytes);
    Wr32(img + 12, Sum(img + 16, bytes));
    const uint32_t len = (16 + bytes + 7) & ~7u;
    SramPi();
    data_cache_hit_writeback(img, len);
    dma_write_raw_async(img, kSramBase, len);
    dma_wait();
}

struct Cookie
{
    RamFile* file;
    size_t pos;
    bool canRead, canWrite, append;
};

int CReadImpl(void* c, char* buf, size_t n)
{
    Cookie* k = static_cast<Cookie*>(c);
    const size_t size = k->file->data.size();
    const size_t avail = k->pos < size ? size - k->pos : 0;
    const size_t m = n < avail ? n : avail;
    if (m)
    {
        memcpy(buf, &k->file->data[k->pos], m);
    }
    k->pos += m;
    return static_cast<int>(m);
}

int CWriteImpl(void* c, const char* buf, size_t n)
{
    Cookie* k = static_cast<Cookie*>(c);
    if (k->append)
    {
        k->pos = k->file->data.size();
    }
    if (k->pos + n > k->file->data.size())
    {
        k->file->data.resize(k->pos + n);
    }
    if (n)
    {
        memcpy(&k->file->data[k->pos], buf, n);
    }
    k->pos += n;
    return static_cast<int>(n);
}

// devolve a nova posicao ou -1
long long CSeekImpl(void* c, long long off, int whence)
{
    Cookie* k = static_cast<Cookie*>(c);
    const long long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? static_cast<long long>(k->pos) : static_cast<long long>(k->file->data.size());
    const long long np = base + off;
    if (np < 0)
    {
        return -1;
    }
    k->pos = static_cast<size_t>(np);
    return np;
}

int CClose(void* c)
{
    Cookie* k = static_cast<Cookie*>(c);
    const bool wrote = k->canWrite;
    delete k;
    if (wrote)
    {
        Flush();
    }
    return 0;
}

// funopen (BSD, newlib): os tipos dos callbacks variam entre versoes do newlib; deduz-os da declaracao real.
template <class T> struct RdT;
template <class N> struct RdT<int (*)(void*, char*, N)>
{
    static int fn(void* c, char* b, N n) { return CReadImpl(c, b, static_cast<size_t>(n)); }
};
template <class T> struct WrT;
template <class N> struct WrT<int (*)(void*, const char*, N)>
{
    static int fn(void* c, const char* b, N n) { return CWriteImpl(c, b, static_cast<size_t>(n)); }
};
template <class T> struct SkT;
template <class O> struct SkT<O (*)(void*, O, int)>
{
    static O fn(void* c, O off, int w) { return static_cast<O>(CSeekImpl(c, static_cast<long long>(off), w)); }
};

template <class C, class RD, class WR, class SK, class CL>
FILE* OpenWith(FILE* (*fo)(C, RD, WR, SK, CL), Cookie* k)
{
    return fo(k, k->canRead ? RdT<RD>::fn : static_cast<RD>(nullptr), k->canWrite ? WrT<WR>::fn : static_cast<WR>(nullptr),
              SkT<SK>::fn, static_cast<CL>(CClose));
}
} // namespace

extern "C" FILE* gta2_ramfs_open(const std::string& key, const char* mode)
{
    if (!gLoaded)
    {
        Load();
    }
    const bool plus = strchr(mode, '+') != nullptr;
    const char m = mode[0];
    auto it = gFiles.find(key);
    if (m == 'r' && it == gFiles.end())
    {
        errno = ENOENT;
        return nullptr;
    }
    RamFile& f = gFiles[key];
    if (m == 'w')
    {
        f.data.clear();
    }
    Cookie* k = new Cookie{&f, 0, m == 'r' || plus, m != 'r' || plus, m == 'a'};
    FILE* fp = OpenWith(&funopen, k);
    if (!fp)
    {
        delete k;
    }
    return fp;
}

extern "C" bool gta2_ramfs_size(const std::string& key, long* size)
{
    if (!gLoaded)
    {
        Load();
    }
    auto it = gFiles.find(key);
    if (it == gFiles.end())
    {
        return false;
    }
    *size = static_cast<long>(it->second.data.size());
    return true;
}

extern "C" int gta2_ramfs_remove(const std::string& key)
{
    if (!gLoaded)
    {
        Load();
    }
    if (!gFiles.erase(key))
    {
        return -1;
    }
    Flush();
    return 0;
}
