#include "sc3_draw_sink.h"

#include "runtime/gs/gs_registers.h"
#include "runtime/native/native_renderer.h"

#include <cstring>

namespace sc3
{
    namespace
    {
        uint32_t lane(__m128 value, int index)
        {
            alignas(16) uint32_t words[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(words), _mm_castps_si128(value));
            return words[index];
        }

        float laneFloat(__m128 value, int index)
        {
            const uint32_t bits = lane(value, index);
            float out;
            std::memcpy(&out, &bits, 4);
            return out;
        }

        uint32_t qwordsOf(uint64_t lo)
        {
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu), format = static_cast<uint32_t>((lo >> 58) & 3u);
            const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            if (format == 0u)
                return 1u + nloop * nreg;
            if (format == 1u)
                return 1u + (nloop * nreg + 1u) / 2u;
            return 1u + nloop;
        }
    }

    DrawSink &drawSink()
    {
        static thread_local DrawSink s_sink;
        return s_sink;
    }

    uint32_t DrawSink::kick(int32_t vi)
    {
        const uint32_t qword = static_cast<uint32_t>(static_cast<uint16_t>(vi)) & 0x3FFu;
        ++m_stats.kicks;
        return qword == m_packet ? drawPacket() : dataPacket(qword);
    }

    uint32_t DrawSink::drawPacket()
    {
        const uint64_t lo = static_cast<uint64_t>(lane(m_tag, 0)) | (static_cast<uint64_t>(lane(m_tag, 1)) << 32);
        const uint64_t hi = static_cast<uint64_t>(lane(m_tag, 2)) | (static_cast<uint64_t>(lane(m_tag, 3)) << 32);
        const uint32_t qwords = qwordsOf(lo);
        const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
        if (((lo >> 58) & 3u) != 0u || ((lo >> 60) & 0xFu) != 3u || (hi & 0xFFFu) != 0x412u || ((lo >> 15) & 1u) == 0u ||
            nloop > kVertices)
        {
            ++m_stats.foreign;
            return qwords;
        }
        if ((lo >> 46) & 1u)
            m_prim = (lo >> 47) & 0x7FFu;
        const GSPrimReg prim = ps2x::gs::decodePrim(m_prim);
        if (prim.type != GS_PRIM_TRIANGLE && prim.type != GS_PRIM_TRISTRIP && prim.type != GS_PRIM_TRIFAN)
        {
            ++m_stats.badPrims;
            return qwords;
        }
        m_triangles.clear();
        ps2x::native::GsVertex queue[3];
        uint32_t queued = 0;
        for (uint32_t i = 0; i < nloop; ++i)
        {
            const Vertex &in = m_vertices[i];
            ps2x::native::GsVertex &v = queue[queued];
            v.s = laneFloat(in.st, 0);
            v.t = laneFloat(in.st, 1);
            v.q = laneFloat(in.st, 2);
            if (v.q == 0.0f)
                v.q = 1.0f;
            v.rgba = (lane(in.rgba, 0) & 0xFFu) | ((lane(in.rgba, 1) & 0xFFu) << 8) | ((lane(in.rgba, 2) & 0xFFu) << 16) |
                     ((lane(in.rgba, 3) & 0xFFu) << 24);
            const uint32_t w = lane(in.xyz, 3);
            v.x = static_cast<float>(lane(in.xyz, 0) & 0xFFFFu) / 16.0f;
            v.y = static_cast<float>(lane(in.xyz, 1) & 0xFFFFu) / 16.0f;
            v.z = (lane(in.xyz, 2) >> 4) & 0xFFFFFFu;
            v.fog = static_cast<uint8_t>((w >> 4) & 0xFFu);
            if (++queued < 3u)
                continue;
            if (((w >> 15) & 1u) == 0u)
                m_triangles.insert(m_triangles.end(), queue, queue + 3);
            if (prim.type == GS_PRIM_TRIANGLE)
                queued = 0;
            else
            {
                if (prim.type == GS_PRIM_TRISTRIP)
                    queue[0] = queue[1];
                queue[1] = queue[2];
                queued = 2;
            }
        }
        m_stats.triangles += m_triangles.size() / 3u;
        if (!m_triangles.empty())
            submit(m_material[prim.ctxt ? 1 : 0]);
        return qwords;
    }

    uint32_t DrawSink::dataPacket(uint32_t qword)
    {
        uint32_t total = 0;
        for (;;)
        {
            const uint8_t *tag = m_data + ((qword + total) & 0x3FFu) * 16u;
            uint64_t lo, hi;
            std::memcpy(&lo, tag, 8);
            std::memcpy(&hi, tag + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            const bool registers = ((lo >> 58) & 3u) == 0u && nreg == 1u && (hi & 0xFu) == 0xEu;
            if (!registers)
                ++m_stats.foreign;
            else
            {
                ++m_stats.materials;
                for (uint32_t i = 0; i < nloop; ++i)
                {
                    const uint8_t *entry = m_data + ((qword + total + 1u + i) & 0x3FFu) * 16u;
                    uint64_t value, address;
                    std::memcpy(&value, entry, 8);
                    std::memcpy(&address, entry + 8, 8);
                    writeRegister(static_cast<uint32_t>(address & 0xFFu), value);
                }
            }
            total += qwordsOf(lo);
            if (((lo >> 15) & 1u) != 0u || total >= 0x400u)
                return total;
        }
    }

    void DrawSink::writeRegister(uint32_t reg, uint64_t value)
    {
        ps2x::native::GsMaterial &m = m_material[reg == 0x48u || (reg != 0x47u && (reg & 1u) != 0u) ? 1 : 0];
        switch (reg)
        {
        case 0x00u:
            m_prim = value & 0x7FFu;
            break;
        case 0x06u:
        case 0x07u:
            m.tex0 = ps2x::gs::decodeTex0(value);
            m.loadClut = true;
            break;
        case 0x08u:
        case 0x09u:
            m.clamp = value;
            break;
        case 0x14u:
        case 0x15u:
            m.tex1 = value;
            break;
        case 0x34u:
        case 0x35u:
            m.miptbp1 = value;
            break;
        case 0x36u:
        case 0x37u:
            m.miptbp2 = value;
            break;
        case 0x42u:
        case 0x43u:
            m.alpha = value;
            break;
        case 0x47u:
        case 0x48u:
            m.test = value;
            break;
        default:
            ++m_stats.unknownRegisters;
            break;
        }
    }

    void DrawSink::replay(const uint8_t *packet, size_t bytes)
    {
        size_t offset = 0;
        while (offset + 16u <= bytes)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, packet + offset, 8);
            std::memcpy(&hi, packet + offset + 8, 8);
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t nreg = ((lo >> 60) & 0xFu) ? static_cast<uint32_t>((lo >> 60) & 0xFu) : 16u;
            const size_t size = size_t{qwordsOf(lo)} * 16u;
            if (offset + size > bytes)
                return;
            if (((lo >> 58) & 3u) == 0u && nreg == 1u && (hi & 0xFu) == 0xEu)
            {
                ++m_stats.materials;
                for (uint32_t i = 0; i < nloop; ++i)
                {
                    uint64_t value, address;
                    std::memcpy(&value, packet + offset + 16u * (i + 1u), 8);
                    std::memcpy(&address, packet + offset + 16u * (i + 1u) + 8u, 8);
                    writeRegister(static_cast<uint32_t>(address & 0xFFu), value);
                }
            }
            else
            {
                m_tag = _mm_loadu_ps(reinterpret_cast<const float *>(packet + offset));
                for (uint32_t i = 0; i < nloop && i < kVertices && nreg == 3u; ++i)
                {
                    const float *v = reinterpret_cast<const float *>(packet + offset + 16u + 48u * i);
                    m_vertices[i] = {_mm_loadu_ps(v), _mm_loadu_ps(v + 4), _mm_loadu_ps(v + 8)};
                }
                drawPacket();
            }
            offset += size;
            if ((lo >> 15) & 1u)
                return;
        }
    }

    void DrawSink::submit(ps2x::native::GsMaterial &material)
    {
        ps2x::native::GsMaterial draw = material;
        draw.prim = ps2x::gs::decodePrim(m_prim);
        material.loadClut = false;
        if (m_capture)
        {
            m_capture->push_back({draw, m_triangles});
            return;
        }
        ps2x::native::Renderer::instance().drawGs(draw, m_triangles.data(), static_cast<uint32_t>(m_triangles.size()));
    }
}
