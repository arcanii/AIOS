#pragma once
// HOST SHIM for /tmp/random06457/src/device/v3d/v3d_memory.hpp
// Substituted on the include path (our -I comes first) so we never edit their tree.
// Provides a DETERMINISTIC ARM_TO_BUS: instead of truncating a real heap pointer to
// u32 (non-reproducible), we translate registered buffer pointers to fixed synthetic
// GPU/bus bases. This changes ONLY the absolute addresses embedded in the CLs, never
// the field encodings. The real kernel header is a one-line identity truncation
// (linear map); ours is identity-with-a-known-base.

#include <new>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "types.hpp"
#include "mbox_message.hpp"

// Global translation table: maps a contiguous host buffer [base, base+span) to a
// synthetic bus base. ARM_TO_BUS(p) = busBase + (p - hostBase).
struct BusMap
{
    struct Range { const uint8_t* host; uint32_t bus; size_t span; };
    std::vector<Range> ranges;

    // Default span 64 KB. Callers SHOULD pass the buffer's true capacity so that
    // distinct host allocations that happen to be close on the heap never alias.
    void reg(const void* host, uint32_t bus, size_t span = (64u * 1024u))
    {
        ranges.push_back({ reinterpret_cast<const uint8_t*>(host), bus, span });
    }
    uint32_t translate(const void* p) const
    {
        auto pc = reinterpret_cast<const uint8_t*>(p);
        for (auto& r : ranges) {
            if (pc >= r.host && pc < r.host + r.span)
                return r.bus + static_cast<uint32_t>(pc - r.host);
        }
        // Unregistered: fall back to low-32 truncation (kernel behaviour).
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    }
};

inline BusMap g_busmap;

template<typename T>
static inline u32 ARM_TO_BUS(T* ptr)
{
    return g_busmap.translate(ptr);
}

// V3DBuffer: same shape as theirs, but paddr() routes through the busmap and it
// self-registers its allocation so triangle code that uses V3DBuffer stays valid.
// (Our harness does not actually use V3DBuffer for the clear; provided for parity.)
class V3DBuffer
{
public:
    V3DBuffer(u32 size, u32 align)
    {
        m_addr = new (std::align_val_t(align)) u8[size];
        m_size = size;
        m_align = align;
    }
    ~V3DBuffer() { delete[] m_addr; }

    template<typename T = void>
    inline T* ptr(size_t off = 0)
    {
        return reinterpret_cast<T*>(m_addr + off);
    }

    u32 paddr() const { return ARM_TO_BUS(m_addr); }
    size_t size() const { return m_size; }

public:
    u32 m_size;
    u32 m_align;
    u8* m_addr;
};
