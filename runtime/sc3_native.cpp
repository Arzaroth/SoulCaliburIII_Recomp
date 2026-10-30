#include "sc3_native.h"
#include "sc3_vu1_port.h"

#include "ps2_runtime.h"
#include "runtime/native/native_vif1.h"
#include "runtime/native/native_vu1_sandbox.h"
#include "runtime/native/native_vu1_stream.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_lifted.h"

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
        constexpr uint32_t kProgramB = 1u;
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
            std::unordered_map<uint64_t, uint64_t> calls;
            uint64_t frames = 0, mscals = 0, codeChanges = 0, sandboxed = 0, hidden = 0, ported = 0, checked = 0, mismatches = 0;
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
            bool portNext = false;
            Stats stats;
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        };

        State g_state;

        uint64_t key(uint32_t program, uint32_t pc) { return (static_cast<uint64_t>(program) << 32) | pc; }

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
            if (s.log)
                ++s.stats.calls[key(program, pc)];
            s.portNext = false;
            if (!s.hidden.empty() && s.hidden.contains(s.names[program], pc))
            {
                ++s.stats.hidden;
                return Vu1StreamAdapter::Choice::Port;
            }
            if (s.sandbox && (s.only.empty() || s.only.contains(s.names[program], pc)))
            {
                if (s.port != Port::Off && program == kProgramB && pc == kSkinnedBodyPc &&
                    skinnedBodyReady(adapter.sandbox().vu().state()))
                {
                    ++s.stats.ported;
                    s.portNext = true;
                    return Vu1StreamAdapter::Choice::Port;
                }
                ++s.stats.sandboxed;
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

        uint64_t runPort(Vu1StreamAdapter &adapter, Vu1Kick kick, void *user)
        {
            VU1Interpreter &vu = adapter.sandbox().vu();
            Vu1Lifted::landQueued(vu);
            VU1State &state = vu.state();
            const uint64_t cycles = runSkinnedBody(state, adapter.dataMemory(), adapter.top(), adapter.itop(), kick, user);
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
            const uint64_t portCycles = runPort(adapter, kickToList, &s_portPackets);
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
            static int s_logged = 0;
            if (s_logged++ < 20)
                std::fprintf(stderr, "[sc3:vu1] port B:%#x differs from the sandbox at TOP %#x:%s\n", pc, adapter.top(), difference.c_str());
        }

        void drawPort(Vu1StreamAdapter &adapter, uint32_t pc, bool)
        {
            State &s = g_state;
            if (!s.portNext)
                return;
            ps2x::native::GifToNative &gif = adapter.sandbox().gif();
            gif.setSource(pc);
            if (s.port == Port::Check)
            {
                checkPort(adapter, pc);
                return;
            }
            adapter.addPortCycles(runPort(adapter, kickToGif, &gif));
            gif.flush();
        }

        Port parsePort(const std::string &value)
        {
            if (value == "check")
                return Port::Check;
            return !value.empty() && value != "0" ? Port::On : Port::Off;
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
                std::cerr << "[sc3] native VU1: B:0x1388 " << (port == Port::Off ? "on the sandbox" : port == Port::On ? "ported" : "checked")
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
                for (const auto &[programPc, count] : calls)
                {
                    char item[64];
                    std::snprintf(item, sizeof(item), " %s:%#x x%llu", s.names[programPc >> 32].c_str(),
                                  static_cast<uint32_t>(programPc), static_cast<unsigned long long>(count));
                    line += item;
                }
                std::fprintf(stderr,
                             "[sc3:vu1] %.1f s: %llu frames, mscal=%llu (per frame %.1f, %llu-%llu), code changes %llu, sandbox %llu, "
                             "hidden %llu, port %llu (checked %llu, mismatches %llu);%s\n",
                             seconds, static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.mscals),
                             st.frames ? static_cast<double>(st.mscals) / static_cast<double>(st.frames) : 0.0,
                             static_cast<unsigned long long>(st.frameMin), static_cast<unsigned long long>(st.frameMax),
                             static_cast<unsigned long long>(st.codeChanges), static_cast<unsigned long long>(st.sandboxed),
                             static_cast<unsigned long long>(st.hidden), static_cast<unsigned long long>(st.ported),
                             static_cast<unsigned long long>(st.checked), static_cast<unsigned long long>(st.mismatches), line.c_str());
            }
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
        config.directs = s.mode == Mode::Sandbox;
        const char *gsWrites = std::getenv("SC3_NATIVE_VU1_GSWRITES");
        config.gsWrites = nativeGs && !(gsWrites && *gsWrites == '0');
        if (s.mode == Mode::Sandbox)
            config.active = [] { return g_state.sandbox; };
        const char *thread = std::getenv("SC3_NATIVE_VU1_THREAD");
        s.onVif1Thread = runtime.memory().vif1Threaded() && !(thread && *thread == '0');
        config.onVif1Thread = s.onVif1Thread;
        Vu1StreamAdapter::instance().install(config);
        std::cerr << "[sc3] native VU1: "
                  << (s.mode == Mode::Sandbox ? "every program on the sandbox" : "observed, every program on VU1")
                  << (s.hidden.empty() && s.hidePath.empty() ? "" : ", some hidden")
                  << (s.only.empty() ? "" : ", only some on the sandbox")
                  << (s.port == Port::On ? ", B:0x1388 ported" : s.port == Port::Check ? ", B:0x1388 checked against the port" : "")
                  << (config.gsWrites ? ", drawn by the GS back end" : ", drawn by the native renderer")
                  << (s.onVif1Thread ? ", on the VIF1 thread" : ", on the game thread") << std::endl;
    }
}
