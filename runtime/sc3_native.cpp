#include "sc3_native.h"
#include "sc3_draw_sink.h"
#include "sc3_vu1_port.h"

#include "ps2_runtime.h"
#include "runtime/native/native_vif1.h"
#include "runtime/native/native_vu1_sandbox.h"
#include "runtime/native/native_vu1_stream.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_lifted.h"
#include "runtime/ps2_vu1_trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sc3
{
    namespace
    {
        using ps2x::native::Vu1StreamAdapter;

        constexpr uint32_t kElapsedLines = 0x00148098u;
        constexpr uint32_t kFrameEndReturn = 0x0012EA90u;
        constexpr size_t kCodeBytes = 16u * 1024u;

        struct KnownProgram
        {
            const char *name;
            uint32_t pairs;
            uint64_t hash;
        };

        constexpr KnownProgram kPrograms[] = {
            {"A", 1177u, 0x95e81f9a833c1b45ull},
            {"B", 1352u, 0xd2fbf7af837a8df4ull},
        };
        constexpr uint32_t kVuMemory = 16u * 1024u;

        enum class Mode
        {
            Observe,
            Sandbox,
        };

        enum class Port
        {
            Off,
            On,
            Check,
        };

        struct ProgramSet
        {
            std::set<std::pair<std::string, uint32_t>> entries;
            std::set<std::string> whole;

            bool empty() const { return entries.empty() && whole.empty(); }
            bool contains(const std::string &name, uint32_t pc) const
            {
                return whole.count(name) != 0u || entries.count({name, pc}) != 0u;
            }
        };

        struct Stats
        {
            std::unordered_map<uint64_t, uint64_t> calls, fallbacks, mismatched;
            uint64_t frames = 0, mscals = 0, resumed = 0, codeChanges = 0, sandboxed = 0, hidden = 0, ported = 0, checked = 0, mismatches = 0;
            uint64_t directChecked = 0, directMismatches = 0;
            uint64_t frameMin = ~0ull, frameMax = 0, frameCalls = 0;
        };

        struct State
        {
            Mode mode = Mode::Observe;
            bool sandbox = false;
            std::string switchPath;
            uint8_t *rdram = nullptr;
            PS2Runtime::RecompiledFunction elapsedLines = nullptr;
            uint64_t codeVersion = ~0ull;
            uint32_t program = 0;
            std::vector<std::string> names;
            std::vector<std::vector<uint8_t>> images;
            ProgramSet hidden, only;
            std::string hidePath, hideSpec;
            bool log = false;
            bool onVif1Thread = false;
            Port port = Port::Off;
            std::string portPath;
            Port direct = Port::Off;
            std::string directPath;
            bool portNext = false;
            uint32_t portProgram = 0;
            uint64_t portEntry = 0;
            ProgramSet portSkip;
            FILE *record = nullptr;
            std::unordered_map<uint64_t, uint64_t> recordSeen, recordKept;
            uint64_t recordCap = 2000u;
            uint32_t recordProgram = ~0u;
            bool recordNext = false;
            Stats stats;
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        };

        State g_state;

        uint64_t key(uint32_t program, uint32_t pc, uint32_t target = 0u)
        {
            return (static_cast<uint64_t>(program) << 48) | (static_cast<uint64_t>(target) << 16) | pc;
        }

        std::string keyName(uint64_t entry)
        {
            char name[64];
            const uint32_t target = static_cast<uint32_t>((entry >> 16) & 0xFFFFu);
            const std::string &program = g_state.names[entry >> 48];
            if (target != 0u)
                std::snprintf(name, sizeof(name), "%s:%#x>%#x", program.c_str(), static_cast<uint32_t>(entry & 0xFFFFu), target);
            else
                std::snprintf(name, sizeof(name), "%s:%#x", program.c_str(), static_cast<uint32_t>(entry & 0xFFFFu));
            return name;
        }

        bool sampleRecord(uint64_t entry)
        {
            State &s = g_state;
            if (!s.record)
                return false;
            const uint64_t n = ++s.recordSeen[entry];
            if (!(n <= 100u || (n <= 1000u && n % 10u == 0u) || n % 500u == 0u))
                return false;
            uint64_t &kept = s.recordKept[entry];
            if (kept >= s.recordCap)
                return false;
            ++kept;
            return true;
        }

        uint32_t residentProgram(const Vu1StreamAdapter &adapter)
        {
            State &s = g_state;
            if (adapter.codeVersion() == s.codeVersion)
                return s.program;
            s.codeVersion = adapter.codeVersion();
            ++s.stats.codeChanges;
            const uint8_t *code = adapter.code();
            for (uint32_t i = 0; i < std::size(kPrograms); ++i)
            {
                const KnownProgram &known = kPrograms[i];
                const size_t bytes = size_t{known.pairs} * 8u;
                std::vector<uint8_t> &image = s.images[i];
                const bool same = image.empty() ? VU1Interpreter::codeHash(code, static_cast<uint32_t>(bytes)) == known.hash
                                                : std::memcmp(code, image.data(), bytes) == 0;
                if (same)
                {
                    if (image.empty())
                        image.assign(code, code + bytes);
                    return s.program = i;
                }
            }
            char name[24];
            std::snprintf(name, sizeof(name), "%016llx",
                          static_cast<unsigned long long>(VU1Interpreter::codeHash(code, static_cast<uint32_t>(kCodeBytes))));
            const auto found = std::find(s.names.begin(), s.names.end(), name);
            if (found != s.names.end())
                return s.program = static_cast<uint32_t>(found - s.names.begin());
            s.names.emplace_back(name);
            return s.program = static_cast<uint32_t>(s.names.size() - 1u);
        }

        Vu1StreamAdapter::Choice choose(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            State &s = g_state;
            const uint32_t program = residentProgram(adapter);
            ++s.stats.mscals;
            ++s.stats.frameCalls;
            const uint64_t entry =
                key(program, pc, program < std::size(kPrograms) ? vu1DispatchTarget(program, pc, adapter.dataMemory(), adapter.top()) : 0u);
            if (s.log)
                ++s.stats.calls[entry];
            s.portNext = false;
            s.recordNext = false;
            if (!s.hidden.empty() && s.hidden.contains(s.names[program], pc))
            {
                ++s.stats.hidden;
                return Vu1StreamAdapter::Choice::Port;
            }
            if (s.sandbox && (s.only.empty() || s.only.contains(s.names[program], pc)))
            {
                if (s.port != Port::Off && program < std::size(kPrograms) && !s.portSkip.contains(s.names[program], pc) &&
                    vu1Ported(program, pc, adapter.dataMemory(), adapter.top()))
                {
                    ++s.stats.ported;
                    s.portNext = true;
                    s.portProgram = program;
                    s.portEntry = entry;
                    return Vu1StreamAdapter::Choice::Port;
                }
                ++s.stats.fallbacks[entry];
                if (s.direct == Port::On)
                    return Vu1StreamAdapter::Choice::Vu1;
                ++s.stats.sandboxed;
                if (sampleRecord(entry))
                {
                    s.recordNext = true;
                    return Vu1StreamAdapter::Choice::Port;
                }
                return Vu1StreamAdapter::Choice::Sandbox;
            }
            return Vu1StreamAdapter::Choice::Vu1;
        }

        void kickToGif(void *user, const uint8_t *packet, uint32_t bytes)
        {
            static_cast<ps2x::native::GifToNative *>(user)->packet(packet, bytes);
        }

        void kickToList(void *user, const uint8_t *packet, uint32_t bytes)
        {
            static_cast<std::vector<std::vector<uint8_t>> *>(user)->emplace_back(packet, packet + bytes);
        }

        uint64_t runPort(Vu1StreamAdapter &adapter, uint32_t pc, Vu1Kick kick, void *user)
        {
            VU1Interpreter &vu = adapter.sandbox().vu();
            Vu1Lifted::landQueued(vu);
            VU1State &state = vu.state();
            const uint64_t cycles =
                runVu1Program(g_state.portProgram, pc, state, adapter.dataMemory(), adapter.top(), adapter.itop(), kick, user);
            Vu1Lifted::setClip(vu, state.clip);
            Vu1Lifted::advanceTo(vu, Vu1Lifted::cycle(vu) + cycles);
            state.cycles = Vu1Lifted::cycle(vu);
            return cycles;
        }

        std::string stateDifference(const VU1State &port, const VU1State &vu1)
        {
            std::string out;
            char item[96];
            for (int r = 0; r < 32; ++r)
                if (std::memcmp(port.vf[r], vu1.vf[r], sizeof(port.vf[r])) != 0)
                {
                    std::snprintf(item, sizeof(item), " vf%d", r);
                    out += item;
                }
            for (int r = 0; r < 16; ++r)
                if (port.vi[r] != vu1.vi[r])
                {
                    std::snprintf(item, sizeof(item), " vi%d %#x/%#x", r, port.vi[r], vu1.vi[r]);
                    out += item;
                }
            if (std::memcmp(port.acc, vu1.acc, sizeof(port.acc)) != 0)
                out += " acc";
            const auto bitsOf = [](float value) { uint32_t b; std::memcpy(&b, &value, 4); return b; };
            if (bitsOf(port.q) != bitsOf(vu1.q) || bitsOf(port.i) != bitsOf(vu1.i) || bitsOf(port.p) != bitsOf(vu1.p))
                out += " q/i/p";
            if (port.clip != vu1.clip || port.mac != vu1.mac || port.status != vu1.status)
            {
                std::snprintf(item, sizeof(item), " clip %#x/%#x mac %#x/%#x status %#x/%#x", port.clip, vu1.clip, port.mac, vu1.mac,
                              port.status, vu1.status);
                out += item;
            }
            if (port.pc != vu1.pc)
                out += " pc";
            return out;
        }

        void checkPort(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            State &s = g_state;
            ps2x::native::Vu1Sandbox &sandbox = adapter.sandbox();
            VU1Interpreter &vu = sandbox.vu();
            uint8_t *data = adapter.dataMemory();
            static std::vector<uint8_t> s_before(kVuMemory), s_portData(kVuMemory);
            static std::vector<std::vector<uint8_t>> s_portPackets, s_packets;
            s_portPackets.clear();
            s_packets.clear();
            std::memcpy(s_before.data(), data, kVuMemory);
            Vu1Lifted::landQueued(vu);
            const VU1State before = vu.state();
            const uint64_t portCycles = runPort(adapter, pc, kickToList, &s_portPackets);
            const VU1State portState = vu.state();
            std::memcpy(s_portData.data(), data, kVuMemory);
            std::memcpy(data, s_before.data(), kVuMemory);
            const uint64_t clock = vu.state().cycles;
            vu.state() = before;
            vu.state().cycles = clock;
            Vu1Lifted::setClip(vu, before.clip);
            vu.setPacketObserver([&sandbox](const uint8_t *packet, uint32_t bytes)
                                 {
                                     s_packets.emplace_back(packet, packet + bytes);
                                     sandbox.gif().packet(packet, bytes);
                                 });
            const uint64_t cyclesBefore = sandbox.cycles();
            sandbox.call(const_cast<uint8_t *>(adapter.code()), kVuMemory, data, kVuMemory, pc, adapter.top(), adapter.itop());
            vu.setPacketObserver([&sandbox](const uint8_t *packet, uint32_t bytes) { sandbox.gif().packet(packet, bytes); });
            const uint64_t cycles = sandbox.cycles() - cyclesBefore;
            adapter.addPortCycles(cycles);
            ++s.stats.checked;
            std::string difference = stateDifference(portState, vu.state());
            if (s_portPackets != s_packets)
                difference += " packets";
            if (std::memcmp(s_portData.data(), data, kVuMemory) != 0)
                difference += " memory";
            if (portCycles != cycles)
                difference += " cycles " + std::to_string(portCycles) + "/" + std::to_string(cycles);
            if (difference.empty())
                return;
            ++s.stats.mismatches;
            ++s.stats.mismatched[s.portEntry];
            static int s_logged = 0;
            if (s_logged++ < 50)
                std::fprintf(stderr, "[sc3:vu1] port %s differs from the sandbox at TOP %#x:%s\n", keyName(s.portEntry).c_str(), adapter.top(),
                             difference.c_str());
            if (s.record)
            {
                Vu1TraceRecord record;
                record.startPC = pc;
                record.top = adapter.top();
                record.itop = adapter.itop();
                record.maxCycles = 1u << 24;
                record.code.assign(adapter.code(), adapter.code() + kCodeBytes);
                s.recordProgram = s.program;
                record.stateBefore = before;
                record.stateBefore.cycles = clock;
                record.dataBefore = s_before;
                record.stateAfter = vu.state();
                record.dataAfter.assign(data, data + kVuMemory);
                record.packets = s_packets;
                writeVu1TraceRecord(s.record, record);
            }
        }

        void recordSandbox(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            State &s = g_state;
            ps2x::native::Vu1Sandbox &sandbox = adapter.sandbox();
            VU1Interpreter &vu = sandbox.vu();
            uint8_t *data = adapter.dataMemory();
            Vu1TraceRecord record;
            record.startPC = pc;
            record.top = adapter.top();
            record.itop = adapter.itop();
            record.maxCycles = 1u << 24;
            if (s.recordProgram != s.program)
            {
                record.code.assign(adapter.code(), adapter.code() + kCodeBytes);
                s.recordProgram = s.program;
            }
            Vu1Lifted::landQueued(vu);
            record.stateBefore = vu.state();
            record.dataBefore.assign(data, data + kVuMemory);
            vu.setPacketObserver([&sandbox, &record](const uint8_t *packet, uint32_t bytes)
                                 {
                                     record.packets.emplace_back(packet, packet + bytes);
                                     sandbox.gif().packet(packet, bytes);
                                 });
            const uint64_t cyclesBefore = sandbox.cycles();
            sandbox.call(const_cast<uint8_t *>(adapter.code()), kVuMemory, data, kVuMemory, pc, adapter.top(), adapter.itop());
            vu.setPacketObserver([&sandbox](const uint8_t *packet, uint32_t bytes) { sandbox.gif().packet(packet, bytes); });
            adapter.addPortCycles(sandbox.cycles() - cyclesBefore);
            record.stateAfter = vu.state();
            record.dataAfter.assign(data, data + kVuMemory);
            writeVu1TraceRecord(s.record, record);
        }

        uint64_t runDirect(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            VU1Interpreter &vu = adapter.sandbox().vu();
            Vu1Lifted::landQueued(vu);
            VU1State &state = vu.state();
            const uint64_t cycles = drawVu1Program(g_state.portProgram, pc, state, adapter.dataMemory(), adapter.top(), adapter.itop(), drawSink());
            Vu1Lifted::setClip(vu, state.clip);
            Vu1Lifted::advanceTo(vu, Vu1Lifted::cycle(vu) + cycles);
            state.cycles = Vu1Lifted::cycle(vu);
            return cycles;
        }

        std::string drawDifference(const std::vector<DrawSink::Draw> &a, const std::vector<DrawSink::Draw> &b)
        {
            if (a.size() != b.size())
                return " draws " + std::to_string(a.size()) + "/" + std::to_string(b.size());
            for (size_t i = 0; i < a.size(); ++i)
            {
                const ps2x::native::GsMaterial &x = a[i].material, &y = b[i].material;
                const auto prim = [](const GSPrimReg &p)
                { return p.type | (p.iip << 3) | (p.tme << 4) | (p.fge << 5) | (p.abe << 6) | (p.aa1 << 7) | (p.fst << 8) | (p.ctxt << 9) | (p.fix << 10); };
                const auto tex0 = [](const GSTex0Reg &t)
                { return std::make_tuple(t.tbp0, t.tbw, t.psm, t.tw, t.th, t.tcc, t.tfx, t.cbp, t.cpsm, t.csm, t.csa, t.cld); };
                if (prim(x.prim) != prim(y.prim) || tex0(x.tex0) != tex0(y.tex0) || x.tex1 != y.tex1 || x.clamp != y.clamp ||
                    x.alpha != y.alpha || x.test != y.test || x.miptbp1 != y.miptbp1 || x.miptbp2 != y.miptbp2 || x.loadClut != y.loadClut)
                    return " material of draw " + std::to_string(i);
                if (a[i].triangles.size() != b[i].triangles.size())
                    return " vertices of draw " + std::to_string(i);
                for (size_t v = 0; v < a[i].triangles.size(); ++v)
                {
                    const ps2x::native::GsVertex &p = a[i].triangles[v], &q = b[i].triangles[v];
                    const auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
                    if (bits(p.x) != bits(q.x) || bits(p.y) != bits(q.y) || p.z != q.z || bits(p.s) != bits(q.s) || bits(p.t) != bits(q.t) ||
                        bits(p.q) != bits(q.q) || p.rgba != q.rgba || p.fog != q.fog)
                        return " vertex " + std::to_string(v) + " of draw " + std::to_string(i);
                }
            }
            return {};
        }

        void checkDirect(Vu1StreamAdapter &adapter, uint32_t pc)
        {
            State &s = g_state;
            VU1Interpreter &vu = adapter.sandbox().vu();
            Vu1Lifted::landQueued(vu);
            uint8_t *data = adapter.dataMemory();
            const uint32_t top = adapter.top(), itop = adapter.itop();
            static std::vector<uint8_t> s_directData(kVuMemory), s_packetData(kVuMemory);
            static std::vector<std::vector<uint8_t>> s_packets;
            std::memcpy(s_directData.data(), data, kVuMemory);
            std::memcpy(s_packetData.data(), data, kVuMemory);
            VU1State directState = vu.state(), packetState = vu.state();
            std::vector<DrawSink::Draw> direct, reference;
            DrawSink trial = drawSink();
            trial.capture(&direct);
            const uint64_t directCycles = drawVu1Program(s.portProgram, pc, directState, s_directData.data(), top, itop, trial);
            s_packets.clear();
            const uint64_t packetCycles =
                runVu1Program(s.portProgram, pc, packetState, s_packetData.data(), top, itop, kickToList, &s_packets);
            DrawSink &sink = drawSink();
            sink.capture(&reference);
            for (const std::vector<uint8_t> &packet : s_packets)
                sink.replay(packet.data(), packet.size());
            sink.capture(nullptr);
            ++s.stats.directChecked;
            std::string difference = stateDifference(directState, packetState) + drawDifference(direct, reference);
            if (directCycles != packetCycles)
                difference += " cycles " + std::to_string(directCycles) + "/" + std::to_string(packetCycles);
            for (uint32_t q = 0; q < 1024u; ++q)
            {
                const uint32_t offset = (q - top) & 0x3FFu;
                if (offset >= 0xE0u && offset < 0x19Fu)
                    continue;
                if (std::memcmp(s_directData.data() + q * 16u, s_packetData.data() + q * 16u, 16u) != 0)
                {
                    difference += " memory at " + std::to_string(q);
                    break;
                }
            }
            if (!difference.empty())
            {
                ++s.stats.directMismatches;
                ++s.stats.mismatched[s.portEntry];
                static int s_logged = 0;
                if (s_logged++ < 50)
                    std::fprintf(stderr, "[sc3:vu1] direct %s differs from its packets at TOP %#x:%s\n", keyName(s.portEntry).c_str(), top,
                                 difference.c_str());
            }
            ps2x::native::GifToNative &gif = adapter.sandbox().gif();
            adapter.addPortCycles(runPort(adapter, pc, kickToGif, &gif));
            gif.flush();
        }

        void drawPort(Vu1StreamAdapter &adapter, uint32_t pc, bool resumed)
        {
            State &s = g_state;
            if (resumed)
            {
                ++s.stats.resumed;
                return;
            }
            if (s.recordNext)
            {
                adapter.sandbox().gif().setSource(pc);
                recordSandbox(adapter, pc);
                return;
            }
            if (!s.portNext)
                return;
            ps2x::native::GifToNative &gif = adapter.sandbox().gif();
            gif.setSource(pc);
            if (s.port == Port::Check)
            {
                checkPort(adapter, pc);
                return;
            }
            if (s.direct == Port::On)
            {
                adapter.addPortCycles(runDirect(adapter, pc));
                return;
            }
            if (s.direct == Port::Check)
            {
                checkDirect(adapter, pc);
                return;
            }
            adapter.addPortCycles(runPort(adapter, pc, kickToGif, &gif));
            gif.flush();
        }

        Port parsePort(const std::string &value)
        {
            if (value == "check")
                return Port::Check;
            return !value.empty() && value != "0" ? Port::On : Port::Off;
        }

        void followDirectFile()
        {
            State &s = g_state;
            if (s.directPath.empty())
                return;
            std::ifstream file(s.directPath);
            std::string value;
            if (!std::getline(file, value))
                return;
            const Port direct = parsePort(value);
            if (direct != s.direct)
                std::cerr << "[sc3] native VU1: programs drawn "
                          << (direct == Port::Off ? "from their packets" : direct == Port::On ? "directly" : "directly, checked against their packets")
                          << " from the next frame" << std::endl;
            s.direct = direct;
        }

        void followPortFile()
        {
            State &s = g_state;
            if (s.portPath.empty())
                return;
            std::ifstream file(s.portPath);
            std::string value;
            if (!std::getline(file, value))
                return;
            const Port port = parsePort(value);
            if (port != s.port)
                std::cerr << "[sc3] native VU1: programs " << (port == Port::Off ? "on the sandbox" : port == Port::On ? "ported" : "checked")
                          << " from the next frame" << std::endl;
            s.port = port;
        }

        void report()
        {
            State &s = g_state;
            Stats &st = s.stats;
            ++st.frames;
            st.frameMin = std::min(st.frameMin, st.frameCalls);
            st.frameMax = std::max(st.frameMax, st.frameCalls);
            st.frameCalls = 0;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - s.since).count();
            if (seconds < 5.0)
                return;
            if (s.log)
            {
                std::vector<std::pair<uint64_t, uint64_t>> calls(st.calls.begin(), st.calls.end());
                std::sort(calls.begin(), calls.end());
                std::string line;
                for (const auto &[entry, count] : calls)
                    line += " " + keyName(entry) + " x" + std::to_string(count);
                std::fprintf(stderr,
                             "[sc3:vu1] %.1f s: %llu frames, mscal=%llu (per frame %.1f, %llu-%llu), code changes %llu, sandbox %llu, "
                             "hidden %llu, port %llu (checked %llu, mismatches %llu), MSCNT %llu;%s\n",
                             seconds, static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.mscals),
                             st.frames ? static_cast<double>(st.mscals) / static_cast<double>(st.frames) : 0.0,
                             static_cast<unsigned long long>(st.frameMin), static_cast<unsigned long long>(st.frameMax),
                             static_cast<unsigned long long>(st.codeChanges), static_cast<unsigned long long>(st.sandboxed),
                             static_cast<unsigned long long>(st.hidden), static_cast<unsigned long long>(st.ported),
                             static_cast<unsigned long long>(st.checked), static_cast<unsigned long long>(st.mismatches),
                             static_cast<unsigned long long>(st.resumed), line.c_str());
            }
            if (s.log && s.direct != Port::Off)
            {
                DrawSink &sink = drawSink();
                const DrawSink::Stats &d = sink.stats();
                std::fprintf(stderr,
                             "[sc3:vu1] %.1f s: direct kicks %llu, A+D blocks %llu, triangles %llu, foreign packets %llu, unknown registers %llu, "
                             "other primitives %llu, overflows %llu; checked %llu, mismatches %llu\n",
                             seconds, static_cast<unsigned long long>(d.kicks), static_cast<unsigned long long>(d.materials),
                             static_cast<unsigned long long>(d.triangles), static_cast<unsigned long long>(d.foreign),
                             static_cast<unsigned long long>(d.unknownRegisters), static_cast<unsigned long long>(d.badPrims),
                             static_cast<unsigned long long>(d.overflows), static_cast<unsigned long long>(st.directChecked),
                             static_cast<unsigned long long>(st.directMismatches));
                sink.resetStats();
            }
            if (!st.fallbacks.empty())
            {
                std::vector<std::pair<uint64_t, uint64_t>> fallbacks(st.fallbacks.begin(), st.fallbacks.end());
                std::sort(fallbacks.begin(), fallbacks.end());
                std::string line;
                uint64_t total = 0u;
                for (const auto &[entry, count] : fallbacks)
                {
                    line += " " + keyName(entry) + " x" + std::to_string(count);
                    total += count;
                }
                std::fprintf(stderr, "[sc3:vu1] %.1f s: %llu MSCALs without a C++ port, %s:%s\n", seconds, static_cast<unsigned long long>(total),
                             s.direct == Port::On ? "left on VU1" : "on the sandbox", line.c_str());
            }
            if (!st.mismatched.empty())
            {
                std::string line;
                for (const auto &[entry, count] : st.mismatched)
                    line += " " + keyName(entry) + " x" + std::to_string(count);
                std::fprintf(stderr, "[sc3:vu1] %.1f s: ports that differ from the sandbox:%s\n", seconds, line.c_str());
            }
            if (s.record)
                std::fflush(s.record);
            st = Stats{};
            s.since = now;
        }

        ProgramSet parsePrograms(const std::string &text)
        {
            ProgramSet set;
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(',', start);
                if (end == std::string::npos)
                    end = text.size();
                const std::string item = text.substr(start, end - start);
                start = end + 1;
                if (item.empty())
                    continue;
                const size_t colon = item.find(':');
                const std::string name = item.substr(0, colon);
                const std::string pc = colon == std::string::npos ? "*" : item.substr(colon + 1);
                if (pc == "*")
                    set.whole.insert(name);
                else
                    set.entries.insert({name, static_cast<uint32_t>(std::strtoul(pc.c_str(), nullptr, 16))});
            }
            return set;
        }

        void followHideFile()
        {
            State &s = g_state;
            if (s.hidePath.empty())
                return;
            std::ifstream file(s.hidePath);
            std::string spec;
            std::getline(file, spec);
            if (spec == s.hideSpec)
                return;
            s.hideSpec = spec;
            s.hidden = parsePrograms(spec);
            std::cerr << "[sc3] native VU1: hiding '" << spec << "' from the next frame" << std::endl;
        }

        void followSwitch()
        {
            State &s = g_state;
            if (s.switchPath.empty())
                return;
            std::ifstream file(s.switchPath);
            char value = 0;
            if (!(file >> value))
                return;
            const bool sandbox = value == '1';
            if (sandbox != s.sandbox)
                std::cerr << "[sc3] native VU1: " << (sandbox ? "sandbox" : "VU1") << " from the next frame" << std::endl;
            s.sandbox = sandbox;
        }

        void frameEndElapsedLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (getRegU32(ctx, 31) == kFrameEndReturn)
            {
                auto frameEnd = []
                {
                    followSwitch();
                    followHideFile();
                    followPortFile();
                    followDirectFile();
                    report();
                    Vu1StreamAdapter::instance().frameEnd();
                };
                if (g_state.onVif1Thread)
                    ps2x::native::runOnVif1Thread(frameEnd);
                else
                    frameEnd();
            }
            g_state.elapsedLines(rdram, ctx, runtime);
        }

        ps2x::native::Screen screen()
        {
            ps2x::native::Screen screen;
            screen.width = 640.0f;
            screen.height = 448.0f;
            screen.originX = 2048.0f - screen.width / 2.0f;
            screen.originY = 2048.0f - screen.height / 2.0f;
            return screen;
        }
    }

    void installNativeVu1(PS2Runtime &runtime, bool nativeGs)
    {
        const char *value = std::getenv("SC3_NATIVE_VU1");
        const char *hide = std::getenv("SC3_NATIVE_VU1_HIDE");
        const bool unset = !value || !*value;
        const bool on = unset ? nativeGs : *value != '0';
        if (!on && !(hide && *hide))
            return;
        State &s = g_state;
        s.mode = on && (unset || std::strcmp(value, "1") == 0) ? Mode::Sandbox : Mode::Observe;
        s.sandbox = s.mode == Mode::Sandbox;
        if (const char *path = std::getenv("SC3_NATIVE_VU1_SWITCH"); path && *path && s.mode == Mode::Sandbox)
        {
            s.switchPath = path;
            followSwitch();
        }
        s.rdram = runtime.memory().getRDRAM();
        for (const KnownProgram &known : kPrograms)
            s.names.emplace_back(known.name);
        s.images.resize(std::size(kPrograms));
        if (hide && *hide == '@')
        {
            s.hidePath = hide + 1;
            followHideFile();
        }
        else if (hide)
            s.hidden = parsePrograms(hide);
        if (const char *only = std::getenv("SC3_NATIVE_VU1_ONLY"); only && *only)
            s.only = parsePrograms(only);
        if (const char *skip = std::getenv("SC3_NATIVE_VU1_PORT_SKIP"); skip && *skip)
            s.portSkip = parsePrograms(skip);
        if (const char *path = std::getenv("SC3_NATIVE_VU1_RECORD"); path && *path)
        {
            s.record = std::fopen(path, "wb");
            if (const char *cap = std::getenv("SC3_NATIVE_VU1_RECORD_CAP"); cap && *cap)
                s.recordCap = std::strtoull(cap, nullptr, 0);
        }
        const char *log = std::getenv("SC3_NATIVE_VU1_LOG");
        s.log = (log && *log == '1') || s.mode == Mode::Observe;
        s.elapsedLines = runtime.lookupFunction(kElapsedLines);
        if (!s.elapsedLines)
        {
            std::cerr << "[sc3] SC3_NATIVE_VU1: no function at 0x148098, the native frame would never end" << std::endl;
            return;
        }
        runtime.registerFunction(kElapsedLines, frameEndElapsedLines);
        Vu1StreamAdapter::Config config;
        config.screen = screen();
        config.choose = choose;
        config.draw = drawPort;
        if (const char *port = std::getenv("SC3_NATIVE_VU1_PORT"); port && *port == '@')
        {
            s.portPath = port + 1;
            followPortFile();
        }
        else
            s.port = port && *port ? parsePort(port) : Port::On;
        if (const char *direct = std::getenv("SC3_NATIVE_VU1_DIRECT"); direct && *direct == '@')
        {
            s.directPath = direct + 1;
            followDirectFile();
        }
        else
            s.direct = direct && *direct ? parsePort(direct) : s.port == Port::On ? Port::On : Port::Off;
        const bool packets = s.direct != Port::On || !s.directPath.empty();
        config.directs = s.mode == Mode::Sandbox && packets;
        const char *gsWrites = std::getenv("SC3_NATIVE_VU1_GSWRITES");
        config.gsWrites = nativeGs && (gsWrites && *gsWrites ? *gsWrites != '0' : packets);
        if (s.mode == Mode::Sandbox)
            config.active = [] { return g_state.sandbox; };
        const char *thread = std::getenv("SC3_NATIVE_VU1_THREAD");
        s.onVif1Thread = runtime.memory().vif1Threaded() && !(thread && *thread == '0');
        config.onVif1Thread = s.onVif1Thread;
        Vu1StreamAdapter::instance().install(config);
        std::cerr << "[sc3] native VU1: "
                  << (s.mode == Mode::Sandbox ? "every program off VU1" : "observed, every program on VU1")
                  << (s.hidden.empty() && s.hidePath.empty() ? "" : ", some hidden")
                  << (s.only.empty() ? "" : ", only some on the sandbox")
                  << (s.port == Port::On ? ", C++ ports in the sandbox's place" : s.port == Port::Check ? ", C++ ports checked against the sandbox" : "")
                  << (s.direct == Port::On ? ", drawn directly" : s.direct == Port::Check ? ", drawn from packets, checked against direct draws" : "")
                  << (!packets ? "" : config.gsWrites ? ", packets drawn by the GS back end" : ", packets drawn by the native renderer")
                  << (s.onVif1Thread ? ", on the VIF1 thread" : ", on the game thread") << std::endl;
    }
}
