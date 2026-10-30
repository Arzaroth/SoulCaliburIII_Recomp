#pragma once

#include "runtime/native/native_gs_hook.h"

#include <immintrin.h>

#include <array>
#include <cstdint>
#include <vector>

namespace sc3
{
    class DrawSink
    {
    public:
        enum Field : int
        {
            Tag = -1,
            St = 0,
            Rgba = 1,
            Xyz = 2,
        };

        struct Stats
        {
            uint64_t kicks = 0, materials = 0, triangles = 0, foreign = 0, unknownRegisters = 0, badPrims = 0, overflows = 0;
        };

        void begin(const uint8_t *data, uint32_t top, uint32_t packetBase)
        {
            m_data = data;
            m_packet = (top + packetBase) & 0x3FFu;
        }

        template <int kField, int kDest>
        void put(int32_t qword, __m128 value)
        {
            if constexpr (kField == Tag)
            {
                m_tag = blend<kDest>(m_tag, value);
                return;
            }
            const int32_t offset = static_cast<int32_t>((static_cast<uint32_t>(qword) - m_packet - 1u) & 0x3FFu);
            const int32_t vertex = offset / 3;
            if (vertex >= static_cast<int32_t>(kVertices))
            {
                if (offset < 0x3F0)
                    ++m_stats.overflows;
                return;
            }
            Vertex &v = m_vertices[static_cast<size_t>(vertex)];
            if constexpr (kField == St)
                v.st = blend<kDest>(v.st, value);
            else if constexpr (kField == Rgba)
                v.rgba = blend<kDest>(v.rgba, value);
            else
                v.xyz = blend<kDest>(v.xyz, value);
        }

        // Returns the quadwords PATH1 would send.
        uint32_t kick(int32_t vi);

        const Stats &stats() const { return m_stats; }
        void resetStats() { m_stats = Stats{}; }

        struct Draw
        {
            ps2x::native::GsMaterial material;
            std::vector<ps2x::native::GsVertex> triangles;
        };
        void capture(std::vector<Draw> *draws) { m_capture = draws; }
        void replay(const uint8_t *packet, size_t bytes);

    private:
        static constexpr size_t kVertices = 128u;

        struct Vertex
        {
            __m128 st, rgba, xyz;
        };

        template <int kDest>
        static __m128 blend(__m128 old, __m128 value)
        {
            constexpr int lanes = ((kDest >> 3) & 1) | (((kDest >> 2) & 1) << 1) | (((kDest >> 1) & 1) << 2) | ((kDest & 1) << 3);
            if constexpr (lanes == 15)
                return value;
            else
                return _mm_blend_ps(old, value, lanes);
        }

        uint32_t drawPacket();
        uint32_t dataPacket(uint32_t qword);
        void writeRegister(uint32_t reg, uint64_t value);
        void submit(ps2x::native::GsMaterial &material);

        const uint8_t *m_data = nullptr;
        uint32_t m_packet = 0;
        __m128 m_tag = _mm_setzero_ps();
        std::array<Vertex, kVertices> m_vertices{};
        ps2x::native::GsMaterial m_material[2];
        uint64_t m_prim = 0;
        std::vector<ps2x::native::GsVertex> m_triangles;
        std::vector<Draw> *m_capture = nullptr;
        Stats m_stats;
    };

    DrawSink &drawSink();
}
