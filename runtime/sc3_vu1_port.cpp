#include "sc3_vu1_port.h"

#include "runtime/ps2_vu1_inline.h"

#include <immintrin.h>

#include <array>
#include <cstring>
#include <limits>

namespace sc3
{
    namespace
    {
        constexpr int lanes(int dest) { return ((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3); }

        constexpr int X = 8, Y = 4, Z = 2, W = 1, XY = X | Y, XYZ = X | Y | Z, ZW = Z | W, XYZW = 15;

        template <int kDest>
        inline __m128 put(__m128 old, __m128 value)
        {
            if constexpr (kDest == XYZW)
                return value;
            else
                return _mm_blend_ps(old, value, lanes(kDest));
        }

        inline __m128i bits(__m128 value) { return _mm_castps_si128(value); }
        inline __m128 floats(__m128i value) { return _mm_castsi128_ps(value); }

        inline __m128 norm(__m128 value)
        {
            const __m128i b = bits(value);
            const __m128i magnitudeMask = _mm_set1_epi32(0x7FFFFFFF);
            const __m128i magnitude = _mm_and_si128(b, magnitudeMask);
            const __m128i normal = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x007FFFFF));
            const __m128i clamped = _mm_and_si128(_mm_min_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFF)), normal);
            return floats(_mm_or_si128(_mm_andnot_si128(magnitudeMask, b), clamped));
        }

        template <int kLane>
        inline __m128 bc(__m128 value)
        {
            return _mm_shuffle_ps(value, value, _MM_SHUFFLE(kLane, kLane, kLane, kLane));
        }

        enum class Op
        {
            Add,
            Mul,
            Madd,
        };

        struct Flags
        {
            uint32_t sticky = 0; // status U / O bits (0x4, 0x8) from results out of range
        };

        struct Classified
        {
            __m128 value;
            int zero, sign, under, over;
        };

        template <Op kOp>
        __attribute__((noinline, cold)) Classified exact(__m128 value, int active, __m128 l, __m128 r, __m128 a)
        {
            const __m128i e = _mm_and_si128(_mm_srli_epi32(bits(value), 23), _mm_set1_epi32(0xFF));
            const __m128i inRange = _mm_and_si128(_mm_cmpgt_epi32(e, _mm_set1_epi32(1)), _mm_cmpgt_epi32(_mm_set1_epi32(254), e));
            const int outOfRange = active & ~_mm_movemask_ps(floats(inRange));
            Classified c{value, 0, _mm_movemask_ps(value) & active & ~outOfRange, 0, 0};
            if (outOfRange == 0)
                return c;
            const __m256d lw = _mm256_cvtps_pd(l), rw = _mm256_cvtps_pd(r);
            __m256d wide;
            if constexpr (kOp == Op::Add)
                wide = _mm256_add_pd(lw, rw);
            else if constexpr (kOp == Op::Mul)
                wide = _mm256_mul_pd(lw, rw);
            else
                wide = _mm256_add_pd(_mm256_cvtps_pd(a), _mm256_mul_pd(lw, rw));
            const __m256d magnitude = _mm256_andnot_pd(_mm256_set1_pd(-0.0), wide);
            const __m256d floatMax = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max()));
            const __m256d floatMin = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::min()));
            const __m256d margin = _mm256_set1_pd(static_cast<double>(std::numeric_limits<float>::max()) * (1.0 + 0x1p-40));
            const int atMax = kOp == Op::Mul ? 0
                                             : _mm256_movemask_pd(_mm256_and_pd(_mm256_cmp_pd(magnitude, floatMax, _CMP_GE_OQ),
                                                                                _mm256_cmp_pd(magnitude, margin, _CMP_LE_OQ))) &
                                                   outOfRange;
            const int classified = outOfRange & ~atMax;
            const int isZero = _mm256_movemask_pd(_mm256_cmp_pd(magnitude, _mm256_setzero_pd(), _CMP_EQ_OQ)) & classified;
            const int isTiny = _mm256_movemask_pd(_mm256_cmp_pd(magnitude, floatMin, _CMP_LT_OQ)) & classified & ~isZero;
            const int isHuge = _mm256_movemask_pd(_mm256_cmp_pd(magnitude, floatMax, _CMP_GT_OQ)) & classified;
            const int negative = _mm256_movemask_pd(wide) & classified;
            alignas(16) uint32_t words[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(words), bits(value));
            for (int lane = 0; lane < 4; ++lane)
            {
                const int bit = 1 << lane;
                if ((isZero | isTiny | isHuge) & bit)
                    words[lane] = ((negative & bit) ? 0x80000000u : 0u) | ((isHuge & bit) ? 0x7F7FFFFFu : 0u);
                else if (atMax & bit)
                {
                    const uint32_t sign = words[lane] & 0x80000000u;
                    if (((words[lane] >> 23) & 0xFFu) == 0xFFu)
                        words[lane] = sign | 0x7F7FFFFFu;
                    if (sign)
                        c.sign |= bit;
                    if ((words[lane] & 0x7FFFFFFFu) == 0x7F7FFFFFu)
                        c.over |= bit;
                }
            }
            c.value = _mm_load_ps(reinterpret_cast<const float *>(words));
            c.zero = isZero | isTiny;
            c.sign |= negative;
            c.under = isTiny;
            c.over |= isHuge;
            return c;
        }

        template <Op kOp, int kDest>
        inline __m128 fmacExact(Flags &flags, __m128 target, __m128 a, __m128 l, __m128 r)
        {
            __m128 value;
            if constexpr (kOp == Op::Add)
                value = ps2_vu1_detail::vuAdd4(l, r);
            else if constexpr (kOp == Op::Mul)
                value = _mm_mul_ps(l, r);
            else
                value = ps2_vu1_detail::vuAdd4(a, _mm_mul_ps(l, r));
            constexpr int kActive = lanes(kDest);
            const __m128i e = _mm_and_si128(_mm_srli_epi32(bits(value), 23), _mm_set1_epi32(0xFF));
            const __m128i inRange = _mm_and_si128(_mm_cmpgt_epi32(e, _mm_set1_epi32(1)), _mm_cmpgt_epi32(_mm_set1_epi32(254), e));
            const int bad = kActive & ~_mm_movemask_ps(floats(inRange));
            if (bad != 0) [[unlikely]]
            {
                const int zeroResult =
                    _mm_movemask_ps(floats(_mm_cmpeq_epi32(_mm_and_si128(bits(value), _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128())));
                int zeroExact = 0xF;
                if constexpr (kOp != Op::Add)
                {
                    const __m128i mask = _mm_set1_epi32(0x7F800000);
                    zeroExact = _mm_movemask_ps(floats(_mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(bits(l), mask), _mm_setzero_si128()),
                                                                    _mm_cmpeq_epi32(_mm_and_si128(bits(r), mask), _mm_setzero_si128()))));
                }
                if ((bad & ~(zeroResult & zeroExact)) != 0)
                {
                    const Classified c = exact<kOp>(value, kActive, l, r, a);
                    value = c.value;
                    flags.sticky |= (c.under ? 0x4u : 0u) | (c.over ? 0x8u : 0u);
                }
            }
            return put<kDest>(target, value);
        }

        inline __m128 maddLive(Flags &f, __m128 a, __m128 l, __m128 r, uint32_t &mac, uint32_t &current)
        {
            const __m128 value = ps2_vu1_detail::vuAdd4(a, _mm_mul_ps(l, r));
            Classified c = exact<Op::Madd>(value, 0xF, l, r, a);
            const int zeroLanes =
                _mm_movemask_ps(floats(_mm_cmpeq_epi32(_mm_and_si128(bits(c.value), _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128())));
            const __m128i e = _mm_and_si128(_mm_srli_epi32(bits(value), 23), _mm_set1_epi32(0xFF));
            const int inRange = _mm_movemask_ps(floats(_mm_and_si128(_mm_cmpgt_epi32(e, _mm_set1_epi32(1)), _mm_cmpgt_epi32(_mm_set1_epi32(254), e))));
            c.zero |= zeroLanes & inRange;
            static constexpr uint8_t kReverse4[16] = {0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE, 0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF};
            mac = kReverse4[c.zero] | (kReverse4[c.sign] << 4) | (kReverse4[c.under] << 8) | (kReverse4[c.over] << 12);
            current = (c.zero ? 0x1u : 0u) | (c.sign ? 0x2u : 0u) | (c.under ? 0x4u : 0u) | (c.over ? 0x8u : 0u);
            f.sticky |= current & 0xCu;
            return c.value;
        }

        template <int kScale, int kDest>
        inline __m128 ftoi(__m128 target, __m128 vs)
        {
            __m128 scaled = vs;
            if constexpr (kScale != 1)
                scaled = _mm_mul_ps(scaled, _mm_set1_ps(static_cast<float>(kScale)));
            const __m128i truncated = _mm_cvttps_epi32(scaled);
            const __m128 overflow = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
            return put<kDest>(target, _mm_blendv_ps(floats(truncated), floats(_mm_set1_epi32(0x7FFFFFFF)), overflow));
        }

        inline __m128i minMaxKey(__m128 value)
        {
            const __m128i b = bits(value);
            const __m128i sign = _mm_srai_epi32(b, 31);
            return _mm_sub_epi32(_mm_xor_si128(_mm_and_si128(b, _mm_set1_epi32(0x7FFFFFFF)), sign), sign);
        }

        template <int kDest>
        inline __m128 maxv(__m128 target, __m128 vs, __m128 t)
        {
            const __m128 pickS = floats(_mm_cmpgt_epi32(minMaxKey(vs), minMaxKey(t)));
            return put<kDest>(target, _mm_blendv_ps(t, vs, pickS));
        }

        template <int kDest>
        inline __m128 miniv(__m128 target, __m128 vs, __m128 t)
        {
            const __m128 pickS = floats(_mm_cmpgt_epi32(minMaxKey(t), minMaxKey(vs)));
            return put<kDest>(target, _mm_blendv_ps(t, vs, pickS));
        }

        inline uint32_t laneBits(__m128 value, int lane)
        {
            alignas(16) uint32_t words[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(words), bits(value));
            return words[lane];
        }

        inline uint32_t clipFlags(__m128 vs)
        {
            constexpr int32_t kLimit = 0x3F800000;
            alignas(16) uint32_t w[4];
            _mm_store_si128(reinterpret_cast<__m128i *>(w), bits(vs));
            uint32_t flags = 0u;
            flags |= static_cast<int32_t>(w[0]) > kLimit ? 0x01u : 0u;
            flags |= static_cast<int32_t>(w[0] ^ 0x80000000u) > kLimit ? 0x02u : 0u;
            flags |= static_cast<int32_t>(w[1]) > kLimit ? 0x04u : 0u;
            flags |= static_cast<int32_t>(w[1] ^ 0x80000000u) > kLimit ? 0x08u : 0u;
            flags |= static_cast<int32_t>(w[2]) > kLimit ? 0x10u : 0u;
            flags |= static_cast<int32_t>(w[2] ^ 0x80000000u) > kLimit ? 0x20u : 0u;
            return flags;
        }

        inline float normalizeOperand(float value)
        {
            uint32_t b = std::bit_cast<uint32_t>(value);
            const uint32_t exponent = (b >> 23) & 0xFFu;
            if (exponent == 0u)
                b &= 0x80000000u;
            else if (exponent == 0xFFu)
                b = (b & 0x80000000u) | 0x7F7FFFFFu;
            return std::bit_cast<float>(b);
        }

        inline float normalizeResult(float value)
        {
            uint32_t b = std::bit_cast<uint32_t>(value);
            const uint32_t exponent = (b >> 23) & 0xFFu;
            if (exponent == 0u)
                b &= 0x80000000u;
            else if (exponent == 0xFFu)
                b = (b & 0x80000000u) | 0x7F7FFFFFu;
            return std::bit_cast<float>(b);
        }

        inline float divide(float num, float den, uint32_t &statusDi)
        {
            num = normalizeOperand(num);
            den = normalizeOperand(den);
            float result;
            if (den == 0.0f)
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            }
            else
            {
                statusDi = 0u;
                result = num / den;
            }
            return normalizeResult(result);
        }

        struct Memory
        {
            uint8_t *data;

            uint8_t *at(int32_t qword) const { return data + ((static_cast<uint32_t>(qword) & 0x3FFu) << 4); }
            __m128 lq(int32_t qword) const { return _mm_loadu_ps(reinterpret_cast<const float *>(at(qword))); }
            template <int kDest>
            void sq(int32_t qword, __m128 value) const
            {
                float *target = reinterpret_cast<float *>(at(qword));
                _mm_storeu_ps(target, put<kDest>(_mm_loadu_ps(target), value));
            }
            int32_t ilw(int32_t qword, int component) const
            {
                uint32_t value;
                std::memcpy(&value, at(qword) + component * 4, 4);
                return static_cast<int16_t>(value & 0xFFFFu);
            }
            void isw(int32_t qword, int component, int32_t value) const
            {
                const uint32_t word = static_cast<uint16_t>(value & 0xFFFF);
                std::memcpy(at(qword) + component * 4, &word, 4);
            }
        };

        inline int32_t i16(int32_t value) { return static_cast<int16_t>(value); }

        uint32_t kickPacket(const uint8_t *data, int32_t vi, Vu1Kick kick, void *user)
        {
            static thread_local std::array<uint8_t, 0x10000u> s_packet;
            const uint32_t start = (static_cast<uint32_t>(static_cast<uint16_t>(vi)) * 16u) % 16384u;
            uint32_t bytes = 0u;
            for (;;)
            {
                if (bytes > s_packet.size() - 16u)
                    return 0u;
                const uint32_t source = (start + bytes) % 16384u;
                std::memcpy(s_packet.data() + bytes, data + source, 16u);
                uint64_t tag;
                std::memcpy(&tag, s_packet.data() + bytes, 8);
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu), format = static_cast<uint32_t>((tag >> 58) & 3u);
                const uint32_t nreg = ((tag >> 60) & 0xFu) ? static_cast<uint32_t>((tag >> 60) & 0xFu) : 16u;
                uint64_t tagBytes = 16u;
                if (format == 0u)
                    tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
                else if (format == 1u)
                    tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
                else
                    tagBytes += static_cast<uint64_t>(nloop) * 16u;
                if (tagBytes > s_packet.size() - bytes)
                    return 0u;
                for (uint32_t copied = 16u; copied < tagBytes; copied += 16u)
                    std::memcpy(s_packet.data() + bytes + copied, data + (start + bytes + copied) % 16384u, 16u);
                bytes += static_cast<uint32_t>(tagBytes);
                if ((tag >> 15) & 1u)
                    break;
            }
            kick(user, s_packet.data(), bytes);
            return bytes;
        }

        struct RoundTowardZero
        {
            unsigned int saved = _mm_getcsr();
            RoundTowardZero() { _mm_setcsr(saved | 0x6000u); }
            ~RoundTowardZero() { _mm_setcsr(saved); }
        };

        constexpr uint32_t kCheckLow = 0x21800000u - 1u;
        constexpr int32_t kCheckHigh = 0x7B800000;

        struct Check
        {
            __m128i low = _mm_set1_epi32(-1);
            __m128i high = _mm_setzero_si128();

            template <int kDest>
            void add(__m128 value)
            {
                __m128i magnitude = _mm_and_si128(bits(value), _mm_set1_epi32(0x7FFFFFFF));
                if constexpr (kDest != XYZW)
                    magnitude = bits(_mm_blend_ps(_mm_set1_ps(1.0f), floats(magnitude), lanes(kDest)));
                low = _mm_min_epu32(low, _mm_sub_epi32(magnitude, _mm_set1_epi32(1)));
                high = _mm_max_epi32(high, magnitude);
            }

            bool ok() const
            {
                const __m128i lowOk = _mm_cmpeq_epi32(_mm_max_epu32(low, _mm_set1_epi32(static_cast<int>(kCheckLow))), low);
                const __m128i highOk = _mm_cmpgt_epi32(_mm_set1_epi32(kCheckHigh), high);
                return _mm_movemask_ps(floats(_mm_and_si128(lowOk, highOk))) == 0xF;
            }
        };

        template <bool kFast>
        struct Alu
        {
            Flags &flags;
            Check check{};

            __m128 operand(__m128 raw) const
            {
                if constexpr (kFast)
                    return raw;
                else
                    return norm(raw);
            }

            template <int kDest>
            void loaded(__m128 raw)
            {
                if constexpr (kFast)
                    check.add<kDest>(raw);
            }

            template <Op kOp, int kDest>
            __m128 fmac(__m128 target, __m128 a, __m128 l, __m128 r)
            {
                if constexpr (kFast)
                {
                    __m128 value;
                    if constexpr (kOp == Op::Add)
                        value = ps2_vu1_detail::vuAdd4(l, r);
                    else if constexpr (kOp == Op::Mul)
                        value = _mm_mul_ps(l, r);
                    else
                        value = ps2_vu1_detail::vuAdd4(a, _mm_mul_ps(l, r));
                    check.add<kDest>(value);
                    return put<kDest>(target, value);
                }
                else
                    return fmacExact<kOp, kDest>(flags, target, a, l, r);
            }

            template <int kDest>
            __m128 add(__m128 t, __m128 l, __m128 r) { return fmac<Op::Add, kDest>(t, _mm_setzero_ps(), l, r); }
            template <int kDest>
            __m128 mul(__m128 t, __m128 l, __m128 r) { return fmac<Op::Mul, kDest>(t, _mm_setzero_ps(), l, r); }
            template <int kDest>
            __m128 madd(__m128 t, __m128 a, __m128 l, __m128 r) { return fmac<Op::Madd, kDest>(t, a, l, r); }
        };

        struct Constants
        {
            __m128 vf15, vf16, vf17, vf17raw, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30, vf31;
            int32_t vi3, vi12;
        };

        struct Skin
        {
            __m128 vf1, vf2, vf3, vf4, vf5, vf6, vf7, vf8, vf9, acc;
            int32_t vi4, vi10, vi13, vi14;
            uint64_t influences;
        };

        struct Light
        {
            __m128 vf2, vf3, vf4, vf5, vf6, vf7, vf8, vf9, vf10, vf11, acc;
            int32_t vi1, vi4, vi10, adc;
            float q;
            uint32_t clip, lastDi, stickyDi;
        };

        template <bool kFast>
        bool skinVertex(Skin &r, const Constants &k, const Memory &m, Flags &flags)
        {
            Alu<kFast> o{flags};
            const __m128 zero = _mm_setzero_ps();
            r.vi13 = m.ilw(r.vi10 + 1, 3);
            r.vf2 = zero;
            r.vf4 = zero;
            r.vf1 = zero;
            bool first = true;
            do
            {
                ++r.influences;
                if (kFast && first)
                {
                    r.acc = zero;
                    r.vf3 = m.lq(r.vi10);
                    r.vi10 = i16(r.vi10 + 1);
                    r.vf5 = m.lq(r.vi13);
                    r.vf6 = m.lq(r.vi13 + 1);
                    r.vf7 = m.lq(r.vi13 + 2);
                    r.vf8 = m.lq(r.vi13 + 3);
                }
                else
                {
                    r.acc = o.template add<XYZW>(r.acc, r.vf2, zero);
                    r.vf3 = m.lq(r.vi10);
                    r.vi10 = i16(r.vi10 + 1);
                    const __m128 n4 = o.operand(r.vf4);
                    r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf5), bc<0>(n4));
                    r.vf5 = m.lq(r.vi13);
                    r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf6), bc<1>(n4));
                    r.vf6 = m.lq(r.vi13 + 1);
                    r.vf2 = o.template madd<XYZW>(r.vf2, r.acc, o.operand(r.vf7), bc<2>(n4));
                    r.vf7 = m.lq(r.vi13 + 2);
                    r.acc = o.template add<XYZW>(r.acc, r.vf1, zero);
                    r.vf8 = m.lq(r.vi13 + 3);
                }
                first = false;
                o.template loaded<XYZW>(r.vf3);
                o.template loaded<XYZW>(r.vf5);
                o.template loaded<XYZW>(r.vf6);
                o.template loaded<XYZW>(r.vf7);
                o.template loaded<XYZW>(r.vf8);
                const __m128 n3 = o.operand(r.vf3);
                r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf5), bc<0>(n3));
                r.vi14 = r.vi13;
                r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf6), bc<1>(n3));
                r.vf4 = m.lq(r.vi10);
                o.template loaded<XYZ>(r.vf4);
                r.vi10 = i16(r.vi10 + 1);
                r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf7), bc<2>(n3));
                r.vi13 = m.ilw(r.vi10 + 1, 3);
                r.vf1 = o.template madd<XYZW>(r.vf1, r.acc, o.operand(r.vf8), bc<3>(n3));
                r.acc = o.template add<XYZW>(r.acc, r.vf2, zero);
                r.vi4 = m.ilw(r.vi10, 3);
            } while (i16(r.vi14) >= 0);
            const __m128 n4 = o.operand(r.vf4);
            r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf5), bc<0>(n4));
            r.vf3 = m.lq(r.vi10);
            r.vi10 = i16(r.vi10 + 1);
            r.acc = o.template madd<XYZW>(r.acc, r.acc, o.operand(r.vf6), bc<1>(n4));
            r.vf9 = m.lq(r.vi10);
            r.vi10 = i16(r.vi10 + 1);
            r.vf2 = o.template madd<XYZW>(r.vf2, r.acc, o.operand(r.vf7), bc<2>(n4));
            r.acc = o.template mul<XYZW>(r.acc, k.vf28, bc<0>(r.vf1));
            r.acc = o.template madd<XYZW>(r.acc, r.acc, k.vf29, bc<1>(r.vf1));
            r.acc = o.template madd<XYZW>(r.acc, r.acc, k.vf30, bc<2>(r.vf1));
            r.vf1 = o.template madd<XYZW>(r.vf1, r.acc, k.vf31, _mm_set1_ps(1.0f));
            if constexpr (kFast)
                return o.check.ok();
            else
                return true;
        }

        template <bool kFast>
        bool lightVertex(Light &r, const Constants &k, const Memory &m, Flags &flags)
        {
            Alu<kFast> o{flags};
            const __m128 zero = _mm_setzero_ps();
            r.acc = o.template mul<XYZW>(r.acc, k.vf24, bc<2>(r.vf10));
            r.vf2 = m.lq(r.vi10 + 2);
            r.vf6 = ftoi<16, XYZW>(r.vf6, o.operand(r.vf5));
            r.vf5 = m.lq(r.vi10 + 3);
            r.vf7 = o.template mul<XYZ>(r.vf7, o.operand(r.vf3), _mm_set1_ps(normalizeOperand(r.q)));
            r.vf3 = put<XY>(r.vf3, m.lq(r.vi10));
            o.template loaded<XYZ>(r.vf2);
            o.template loaded<XYZW>(r.vf5);
            o.template loaded<XY>(r.vf3);
            r.vf10 = o.template madd<XYZW>(r.vf10, r.acc, k.vf24, bc<3>(r.vf10));
            const __m128 n2 = o.operand(r.vf2);
            r.acc = o.template mul<XYZW>(r.acc, k.vf25, bc<0>(n2));
            r.acc = o.template madd<XYZW>(r.acc, r.acc, k.vf26, bc<1>(n2));
            uint32_t di = 0u;
            r.q = divide(1.0f, std::bit_cast<float>(laneBits(r.vf5, 3)), di);
            r.lastDi = di;
            r.stickyDi |= di;
            r.vf2 = o.template madd<XYZW>(r.vf2, r.acc, k.vf27, bc<2>(n2));
            r.acc = o.template mul<XYZW>(r.acc, r.vf8, o.operand(r.vf4));
            r.vf4 = m.lq(r.vi10 + 1);
            o.template loaded<XYZW>(r.vf4);
            r.vf9 = o.template madd<XYZW>(r.vf9, r.acc, r.vf10, _mm_set1_ps(128.0f));
            r.vf3 = o.template mul<XY>(r.vf3, o.operand(r.vf3), k.vf15);
            r.vi1 = r.vi1 | r.vi4;
            r.vf10 = o.template add<ZW>(r.vf10, k.vf16, bc<3>(r.vf2));
            r.vi1 = i16(r.vi1 + 0x7FFF);
            r.vf8 = maxv<XYZW>(r.vf8, r.vf2, zero);
            r.vi1 = r.vi1 & k.vi12;
            r.vf5 = o.template mul<XYZ>(r.vf5, o.operand(r.vf5), _mm_set1_ps(normalizeOperand(r.q)));
            r.vi1 = i16(r.vi1 + k.vi3);
            r.vf9 = miniv<XYZW>(r.vf9, r.vf9, bc<0>(k.vf17raw));
            r.adc = r.vi1;
            r.vf10 = maxv<ZW>(r.vf10, r.vf10, zero);
            r.acc = o.template mul<XYZW>(r.acc, k.vf18, k.vf19);
            r.vf11 = o.template madd<XYZW>(r.vf11, r.acc, o.operand(r.vf5), k.vf19);
            r.vi4 = m.ilw(r.vi10, 3);
            r.vf9 = ftoi<1, XYZW>(r.vf9, o.operand(r.vf9));
            r.vi10 = i16(r.vi10 + 4);
            r.vf10 = o.template mul<ZW>(r.vf10, r.vf10, k.vf17);
            r.acc = o.template add<XYZW>(r.acc, k.vf20, zero);
            r.clip = ((r.clip << 6) | clipFlags(r.vf11)) & 0xFFFFFFu;
            r.acc = o.template madd<XYZW>(r.acc, r.acc, k.vf21, bc<0>(r.vf8));
            r.vf10 = miniv<W>(r.vf10, r.vf10, bc<1>(k.vf17raw));
            r.acc = o.template madd<XYZW>(r.acc, r.acc, k.vf22, bc<1>(r.vf8));
            r.vf8 = o.template madd<XYZW>(r.vf8, r.acc, k.vf23, bc<2>(r.vf8));
            r.vi1 = (r.clip & 0x03FFFFu) != 0u ? 1 : 0;
            if constexpr (kFast)
                return o.check.ok();
            else
                return true;
        }

        __m128 load(const float *v) { return _mm_loadu_ps(v); }
        void store(float *target, __m128 value) { _mm_storeu_ps(target, value); }
    }

    bool skinnedBodyReady(const VU1State &state) { return (state.status & 0x0C0u) == 0x0C0u; }

    uint64_t runSkinnedBody(VU1State &state, uint8_t *data, uint32_t top, uint32_t itop, Vu1Kick kick, void *user, bool fast)
    {
        const RoundTowardZero rounding;
        const Memory m{data};
        Flags f;
        const __m128 zero = _mm_setzero_ps();
        const __m128 vf0 = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);

        Constants k;
        k.vf15 = norm(load(state.vf[15])), k.vf16 = norm(load(state.vf[16]));
        k.vf17raw = load(state.vf[17]), k.vf17 = norm(k.vf17raw);
        k.vf18 = norm(load(state.vf[18])), k.vf19 = norm(load(state.vf[19])), k.vf20 = norm(load(state.vf[20]));
        k.vf21 = norm(load(state.vf[21])), k.vf22 = norm(load(state.vf[22])), k.vf23 = norm(load(state.vf[23]));
        k.vf24 = norm(load(state.vf[24])), k.vf25 = norm(load(state.vf[25])), k.vf26 = norm(load(state.vf[26]));
        k.vf27 = norm(load(state.vf[27])), k.vf28 = norm(load(state.vf[28])), k.vf29 = norm(load(state.vf[29]));
        k.vf30 = norm(load(state.vf[30])), k.vf31 = norm(load(state.vf[31]));
        k.vi3 = state.vi[3], k.vi12 = state.vi[12];
        if (fast)
        {
            Check constants;
            for (const __m128 value : {k.vf15, k.vf16, k.vf17, k.vf17raw, k.vf18, k.vf19, k.vf20, k.vf21, k.vf22, k.vf23, k.vf24, k.vf25,
                                       k.vf26, k.vf27, k.vf28, k.vf29, k.vf30, k.vf31})
                constants.add<XYZW>(value);
            fast = constants.ok();
        }

        Skin s;
        s.vf1 = load(state.vf[1]), s.vf2 = load(state.vf[2]), s.vf3 = load(state.vf[3]), s.vf4 = load(state.vf[4]);
        s.vf5 = load(state.vf[5]), s.vf6 = load(state.vf[6]), s.vf7 = load(state.vf[7]), s.vf8 = load(state.vf[8]);
        s.vf9 = load(state.vf[9]), s.acc = load(state.acc);
        s.vi4 = state.vi[4], s.vi13 = state.vi[13], s.vi14 = state.vi[14];
        s.influences = 0u;
        int32_t vi6 = state.vi[6];

        const int32_t vi2 = static_cast<int32_t>(top & 0x3FFu);
        int32_t vi9 = m.ilw(vi2, 0);
        const __m128 vf13 = m.lq(vi2);
        s.vi10 = i16(vi2 + 1);
        int32_t vi11 = vi2;
        vi9 = i16(vi9 - k.vi12);
        uint64_t vertices = 0u;
        do
        {
            ++vertices;
            m.sq<XYZW>(vi11, s.vf1);
            vi11 = i16(vi11 + 1);
            const Skin saved = s;
            if (!fast || !skinVertex<true>(s, k, m, f))
            {
                s = saved;
                skinVertex<false>(s, k, m, f);
            }
            vi9 = i16(vi9 - 1);
            m.isw(vi11, 3, s.vi4);
            m.sq<XYZ>(vi11, s.vf3);
            m.sq<XYZW>(vi11 + 1, s.vf9);
            m.sq<XYZW>(vi11 + 2, s.vf2);
            vi11 = i16(vi11 + 3);
        } while (i16(vi9) > 0);

        Light l;
        l.vf2 = s.vf2, l.vf4 = s.vf4, l.vf5 = s.vf5, l.vf6 = s.vf6, l.vf7 = s.vf7, l.vf9 = s.vf9, l.acc = s.acc;
        l.vf8 = norm(s.vf8);
        l.vf10 = norm(load(state.vf[10]));
        l.vf11 = load(state.vf[11]);
        l.vf3 = fmacExact<Op::Add, Z>(f, s.vf3, zero, vf0, bc<3>(vf0));
        l.vi1 = state.vi[1], l.vi4 = s.vi4, l.adc = 0;
        l.q = state.q;
        l.clip = state.clip & 0xFFFFFFu;
        l.lastDi = 0u, l.stickyDi = 0u;
        vi9 = static_cast<int16_t>(laneBits(vf13, 0) & 0xFFFFu);
        const int32_t vi15 = i16(vi2 + 0xEE);
        l.vi10 = i16(vi2 + 1);
        m.sq<XYZW>(vi11, s.vf1);
        vi9 = i16(vi9 - k.vi12);
        vi11 = i16(vi2 + 0xEC);
        bool firstLight = true;
        do
        {
            const Light saved = l;
            if (firstLight || !fast || !lightVertex<true>(l, k, m, f))
            {
                l = saved;
                lightVertex<false>(l, k, m, f);
            }
            firstLight = false;
            vi9 = i16(vi9 - 1);
            m.isw(vi11 + 2, 3, l.adc);
            m.sq<XYZW>(vi11, l.vf7);
            m.sq<XYZW>(vi11 + 1, l.vf9);
            m.sq<XYZ>(vi11 + 2, l.vf6);
            vi11 = i16(vi11 + 3);
        } while (i16(vi9) > 0);

        Alu<false> o{f};
        l.acc = o.mul<XYZW>(l.acc, k.vf24, bc<2>(l.vf10));
        l.vi1 = l.vi1 | l.vi4;
        l.vf10 = o.madd<XYZW>(l.vf10, l.acc, k.vf24, bc<3>(l.vf10));
        l.vi1 = i16(l.vi1 + 0x7FFF);
        l.vf6 = ftoi<16, XYZW>(l.vf6, norm(l.vf5));
        l.vi1 = l.vi1 & k.vi12;
        l.vf7 = o.mul<XYZ>(l.vf7, norm(l.vf3), _mm_set1_ps(normalizeOperand(l.q)));
        l.vi1 = i16(l.vi1 + k.vi3);
        l.acc = o.mul<XYZW>(l.acc, l.vf8, norm(l.vf4));
        m.isw(vi11 + 2, 3, l.vi1);
        uint32_t mac = 0u, current = 0u;
        l.vf9 = maddLive(f, l.acc, l.vf10, _mm_set1_ps(128.0f), mac, current);
        m.sq<XYZW>(vi15, vf13);
        l.vf9 = miniv<XYZW>(l.vf9, l.vf9, bc<0>(k.vf17raw));
        m.sq<XYZ>(vi11 + 2, l.vf6);
        l.vf9 = ftoi<1, XYZW>(l.vf9, norm(l.vf9));
        m.sq<XYZW>(vi11, l.vf7);
        m.sq<XYZW>(vi11 + 1, l.vf9);
        uint64_t cycles = 33u + 43u * vertices + 10u * s.influences;
        if (i16(vi6) != 0)
        {
            vi6 = i16(vi6 + vi2);
            cycles += 2u * (kickPacket(data, vi6, kick, user) / 16u);
            vi6 = 0;
        }
        kickPacket(data, vi15, kick, user);

        store(state.vf[1], s.vf1), store(state.vf[2], l.vf2), store(state.vf[3], l.vf3), store(state.vf[4], l.vf4);
        store(state.vf[5], l.vf5), store(state.vf[6], l.vf6), store(state.vf[7], l.vf7), store(state.vf[8], l.vf8);
        store(state.vf[9], l.vf9), store(state.vf[10], l.vf10), store(state.vf[11], l.vf11), store(state.vf[13], vf13);
        store(state.acc, l.acc);
        state.vi[1] = l.vi1, state.vi[2] = vi2, state.vi[4] = l.vi4, state.vi[6] = vi6, state.vi[9] = vi9, state.vi[10] = l.vi10;
        state.vi[11] = vi11, state.vi[13] = s.vi13, state.vi[14] = s.vi14, state.vi[15] = vi15;
        state.q = l.q;
        state.i = 128.0f;
        state.clip = l.clip;
        state.mac = mac;
        uint32_t status = state.status;
        status = (status & 0xFF0u) | current | ((current | f.sticky) << 6);
        status = (status & 0xFCFu) | l.lastDi | (l.stickyDi << 6);
        state.status = status;
        state.top = top;
        state.itop = itop;
        state.pc = 0x15E8u;
        return cycles;
    }
}
