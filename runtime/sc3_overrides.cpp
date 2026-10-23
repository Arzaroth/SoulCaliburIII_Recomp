#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
    // crt0 calls SetupThread(stack 0x01FDFF00, size 0x20000) then SetupHeap(end, -1): the heap
    // runs up to the main stack and the game allocates nearly all of it. Move the runtime's HLE
    // pools and invocation stacks into kernel RAM above the syscall-table mirror instead.
    constexpr uint32_t kReservedBase = 0x00080000u;
    constexpr uint32_t kReservedTop = 0x00100000u;
    constexpr uint32_t kMainStackBottom = 0x01FDFF00u;

    // CRI Sofdec decodes through the IPU, which the runtime does not emulate: report every movie
    // as finished so the game moves past the opening and attract movies.
    // Sofdec drives the IPU, which the runtime does not emulate. Movies are SFD files (MPEG-PS with
    // MPEG video and ADX audio) inside MOVIE.AFS, decoded here on the host per Sofdec handle.
    // Full-screen movies replace the GS output and mix their soundtrack into the SPU2 output, whose
    // length is the movie clock; smaller ones are menu textures handed out by mwPlyGetCurFrm.
    class SfdPlayer
    {
    public:
        static constexpr uint32_t kRate = 48000u;
        static constexpr uint32_t kFullScreenWidth = 512u;

        ~SfdPlayer() { stop(); }

        bool start(const std::string &path, uint64_t offset, uint64_t size, bool loop)
        {
            m_file.open(path, std::ios::binary);
            if (!m_file)
                return false;
            m_base = offset;
            m_size = size;
            m_loop = loop;
            m_thread = std::thread([this]
                                   { decode(); });
            return true;
        }

        void stop()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_quit = true;
                m_active = false;
            }
            m_wake.notify_all();
            if (m_thread.joinable())
                m_thread.join();
        }

        bool active()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            updateEndLocked();
            return m_active;
        }

        void mix(int16_t *frames, size_t count)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_active)
                return;
            const size_t available = std::min(count, m_audio.size() / 2u);
            for (size_t i = 0; i < available * 2u; ++i)
            {
                const int32_t sum = static_cast<int32_t>(frames[i]) + m_audio[i];
                frames[i] = static_cast<int16_t>(std::clamp(sum, -32768, 32767));
            }
            m_audio.erase(m_audio.begin(), m_audio.begin() + static_cast<std::ptrdiff_t>(available * 2u));
            m_clock += (m_hasAudio && !m_decoderDone) ? available : count;
            updateEndLocked();
            m_wake.notify_all();
        }

        bool present(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_active || !m_fullScreen)
                return false;
            advanceLocked();
            if (m_current.empty())
            {
                rgba.assign(640u * 448u * 4u, 0u);
                width = 640u;
                height = 448u;
                return true;
            }
            rgba = m_current;
            width = m_width;
            height = m_height;
            return true;
        }

        // The current frame as IPU CSC output: RGBA32 16x16 macroblocks, column by column. Alpha
        // movies carry their alpha as the luma of the frame's bottom half.
        bool currentMacroblocks(uint8_t *out, uint32_t capacity, bool alphaMovie, uint32_t &width, uint32_t &height, uint32_t &number)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_active || m_fullScreen)
                return false;
            advanceLocked();
            const uint32_t outHeight = alphaMovie ? m_height / 2u : m_height;
            if (m_current.empty() || (m_width % 16u) != 0u || (outHeight % 16u) != 0u ||
                static_cast<uint64_t>(m_width) * outHeight * 4u > capacity)
                return false;
            width = m_width;
            height = outHeight;
            number = m_number;
            if (m_written == m_number)
                return true;
            m_written = m_number;
            uint8_t *dst = out;
            for (uint32_t mbx = 0; mbx < m_width / 16u; ++mbx)
                for (uint32_t y = 0; y < outHeight; ++y, dst += 64)
                {
                    const uint8_t *src = m_current.data() + (static_cast<size_t>(y) * m_width + mbx * 16u) * 4u;
                    const uint8_t *mask = src + static_cast<size_t>(outHeight) * m_width * 4u;
                    for (uint32_t x = 0; x < 16u; ++x)
                    {
                        std::memcpy(dst + x * 4u, src + x * 4u, 3);
                        const uint32_t luma = (mask[x * 4u] * 77u + mask[x * 4u + 1u] * 150u + mask[x * 4u + 2u] * 29u) >> 8;
                        dst[x * 4u + 3u] = alphaMovie ? static_cast<uint8_t>((luma * 0x80u + 127u) / 255u) : 0x80u;
                    }
                }
            return true;
        }

    private:
        struct Frame
        {
            double pts = 0.0;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> rgba;
        };

        void advanceLocked()
        {
            const double now = static_cast<double>(m_clock) / kRate;
            bool popped = false;
            while (!m_frames.empty() && m_frames.front().pts <= now)
            {
                m_current.swap(m_frames.front().rgba);
                m_width = m_frames.front().width;
                m_height = m_frames.front().height;
                m_frames.pop_front();
                ++m_number;
                popped = true;
            }
            if (popped)
                m_wake.notify_all();
        }

        void updateEndLocked()
        {
            if (m_active && m_decoderDone && m_frames.empty() && m_audio.empty())
                m_active = false;
        }

        static int readPacket(void *opaque, uint8_t *buffer, int size)
        {
            auto *self = static_cast<SfdPlayer *>(opaque);
            const uint64_t left = self->m_size - self->m_position;
            if (left == 0u)
                return AVERROR_EOF;
            const auto wanted = static_cast<std::streamsize>(std::min<uint64_t>(left, static_cast<uint64_t>(size)));
            self->m_file.clear();
            self->m_file.seekg(static_cast<std::streamoff>(self->m_base + self->m_position));
            self->m_file.read(reinterpret_cast<char *>(buffer), wanted);
            const auto got = self->m_file.gcount();
            if (got <= 0)
                return AVERROR_EOF;
            self->m_position += static_cast<uint64_t>(got);
            return static_cast<int>(got);
        }

        static int64_t seekPacket(void *opaque, int64_t offset, int whence)
        {
            auto *self = static_cast<SfdPlayer *>(opaque);
            if (whence == AVSEEK_SIZE)
                return static_cast<int64_t>(self->m_size);
            int64_t target = offset;
            if ((whence & ~AVSEEK_FORCE) == SEEK_CUR)
                target += static_cast<int64_t>(self->m_position);
            else if ((whence & ~AVSEEK_FORCE) == SEEK_END)
                target += static_cast<int64_t>(self->m_size);
            if (target < 0 || static_cast<uint64_t>(target) > self->m_size)
                return -1;
            self->m_position = static_cast<uint64_t>(target);
            return target;
        }

        bool waitForRoom()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]
                        {
                            const size_t audioFrames = m_audio.size() / 2u;
                            const bool full = (m_frames.size() >= 8u && (!m_hasAudio || audioFrames >= kRate / 2u)) ||
                                              m_frames.size() >= 48u || audioFrames >= kRate * 8u;
                            return m_quit || !full; });
            return !m_quit;
        }

        // Decodes the file once; returns the pts just past its last video frame.
        double decodePass(double ptsBase)
        {
            m_position = 0;
            AVFormatContext *format = avformat_alloc_context();
            auto *ioBuffer = static_cast<uint8_t *>(av_malloc(64u * 1024u));
            AVIOContext *io = avio_alloc_context(ioBuffer, 64 * 1024, 0, this, &SfdPlayer::readPacket, nullptr, &SfdPlayer::seekPacket);
            format->pb = io;
            AVCodecContext *video = nullptr;
            AVCodecContext *audio = nullptr;
            SwsContext *scaler = nullptr;
            SwrContext *resampler = nullptr;
            int videoIndex = -1;
            int audioIndex = -1;
            double end = ptsBase;
            if (avformat_open_input(&format, nullptr, nullptr, nullptr) == 0 && avformat_find_stream_info(format, nullptr) >= 0)
            {
                for (unsigned i = 0; i < format->nb_streams; ++i)
                {
                    const AVCodecParameters *parameters = format->streams[i]->codecpar;
                    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO && videoIndex < 0)
                        videoIndex = static_cast<int>(i);
                    else if (parameters->codec_id == AV_CODEC_ID_ADPCM_ADX && audioIndex < 0)
                        audioIndex = static_cast<int>(i);
                }
                const auto openDecoder = [&](int index) -> AVCodecContext *
                {
                    if (index < 0)
                        return nullptr;
                    const AVCodecParameters *parameters = format->streams[index]->codecpar;
                    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
                    AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : nullptr;
                    if (context && (avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0))
                        avcodec_free_context(&context);
                    return context;
                };
                video = openDecoder(videoIndex);
                audio = openDecoder(audioIndex);
                if (audio)
                {
                    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, kRate, &audio->ch_layout,
                                            audio->sample_fmt, audio->sample_rate, 0, nullptr) < 0 ||
                        swr_init(resampler) < 0)
                        swr_free(&resampler);
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_hasAudio = resampler != nullptr;
                if (video)
                    m_fullScreen = static_cast<uint32_t>(video->width) >= kFullScreenWidth;
            }

            AVPacket *packet = av_packet_alloc();
            AVFrame *frame = av_frame_alloc();
            const AVStream *stream = video ? format->streams[videoIndex] : nullptr;
            const double videoStart = stream && stream->start_time != AV_NOPTS_VALUE ? stream->start_time * av_q2d(stream->time_base) : 0.0;
            const double frameTime = stream && stream->avg_frame_rate.num != 0 ? 1.0 / av_q2d(stream->avg_frame_rate) : 1001.0 / 30000.0;
            const auto drainVideo = [&]
            {
                while (avcodec_receive_frame(video, frame) == 0)
                {
                    scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                  frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    Frame out;
                    out.width = static_cast<uint32_t>(frame->width);
                    out.height = static_cast<uint32_t>(frame->height);
                    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4u);
                    uint8_t *planes[1] = {out.rgba.data()};
                    const int strides[1] = {static_cast<int>(out.width * 4u)};
                    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                    out.pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                  ? ptsBase + frame->best_effort_timestamp * av_q2d(stream->time_base) - videoStart
                                  : end;
                    end = std::max(end, out.pts + frameTime);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_frames.push_back(std::move(out));
                }
            };
            const auto drainAudio = [&]
            {
                while (avcodec_receive_frame(audio, frame) == 0)
                {
                    const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                    std::vector<int16_t> pcm(static_cast<size_t>(std::max(capacity, 0)) * 2u);
                    uint8_t *planes[1] = {reinterpret_cast<uint8_t *>(pcm.data())};
                    const int produced = swr_convert(resampler, planes, capacity, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
                    if (produced <= 0)
                        continue;
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_audio.insert(m_audio.end(), pcm.begin(), pcm.begin() + produced * 2);
                }
            };
            while (video && waitForRoom() && av_read_frame(format, packet) >= 0)
            {
                if (packet->stream_index == videoIndex && avcodec_send_packet(video, packet) >= 0)
                    drainVideo();
                else if (resampler && packet->stream_index == audioIndex && avcodec_send_packet(audio, packet) >= 0)
                    drainAudio();
                av_packet_unref(packet);
            }
            if (video && !m_quit)
            {
                avcodec_send_packet(video, nullptr);
                drainVideo();
            }
            if (!video)
                std::cerr << "[sc3] movie: cannot decode the SFD" << std::endl;
            av_frame_free(&frame);
            av_packet_free(&packet);
            sws_freeContext(scaler);
            swr_free(&resampler);
            avcodec_free_context(&video);
            avcodec_free_context(&audio);
            avformat_close_input(&format);
            if (io)
            {
                av_freep(&io->buffer);
                avio_context_free(&io);
            }
            return end;
        }

        void decode()
        {
            double ptsBase = 0.0;
            for (;;)
            {
                const double end = decodePass(ptsBase);
                if (!m_loop || m_quit || end <= ptsBase)
                    break;
                ptsBase = end;
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_decoderDone = true;
        }

        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::ifstream m_file;
        uint64_t m_base = 0;
        uint64_t m_size = 0;
        uint64_t m_position = 0;
        uint64_t m_clock = 0;
        std::deque<Frame> m_frames;
        std::deque<int16_t> m_audio;
        std::vector<uint8_t> m_current;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_number = 0;
        uint32_t m_written = ~0u;
        bool m_loop = false;
        bool m_fullScreen = false;
        bool m_decoderDone = false;
        bool m_hasAudio = false;
        std::atomic<bool> m_quit{false};
        bool m_active = true;
    };

    struct MoviePlayers
    {
        std::mutex mutex;
        std::map<uint32_t, std::shared_ptr<SfdPlayer>> players;

        std::shared_ptr<SfdPlayer> find(uint32_t handle)
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = players.find(handle);
            return it != players.end() ? it->second : nullptr;
        }

        std::vector<std::shared_ptr<SfdPlayer>> all()
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<std::shared_ptr<SfdPlayer>> result;
            for (const auto &entry : players)
                result.push_back(entry.second);
            return result;
        }

        void stop(uint32_t handle)
        {
            std::shared_ptr<SfdPlayer> player;
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = players.find(handle);
                if (it == players.end())
                    return;
                player = std::move(it->second);
                players.erase(it);
            }
            player->stop();
        }

        void set(uint32_t handle, std::shared_ptr<SfdPlayer> player)
        {
            stop(handle);
            std::lock_guard<std::mutex> lock(mutex);
            players[handle] = std::move(player);
        }
    };

    MoviePlayers g_movies;

    bool afsEntry(const std::string &path, uint32_t index, uint64_t &offset, uint64_t &size)
    {
        std::ifstream file(path, std::ios::binary);
        uint32_t header[2] = {};
        if (!file.read(reinterpret_cast<char *>(header), sizeof(header)) || std::memcmp(header, "AFS", 4) != 0 || index >= header[1])
            return false;
        uint32_t entry[2] = {};
        file.seekg(8 + 8 * static_cast<std::streamoff>(index));
        if (!file.read(reinterpret_cast<char *>(entry), sizeof(entry)))
            return false;
        offset = entry[0];
        size = entry[1];
        return size != 0u;
    }

    constexpr uint32_t kMwPlyGetStat = 0x003AE9F8u;
    constexpr int32_t kMwPlyStatPlaying = 2;
    constexpr int32_t kMwPlyStatPlayEnd = 3;

    void mwPlyGetStatPlayEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            std::cerr << "[sc3] mwPlyGetStat follows the host movie player" << std::endl;
        }
        const auto player = g_movies.find(getRegU32(ctx, 4));
        setReturnS32(ctx, player && player->active() ? kMwPlyStatPlaying : kMwPlyStatPlayEnd);
        ctx->pc = getRegU32(ctx, 31);
    }

    constexpr uint32_t kMwPlyGetCurFrm = 0x003AB630u;
    constexpr uint32_t kMwPlyRelCurFrm = 0x003ABF50u;
    constexpr uint32_t kMwPlyIsNextFrmReady = 0x003AC008u;
    constexpr uint32_t kMwPlyStop = 0x003AD870u;
    constexpr uint32_t kMwPlyCompoMode = 0x08u;
    constexpr uint32_t kMwPlyCompoAlpha = 3u;
    constexpr uint32_t kMwPlyWork = 0x20u;
    constexpr uint32_t kMwPlySfdHandle = 0x3Cu;
    constexpr uint32_t kFrameObjectSize = 0x90u;

    // The frame goes into the player's work area, whose stream and picture buffers are unused
    // since the Sofdec decoder never runs; the SFD handle sits at the end of that area.
    void mwPlyGetCurFrmHost(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = getRegU32(ctx, 4);
        const uint32_t frame = getRegU32(ctx, 5) & PS2_RAM_MASK;
        const auto player = g_movies.find(handle);
        if (!player)
        {
            sub_003AB630_0x3ab630(rdram, ctx, runtime);
            return;
        }
        PS2Memory &memory = runtime->memory();
        const uint32_t buffer = (memory.read32(handle + kMwPlyWork) + 63u) & ~63u;
        const uint32_t limit = memory.read32(handle + kMwPlySfdHandle);
        const uint32_t capacity = limit > buffer ? limit - buffer : 0u;
        uint32_t width = 0, height = 0, number = 0;
        std::memset(rdram + frame, 0, kFrameObjectSize);
        const bool alphaMovie = memory.read32(handle + kMwPlyCompoMode) == kMwPlyCompoAlpha;
        if (static std::map<uint32_t, int> traced; std::getenv("PS2X_SC3_TRACE_MOVIE") && traced[handle] < 3)
        {
            ++traced[handle];
            std::cerr << "[sc3] mwPlyGetCurFrm handle=0x" << std::hex << handle << " buffer=0x" << buffer << " limit=0x" << limit
                      << " ra=0x" << getRegU32(ctx, 31) << std::dec << " alpha=" << alphaMovie << " active=" << player->active() << std::endl;
        }
        if (capacity != 0u && player->currentMacroblocks(rdram + (buffer & PS2_RAM_MASK), capacity, alphaMovie, width, height, number))
        {
            if (std::getenv("PS2X_SC3_TRACE_MOVIE") && number % 60u == 1u)
                std::cerr << "[sc3] movie frame handle=0x" << std::hex << handle << " buffer=0x" << buffer << std::dec << " " << width
                          << "x" << height << " alpha=" << alphaMovie << " number=" << number << " ra=0x" << std::hex
                          << getRegU32(ctx, 31) << std::dec << std::endl;
            const uint32_t fields[][2] = {{0x00u, buffer}, {0x04u, number}, {0x0Cu, width}, {0x10u, height}, {0x14u, width}, {0x18u, height}, {0x24u, number}};
            for (const auto &field : fields)
                std::memcpy(rdram + frame + field[0], &field[1], 4);
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    void mwPlyRelCurFrmHost(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_movies.find(getRegU32(ctx, 4)))
        {
            sub_003ABF50_0x3abf50(rdram, ctx, runtime);
            return;
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    void mwPlyIsNextFrmReadyHost(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_movies.find(getRegU32(ctx, 4)))
        {
            sub_003AC008_0x3ac008(rdram, ctx, runtime);
            return;
        }
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
    }

    void mwPlyStopHost(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_movies.stop(getRegU32(ctx, 4));
        sub_003AD870_0x3ad870(rdram, ctx, runtime);
    }

    constexpr uint32_t kAdxtGetStat = 0x003F86D0u;

    void adxtGetStatTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<uint32_t, int32_t> lastStatus;
        const uint32_t handle = getRegU32(ctx, 4);
        const uint32_t caller = getRegU32(ctx, 31);
        sub_003F86D0_0x3f86d0(rdram, ctx, runtime);
        const int32_t status = static_cast<int32_t>(getRegU32(ctx, 2));
        const auto [it, inserted] = lastStatus.emplace(handle, status);
        if (inserted || it->second != status)
        {
            it->second = status;
            std::cerr << "[sc3] adxt_GetStat handle=0x" << std::hex << handle << " ra=0x" << caller << std::dec
                      << " status=" << status << std::endl;
        }
    }

    constexpr uint32_t kAdxfGetPtStat = 0x003ECD20u;

    void adxfGetPtStatTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::map<uint32_t, int32_t> lastStatus;
        const uint32_t partition = getRegU32(ctx, 4);
        sub_003ECD20_0x3ecd20(rdram, ctx, runtime);
        const int32_t status = static_cast<int32_t>(getRegU32(ctx, 2));
        const auto [it, inserted] = lastStatus.emplace(partition, status);
        if (inserted || it->second != status)
        {
            it->second = status;
            std::cerr << "[sc3] ADXF_GetPtStat ptid=" << partition << " status=" << status << std::endl;
        }
    }

    // Recompiled functions return early at scheduler checkpoints with ctx->pc inside the body,
    // so tracing wrappers only inspect state on entry.
    constexpr uint32_t kInitAfsUpdate = 0x002029C0u;

    void initAfsUpdateTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t lastKey = 0xFFFFFFFFu;
        const uint32_t task = getRegU32(ctx, 4);
        uint32_t phase = 0, index = 0, table = 0;
        if (task && (task & PS2_RAM_MASK) + 0x60u <= PS2_RAM_SIZE)
        {
            std::memcpy(&phase, rdram + (task & PS2_RAM_MASK) + 0x54u, 4);
            std::memcpy(&index, rdram + (task & PS2_RAM_MASK) + 0x58u, 4);
            std::memcpy(&table, rdram + (task & PS2_RAM_MASK) + 0x5Cu, 4);
        }
        const uint32_t key = (phase << 16) ^ index;
        if (key != lastKey)
        {
            lastKey = key;
            int32_t ptid = -1;
            if (table && (table & PS2_RAM_MASK) + index * 16u + 4u <= PS2_RAM_SIZE)
                std::memcpy(&ptid, rdram + (table & PS2_RAM_MASK) + index * 16u, 4);
            std::cerr << "[sc3] InitAFS task=0x" << std::hex << task << std::dec << " phase=" << static_cast<int32_t>(phase)
                      << " index=" << index << " ptid=" << ptid << std::endl;
        }
        sub_002029C0_0x2029c0(rdram, ctx, runtime);
    }

    constexpr uint32_t kCreateTask = 0x00128B30u;

    void createTaskTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t nameAddress = getRegU32(ctx, 7);
        std::string name;
        for (uint32_t offset = 0; nameAddress && offset < 32u; ++offset)
        {
            const char c = static_cast<char>(rdram[(nameAddress + offset) & PS2_RAM_MASK]);
            if (c == '\0')
                break;
            name.push_back(c >= 0x20 && c < 0x7F ? c : '?');
        }
        std::cerr << "[sc3] task create '" << name << "' ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl;
        sub_00128B30_0x128b30(rdram, ctx, runtime);
    }

    // The movie task polls this wrapper, which turns PLAYEND back into PLAYING for seamless
    // players, and leaves on status 4 through its normal exit path.
    constexpr uint32_t kMoviePlayStat = 0x003AE988u;
    constexpr int32_t kMoviePlayStatFinished = 4;

    void moviePlayStatFinished(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const auto player = g_movies.find(getRegU32(ctx, 4));
        setReturnS32(ctx, !player ? kMoviePlayStatFinished : player->active() ? kMwPlyStatPlaying : kMwPlyStatPlayEnd);
        ctx->pc = getRegU32(ctx, 31);
    }

    constexpr uint32_t kMovieTaskUpdate = 0x00217F78u;

    void movieTaskUpdateTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t lastKey = ~0ull;
        const uint32_t task = getRegU32(ctx, 4) & PS2_RAM_MASK;
        int16_t state = 0;
        uint32_t adxf = 0, player = 0, flags = 0;
        std::memcpy(&state, rdram + task + 0x50u, 2);
        std::memcpy(&player, rdram + task + 0x58u, 4);
        std::memcpy(&flags, rdram + task + 0x60u, 4);
        std::memcpy(&adxf, rdram + task + 0x84u, 4);
        uint8_t adxfStat = 0;
        if (adxf)
            adxfStat = rdram[(adxf + 1u) & PS2_RAM_MASK];
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint16_t>(state)) << 40) ^ (static_cast<uint64_t>(adxfStat) << 32) ^ adxf;
        if (key != lastKey)
        {
            lastKey = key;
            std::cerr << "[sc3] movie task=0x" << std::hex << task << " state=" << std::dec << state << std::hex
                      << " player=0x" << player << " flags=0x" << flags << " adxf=0x" << adxf << std::dec
                      << " adxfStat=" << static_cast<int>(adxfStat) << std::endl;
        }
        sub_00217F78_0x217f78(rdram, ctx, runtime);
    }

    constexpr uint32_t kFadeDriver = 0x001A2960u;

    void fadeDriverTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t lastKey = ~0ull;
        static uint32_t calls = 0;
        uint32_t flag = 0, step = 0, level = 0, counter = 0;
        std::memcpy(&flag, rdram + 0x4EECC0u, 4);
        std::memcpy(&step, rdram + 0x4EECC4u, 4);
        std::memcpy(&level, rdram + 0x4EE05Cu, 4);
        std::memcpy(&counter, rdram + 0x4EE044u, 4);
        ++calls;
        const uint64_t key = (static_cast<uint64_t>(flag) << 63) ^ (static_cast<uint64_t>(step) << 32) ^ level ^ (static_cast<uint64_t>(counter) << 20);
        if (key != lastKey)
        {
            lastKey = key;
            std::cerr << "[sc3] fade call#" << calls << std::hex << " flag=" << flag << " step=0x" << step << " level=0x" << level
                      << " counter=" << std::dec << counter << " ra=0x" << std::hex << getRegU32(ctx, 31) << std::dec << std::endl;
        }
        sub_001A2960_0x1a2960(rdram, ctx, runtime);
    }

    constexpr uint32_t kMovieJobUpdate = 0x00199368u;

    void movieJobUpdateTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint64_t lastKey = ~0ull;
        PS2Memory &memory = runtime->memory();
        const uint32_t jobTask = memory.read32(0x70000024u);
        const uint32_t movieState = memory.read32(0x70000030u);
        const uint32_t playing = movieState ? memory.read32(movieState + 8u) : 0u;
        const uint32_t counter = jobTask ? memory.read32(jobTask + 0x40u) : 0u;
        static uint32_t calls = 0;
        const uint64_t key = (static_cast<uint64_t>(movieState) << 32) ^ (static_cast<uint64_t>(playing) << 8) ^ counter;
        if (key != lastKey || (++calls % 30u) == 0u)
        {
            lastKey = key;
            std::cerr << "[sc3] movie job task=0x" << std::hex << jobTask << " state=0x" << movieState << " playing=0x"
                      << playing << std::dec << " counter=" << counter << std::endl;
        }
        sub_00199368_0x199368(rdram, ctx, runtime);
    }

    // A movie task that never starts playing ends without clearing the movie job's "playing" word
    // (scratchpad 0x70000030 -> +8), which still names the task, so the job would wait forever.
    constexpr uint32_t kMovieTaskEnd = 0x00218600u;
    constexpr uint32_t kMovieStatePointer = 0x70000030u;

    void movieTaskEndReleasesJob(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2Memory &memory = runtime->memory();
        const uint32_t task = getRegU32(ctx, 4);
        const uint32_t movieState = memory.read32(kMovieStatePointer);
        const uint32_t playing = movieState != 0u ? memory.read32(movieState + 8u) : 0u;
        if (std::getenv("PS2X_SC3_TRACE_MOVIE"))
            std::cerr << "[sc3] movie task end task=0x" << std::hex << task << " playing=0x" << playing << std::dec << std::endl;
        if (movieState != 0u && playing == task)
            memory.write32(movieState + 8u, 0u);
        sub_00218600_0x218600(rdram, ctx, runtime);
    }

    // Sofdec decodes through libmpeg here, and the runtime's MPEG emulation is not fed by CRI's
    // bitstream callbacks: a started player blocks the main thread in sceMpegGetPicture. Players
    // are never started; AFS movies play on the host instead and the others report their end.
    constexpr uint32_t kMwPlyStartFunctions[] = {
        0x003AD4B0u, // mwPlyStartFname
        0x003AD5E8u, // mwPlyStartMem
        0x003AD6E8u, // mwPlyStartSj
        0x003ADA20u, // mwPlyStartAfs
        0x003ADBD0u, // mwPlyStartFnameRange
        0x003B1328u, // mwPlyStartSeamless
        0x003B1428u, // mwPlyStartFnameLp
        0x003B1698u, // mwPlyStartAfsLp
        0x003B17E0u, // mwPlyStartFnameRangeLp
        0x003B2DF0u, // mwPlyStartXX
    };

    constexpr uint32_t kMwPlyStartAfs = 0x003ADA20u;
    constexpr uint32_t kMwPlyStartAfsLp = 0x003B1698u;
    constexpr uint32_t kMovieAfsPartition = 1u;

    void mwPlyStartSkipped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t a1 = getRegU32(ctx, 5);
        std::string name;
        for (uint32_t i = 0; i < 64u && a1 > 0x100000u && a1 < 0x2000000u; ++i)
        {
            const char c = static_cast<char>(rdram[(a1 + i) & PS2_RAM_MASK]);
            if (c == 0 || c < 0x20 || c > 0x7e)
                break;
            name.push_back(c);
        }
        std::cerr << "[sc3] mwPlyStart fn=0x" << std::hex << ctx->pc << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << a1
                  << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7) << std::dec << " name='" << name << "'"
                  << std::endl;
        const uint32_t handle = getRegU32(ctx, 4);
        g_movies.stop(handle);
        if ((ctx->pc == kMwPlyStartAfs || ctx->pc == kMwPlyStartAfsLp) && getRegU32(ctx, 5) == kMovieAfsPartition &&
            !std::getenv("PS2X_SC3_NO_MOVIES"))
        {
            const std::string path = (PS2Runtime::getIoPaths().cdRoot / "MOVIE.AFS").string();
            uint64_t offset = 0, size = 0;
            auto player = std::make_shared<SfdPlayer>();
            if (afsEntry(path, getRegU32(ctx, 6), offset, size) && player->start(path, offset, size, ctx->pc == kMwPlyStartAfsLp))
            {
                g_movies.set(handle, std::move(player));
                std::cerr << "[sc3] movie file " << getRegU32(ctx, 6) << " at 0x" << std::hex << offset << " size 0x" << size << std::dec << std::endl;
            }
        }
        ctx->pc = getRegU32(ctx, 31);
    }

    // Experiment (PS2X_SC3_ADX_PLAYEND=1): adxt_GetStat reports PLAYEND for players stuck in
    // DECINFO. adxt_GetStat only returns the status byte at offset 1 of the handle.
    void adxtGetStatPlayEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t handle = getRegU32(ctx, 4);
        int32_t status = -3;
        if (handle != 0u)
            status = static_cast<int8_t>(rdram[(handle + 1u) & PS2_RAM_MASK]);
        static const int32_t forced = []
        {
            const char *value = std::getenv("PS2X_SC3_ADX_PLAYEND");
            return value && *value ? std::atoi(value) : 5;
        }();
        if (status == 1)
            status = forced;
        setReturnS32(ctx, status);
        ctx->pc = getRegU32(ctx, 31);
    }

    // PS2X_SC3_TRACE_OBJ="0x2E7D00:0xC8,0xCC": on entry to that function, log $a0 and the words at
    // those offsets of the object it points to whenever they change.
    struct ObjectTrace
    {
        uint32_t function = 0;
        PS2Runtime::RecompiledFunction original = nullptr;
        std::vector<uint32_t> offsets;
        std::map<uint32_t, std::vector<uint32_t>> last;
    };

    ObjectTrace &objectTrace()
    {
        static ObjectTrace trace;
        return trace;
    }

    void objectTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ObjectTrace &trace = objectTrace();
        const uint32_t object = getRegU32(ctx, 4);
        std::vector<uint32_t> values;
        for (const uint32_t offset : trace.offsets)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((object + offset) & PS2_RAM_MASK), 4);
            values.push_back(value);
        }
        auto &previous = trace.last[object];
        if (previous != values)
        {
            previous = values;
            std::cerr << "[sc3] obj fn=0x" << std::hex << trace.function << " a0=0x" << object << " ra=0x"
                      << getRegU32(ctx, 31);
            for (size_t index = 0; index < values.size(); ++index)
                std::cerr << " +0x" << trace.offsets[index] << "=0x" << values[index];
            std::cerr << std::dec << std::endl;
        }
        trace.original(rdram, ctx, runtime);
    }

    // PS2X_SC3_TRACE_ENTRY="0x1DE538,0x1DFB78": log pc/sp/ra/a0/a1 and the saved-ra slot candidates at each entry.
    std::map<uint32_t, PS2Runtime::RecompiledFunction> &entryTraceOriginals()
    {
        static std::map<uint32_t, PS2Runtime::RecompiledFunction> originals;
        return originals;
    }

    void entryTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sp = getRegU32(ctx, 29);
        uint32_t slot = 0;
        std::memcpy(&slot, rdram + ((sp + 0x88) & PS2_RAM_MASK), 4);
        std::cerr << "[sc3] entry pc=0x" << std::hex << ctx->pc << " sp=0x" << sp << " ra=0x" << getRegU32(ctx, 31)
                  << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7) << " sp88=0x" << slot << " sp2c=0x" << runtime->memory().read32(sp + 0x2cu) << " t0=0x" << runtime->memory().read32(0x10000000u) << std::dec << " cycle=" << runtime->eeScheduler().currentCycle()
                  << std::endl;
        auto &originals = entryTraceOriginals();
        auto it = originals.find(ctx->pc);
        if (it != originals.end())
            it->second(rdram, ctx, runtime);
    }

    void installEntryTrace(PS2Runtime &runtime, const char *spec)
    {
        std::string text(spec);
        size_t start = 0;
        while (start < text.size())
        {
            size_t end = text.find(',', start);
            if (end == std::string::npos)
                end = text.size();
            const uint32_t address = static_cast<uint32_t>(std::strtoul(text.substr(start, end - start).c_str(), nullptr, 0));
            if (auto original = runtime.lookupFunction(address))
            {
                entryTraceOriginals()[address] = original;
                runtime.registerFunction(address, entryTraced);
            }
            start = end + 1;
        }
    }

    // PS2X_SC3_RATE=<addr,...>: calls of each function per 60 emulated VSyncs, printed every 300.
    struct RateTrace
    {
        std::map<uint32_t, PS2Runtime::RecompiledFunction> originals;
        std::map<uint32_t, uint64_t> counts;
        uint64_t windowStart = 0;
    };

    RateTrace &rateTrace()
    {
        static RateTrace trace;
        return trace;
    }

    void rateTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        RateTrace &trace = rateTrace();
        const uint32_t address = ctx->pc;
        ++trace.counts[address];
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        if (tick >= trace.windowStart + 300u)
        {
            std::cerr << "[sc3] rate vsync=" << tick;
            for (auto &[function, count] : trace.counts)
            {
                std::cerr << " 0x" << std::hex << function << std::dec << "=" << (count * 60.0 / static_cast<double>(tick - trace.windowStart));
                count = 0;
            }
            std::cerr << std::endl;
            trace.windowStart = tick;
        }
        trace.originals[address](rdram, ctx, runtime);
    }

    void installRateTrace(PS2Runtime &runtime, const char *spec)
    {
        std::string text(spec);
        for (size_t start = 0; start < text.size();)
        {
            size_t end = text.find(',', start);
            if (end == std::string::npos)
                end = text.size();
            const uint32_t address = static_cast<uint32_t>(std::strtoul(text.substr(start, end - start).c_str(), nullptr, 0));
            if (auto original = runtime.lookupFunction(address))
            {
                rateTrace().originals[address] = original;
                runtime.registerFunction(address, rateTraced);
            }
            start = end + 1;
        }
    }

    // PS2X_SC3_TRACE_PACING=1: the frame limiter's Timer 3 wait (0x148098 called from 0x12e000-0x12f000).
    constexpr uint32_t kElapsedLines = 0x00148098u;
    PS2Runtime::RecompiledFunction g_elapsedLines = nullptr;

    void elapsedLinesTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t lastRa = 0;
        static uint64_t lastTick = 0;
        static uint32_t calls = 0;
        const uint32_t ra = getRegU32(ctx, 31);
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        static const uint64_t everyFrom = []
        {
            const char *value = std::getenv("PS2X_SC3_TRACE_PACING_ALL");
            return value ? std::strtoull(value, nullptr, 0) : ~0ull;
        }();
        static const uint64_t span = []
        {
            const char *value = std::getenv("PS2X_SC3_TRACE_PACING_SPAN");
            return value ? std::strtoull(value, nullptr, 0) : 7ull;
        }();
        if (tick >= everyFrom && tick < everyFrom + span)
        {
            PS2Memory &memory = runtime->memory();
            const uint32_t timing = memory.read32(0x469234u);
            std::cerr << "[sc3] pace ra=0x" << std::hex << ra << std::dec << " v=" << tick << " cycle=" << runtime->eeScheduler().currentCycle() << " t3=" << (memory.read32(0x10001800u) & 0xFFFFu)
                      << " base=" << memory.read16(timing + 0x100u) << " snap=" << memory.read16(timing + 0x102u)
                      << " abd=" << int(static_cast<int8_t>(memory.read8(0x46e5cdu))) << std::endl;
        }
        if (ra != lastRa || tick != lastTick)
        {
            PS2Memory &memory = runtime->memory();
            const uint32_t timing = memory.read32(0x469234u);
            const uint32_t sp = getRegU32(ctx, 29);
            static const uint64_t fullFrom = []
            {
                const char *value = std::getenv("PS2X_SC3_TRACE_PACING_FULL");
                return value ? std::strtoull(value, nullptr, 0) : ~0ull;
            }();
            if ((++calls % 8u) == 1u || ra != lastRa || (tick >= fullFrom && tick < fullFrom + 20u))
                std::cerr << "[sc3] pacing ra=0x" << std::hex << ra << std::dec << " vsync=" << tick
                          << " t3=" << (memory.read32(0x10001800u) & 0xFFFFu) << " base=" << memory.read16(timing + 0x100u)
                          << " snap=" << memory.read16(timing + 0x102u) << " target=" << static_cast<int32_t>(memory.read32(sp + 0x30u))
                          << " x4c=" << static_cast<int32_t>(memory.read32(timing + 0x4cu)) << std::endl;
            lastRa = ra;
            lastTick = tick;
        }
        g_elapsedLines(rdram, ctx, runtime);
    }

    // PS2X_SC3_TRACE_GSIRQ=1: what the GS interrupt handler (0x10e658) sees on entry.
    constexpr uint32_t kGsIrqHandler = 0x0010E658u;
    PS2Runtime::RecompiledFunction g_gsIrqHandler = nullptr;

    void gsIrqHandlerTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t calls = 0;
        static const uint32_t first = static_cast<uint32_t>(std::atoi(std::getenv("PS2X_SC3_TRACE_GSIRQ")));
        // PS2X_SC3_TRACE_GSIRQ_VSYNC=<tick>: also every interrupt during PS2X_SC3_TRACE_GSIRQ_SPAN (7) VSyncs from that tick.
        static const uint64_t fromTick = []
        {
            const char *value = std::getenv("PS2X_SC3_TRACE_GSIRQ_VSYNC");
            return value ? std::strtoull(value, nullptr, 0) : ~0ull;
        }();
        const uint64_t tick = runtime->eeScheduler().currentVSyncTick();
        static const uint64_t span = []
        {
            const char *value = std::getenv("PS2X_SC3_TRACE_GSIRQ_SPAN");
            return value ? std::strtoull(value, nullptr, 0) : 7ull;
        }();
        if ((++calls >= first && calls < first + 120u) || (tick >= fromTick && tick < fromTick + span))
        {
            PS2Memory &memory = runtime->memory();
            std::cerr << "[sc3] gsirq#" << calls << " vsync=" << runtime->eeScheduler().currentVSyncTick()
                      << " cycle=" << runtime->eeScheduler().currentCycle() << std::hex
                      << " csr=0x" << memory.read32(0x12001000u) << " sig=0x" << memory.read32(0x12001080u)
                      << " prev=0x" << memory.read32(0x467494u) << " h82=0x" << memory.read16(0x5be782u)
                      << " acd=" << unsigned(memory.read8(0x46e5ddu)) << " abe=" << unsigned(memory.read8(0x46e5ceu))
                      << std::dec << std::endl;
        }
        g_gsIrqHandler(rdram, ctx, runtime);
    }

    void installObjectTrace(PS2Runtime &runtime, const char *spec)
    {
        ObjectTrace &trace = objectTrace();
        std::string text(spec);
        const size_t colon = text.find(':');
        trace.function = static_cast<uint32_t>(std::strtoul(text.substr(0, colon).c_str(), nullptr, 0));
        for (size_t position = colon; position != std::string::npos && position + 1 < text.size();)
        {
            const size_t next = text.find(',', position + 1);
            trace.offsets.push_back(static_cast<uint32_t>(std::strtoul(text.substr(position + 1, next - position - 1).c_str(), nullptr, 0)));
            position = next;
        }
        trace.original = runtime.lookupFunction(trace.function);
        if (trace.original)
            runtime.registerFunction(trace.function, objectTraced);
    }

    // Experiment (PS2X_SC3_FORCE_TEXT=1): start the fade-in of texture-font items that have text
    // but were never started, by giving them a per-frame step (+0xC8) on entry to their update.
    constexpr uint32_t kTextureFontUpdate = 0x002E7D00u;
    PS2Runtime::RecompiledFunction g_textureFontUpdate = nullptr;

    void textureFontUpdateForced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t item = getRegU32(ctx, 4) & PS2_RAM_MASK;
        uint32_t text = 0;
        uint16_t counter = 0;
        float step = 0.0f;
        std::memcpy(&text, rdram + item + 0xC0u, 4);
        std::memcpy(&counter, rdram + item + 0xCCu, 2);
        std::memcpy(&step, rdram + item + 0xC8u, 4);
        static std::set<uint32_t> started;
        uint16_t duration = 0;
        std::memcpy(&duration, rdram + item + 0xCEu, 2);
        if (text != 0u && counter == 0u && step == 0.0f && duration != 0u && started.insert(item).second)
        {
            const uint16_t oneFrame = 1u;
            std::memcpy(rdram + item + 0xCCu, &oneFrame, 2);
        }
        g_textureFontUpdate(rdram, ctx, runtime);
    }

    void applySoulcalibur3(PS2Runtime &runtime)
    {
        if (const char *spec = std::getenv("PS2X_SC3_TRACE_ENTRY"); spec && *spec)
            installEntryTrace(runtime, spec);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_PACING"); trace && *trace)
        {
            g_elapsedLines = runtime.lookupFunction(kElapsedLines);
            if (g_elapsedLines)
                runtime.registerFunction(kElapsedLines, elapsedLinesTraced);
        }
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_GSIRQ"); trace && *trace)
        {
            g_gsIrqHandler = runtime.lookupFunction(kGsIrqHandler);
            if (g_gsIrqHandler)
                runtime.registerFunction(kGsIrqHandler, gsIrqHandlerTraced);
        }
        if (const char *spec = std::getenv("PS2X_SC3_RATE"); spec && *spec)
            installRateTrace(runtime, spec);
        if (const char *spec = std::getenv("PS2X_SC3_TRACE_OBJ"); spec && *spec)
            installObjectTrace(runtime, spec);
        if (const char *experiment = std::getenv("PS2X_SC3_FORCE_TEXT"); experiment && *experiment)
        {
            g_textureFontUpdate = runtime.lookupFunction(kTextureFontUpdate);
            if (g_textureFontUpdate)
                runtime.registerFunction(kTextureFontUpdate, textureFontUpdateForced);
        }
        if (const char *experiment = std::getenv("PS2X_SC3_ADX_PLAYEND"); experiment && *experiment)
            runtime.registerFunction(kAdxtGetStat, adxtGetStatPlayEnd);
        for (const uint32_t address : kMwPlyStartFunctions)
            runtime.registerFunction(address, mwPlyStartSkipped);
        runtime.registerFunction(kMovieTaskEnd, movieTaskEndReleasesJob);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_MOVIE"); trace && *trace)
            runtime.registerFunction(kMovieJobUpdate, movieJobUpdateTraced);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_FADE"); trace && *trace)
            runtime.registerFunction(kFadeDriver, fadeDriverTraced);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_MOVIE"); trace && *trace)
            runtime.registerFunction(kMovieTaskUpdate, movieTaskUpdateTraced);
        runtime.registerFunction(kMoviePlayStat, moviePlayStatFinished);
        runtime.configureReservedMemory(kReservedBase, kReservedTop, kMainStackBottom);
        runtime.registerFunction(kMwPlyGetStat, mwPlyGetStatPlayEnd);
        runtime.registerFunction(kMwPlyGetCurFrm, mwPlyGetCurFrmHost);
        runtime.registerFunction(kMwPlyRelCurFrm, mwPlyRelCurFrmHost);
        runtime.registerFunction(kMwPlyIsNextFrmReady, mwPlyIsNextFrmReadyHost);
        runtime.registerFunction(kMwPlyStop, mwPlyStopHost);
        runtime.setPresentationOverride([](std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
                                        {
                                            for (const auto &player : g_movies.all())
                                                if (player->present(rgba, width, height))
                                                    return true;
                                            return false; });
        runtime.audioBackend().setSpu2Mixer([](int16_t *frames, size_t count)
                                            {
                                                for (const auto &player : g_movies.all())
                                                    player->mix(frames, count); });
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_ADXT"); trace && *trace)
            runtime.registerFunction(kAdxtGetStat, adxtGetStatTraced);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_ADXF"); trace && *trace)
            runtime.registerFunction(kInitAfsUpdate, initAfsUpdateTraced);
        if (const char *trace = std::getenv("PS2X_SC3_TRACE_TASKS"); trace && *trace)
            runtime.registerFunction(kCreateTask, createTaskTraced);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Soulcalibur III (SLUS-21216)", "SLUS_212.16", 0x00100008u, 0xD9A4ECF8u, applySoulcalibur3)
