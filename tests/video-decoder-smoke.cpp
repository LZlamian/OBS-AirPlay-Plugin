// Mirror video decoder smoke test: hardware (VideoToolbox) vs software.
//
// usage: obs-airplay-decoder-smoke [--require-hardware] FILE_A [FILE_B]
//
// FILE_A/FILE_B are H.264 or HEVC files (any container FFmpeg reads). Their
// packets are converted to Annex B, as the AirPlay mirror path delivers them,
// and fed to H264Decoder:
//   1. FILE_A through a software-only decoder and through an auto decoder;
//      every frame must match (luma PSNR) and have the same size.
//   2. FILE_B appended to the same auto decoder without a flush: a mid-stream
//      size change, as AirPlay mirroring does.
//   3. Garbage packets, then FILE_A again: the decoder must survive and
//      produce pictures again (hardware or software fallback).

#include "h264-decoder.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <vector>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

extern "C" void blog(int, const char* format, ...)
{
    std::fputs("decoder-smoke: ", stderr);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

namespace {

struct Stream {
    AVCodecID codec = AV_CODEC_ID_NONE;
    std::vector<std::vector<uint8_t>> packets;
};

bool loadAnnexB(const char* path, Stream* out)
{
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        return false;
    }
    const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        avformat_close_input(&format);
        return false;
    }
    AVStream* stream = format->streams[index];
    out->codec = stream->codecpar->codec_id;
    const char* filter_name = out->codec == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb"
                                                             : "h264_mp4toannexb";
    const AVBitStreamFilter* filter = av_bsf_get_by_name(filter_name);
    AVBSFContext* bsf = nullptr;
    if (!filter || av_bsf_alloc(filter, &bsf) < 0) {
        avformat_close_input(&format);
        return false;
    }
    avcodec_parameters_copy(bsf->par_in, stream->codecpar);
    bsf->time_base_in = stream->time_base;
    av_bsf_init(bsf);

    AVPacket* packet = av_packet_alloc();
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && av_bsf_send_packet(bsf, packet) >= 0) {
            while (av_bsf_receive_packet(bsf, packet) >= 0) {
                out->packets.emplace_back(packet->data, packet->data + packet->size);
                av_packet_unref(packet);
            }
        } else {
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
    av_bsf_free(&bsf);
    avformat_close_input(&format);
    return !out->packets.empty();
}

// The luma plane of a decoded frame, tightly packed.
struct Picture {
    int width = 0;
    int height = 0;
    bool hardware = false;
    std::vector<uint8_t> luma;
};

Picture copyPicture(const DecodedVideoFrame& frame, bool hardware)
{
    Picture picture;
    picture.width = frame.width;
    picture.height = frame.height;
    picture.hardware = hardware;
    picture.luma.resize(static_cast<size_t>(frame.width) * frame.height);
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(picture.luma.data() + static_cast<size_t>(y) * frame.width,
                    frame.data[0] + static_cast<size_t>(y) * frame.linesize[0], frame.width);
    }
    return picture;
}

// CPU time used by this process so far (all threads), in milliseconds.
double processCpuMs()
{
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}

// Decodes a stream without keeping pictures and returns CPU ms per frame.
double cpuPerFrame(H264Decoder& decoder, const Stream& stream)
{
    size_t frames = 0;
    const double before = processCpuMs();
    for (const auto& packet : stream.packets) {
        DecodedVideoFrame frame;
        if (decoder.decodeToI420(packet.data(), packet.size(), frame)) ++frames;
    }
    return frames ? (processCpuMs() - before) / frames : 0.0;
}

std::vector<Picture> decodeAll(H264Decoder& decoder, const Stream& stream, double* milliseconds)
{
    std::vector<Picture> pictures;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& packet : stream.packets) {
        DecodedVideoFrame frame;
        if (decoder.decodeToI420(packet.data(), packet.size(), frame)) {
            pictures.push_back(copyPicture(frame, decoder.usingHardware()));
        }
    }
    if (milliseconds) {
        *milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }
    return pictures;
}

double psnr(const Picture& a, const Picture& b)
{
    if (a.width != b.width || a.height != b.height || a.luma.empty()) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.luma.size(); ++i) {
        const double d = static_cast<double>(a.luma[i]) - b.luma[i];
        sum += d * d;
    }
    const double mse = sum / a.luma.size();
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

int main(int argc, char** argv)
{
    bool require_hardware = false;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--require-hardware") == 0) require_hardware = true;
        else files.push_back(argv[i]);
    }
    if (files.empty() || files.size() > 2) {
        std::fprintf(stderr, "usage: %s [--require-hardware] FILE_A [FILE_B]\n", argv[0]);
        return 2;
    }

    Stream a;
    Stream b;
    if (!loadAnnexB(files[0], &a) || (files.size() == 2 && !loadAnnexB(files[1], &b))) {
        std::fprintf(stderr, "could not read input\n");
        return 2;
    }
    if (files.size() == 2 && a.codec != b.codec) {
        std::fprintf(stderr, "FILE_A and FILE_B must use the same codec\n");
        return 2;
    }

    bool ok = true;

    // 1. software reference vs auto (hardware when available)
    H264Decoder software(a.codec, H264Decoder::Mode::SoftwareOnly);
    H264Decoder automatic(a.codec, H264Decoder::Mode::Auto);
    double software_ms = 0.0;
    double automatic_ms = 0.0;
    const std::vector<Picture> reference = decodeAll(software, a, &software_ms);
    const std::vector<Picture> pictures = decodeAll(automatic, a, &automatic_ms);
    size_t hardware_frames = 0;
    double worst = 99.0;
    for (size_t i = 0; i < pictures.size() && i < reference.size(); ++i) {
        if (pictures[i].hardware) ++hardware_frames;
        worst = std::min(worst, psnr(pictures[i], reference[i]));
    }
    const bool same_count = !reference.empty() && pictures.size() == reference.size();
    const bool same_picture = worst >= 40.0;
    const bool hardware_ok = !require_hardware || hardware_frames == pictures.size();
    ok = ok && same_count && same_picture && hardware_ok;
    std::printf("match: frames software=%zu auto=%zu hardware_frames=%zu worst_psnr=%.1f dB "
                "(%dx%d) software=%.2f ms/frame auto=%.2f ms/frame -> %s\n",
                reference.size(), pictures.size(), hardware_frames, worst,
                pictures.empty() ? 0 : pictures[0].width,
                pictures.empty() ? 0 : pictures[0].height,
                reference.empty() ? 0.0 : software_ms / reference.size(),
                pictures.empty() ? 0.0 : automatic_ms / pictures.size(),
                same_count && same_picture && hardware_ok ? "ok" : "FAILED");

    // CPU cost per frame in this process (decode only, no picture copies).
    {
        H264Decoder software_cpu(a.codec, H264Decoder::Mode::SoftwareOnly);
        H264Decoder automatic_cpu(a.codec, H264Decoder::Mode::Auto);
        const double software_cpu_ms = cpuPerFrame(software_cpu, a);
        const double automatic_cpu_ms = cpuPerFrame(automatic_cpu, a);
        std::printf("cpu: software=%.2f ms/frame auto(%s)=%.2f ms/frame\n", software_cpu_ms,
                    automatic_cpu.usingHardware() ? "hardware" : "software", automatic_cpu_ms);
    }

    // 2. a different-size stream on the same decoder, no flush
    if (files.size() == 2) {
        H264Decoder software_b(b.codec, H264Decoder::Mode::SoftwareOnly);
        const std::vector<Picture> reference_b = decodeAll(software_b, b, nullptr);
        const std::vector<Picture> pictures_b = decodeAll(automatic, b, nullptr);
        double worst_b = 99.0;
        size_t hardware_b = 0;
        for (size_t i = 0; i < pictures_b.size() && i < reference_b.size(); ++i) {
            if (pictures_b[i].hardware) ++hardware_b;
            worst_b = std::min(worst_b, psnr(pictures_b[i], reference_b[i]));
        }
        // The first frames may differ while the old stream's delayed frames
        // drain; the size and picture must match from the new stream on.
        const bool resized = !pictures_b.empty() && !reference_b.empty() &&
            pictures_b.back().width == reference_b.back().width &&
            pictures_b.back().height == reference_b.back().height &&
            pictures_b.size() + 2 >= reference_b.size() &&
            psnr(pictures_b.back(), reference_b.back()) >= 40.0;
        ok = ok && resized;
        std::printf("size-change: frames software=%zu auto=%zu hardware_frames=%zu last=%dx%d "
                    "last_psnr=%.1f dB -> %s\n",
                    reference_b.size(), pictures_b.size(), hardware_b,
                    pictures_b.empty() ? 0 : pictures_b.back().width,
                    pictures_b.empty() ? 0 : pictures_b.back().height,
                    pictures_b.empty() || reference_b.empty()
                        ? 0.0 : psnr(pictures_b.back(), reference_b.back()),
                    resized ? "ok" : "FAILED");
    }

    // 3. garbage in the stream, then a clean stream again
    {
        std::vector<uint8_t> garbage(4096);
        for (size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
        }
        garbage[0] = 0; garbage[1] = 0; garbage[2] = 0; garbage[3] = 1;
        garbage[4] = a.codec == AV_CODEC_ID_HEVC ? 0x02 : 0x41; // a non-key slice
        for (int i = 0; i < 8; ++i) {
            DecodedVideoFrame frame;
            automatic.decodeToI420(garbage.data(), garbage.size(), frame);
        }
        const std::vector<Picture> recovered = decodeAll(automatic, a, nullptr);
        const bool recovered_ok = !recovered.empty() && !reference.empty() &&
            recovered.size() + 2 >= reference.size() &&
            psnr(recovered.back(), reference.back()) >= 40.0;
        ok = ok && recovered_ok;
        std::printf("recovery: frames=%zu of %zu last_psnr=%.1f dB mode=%s -> %s\n",
                    recovered.size(), reference.size(),
                    recovered.empty() ? 0.0 : psnr(recovered.back(), reference.back()),
                    automatic.usingHardware() ? "hardware" : "software",
                    recovered_ok ? "ok" : "FAILED");

        // A flush is a stream boundary: hardware is used again afterwards.
        automatic.flush();
        const std::vector<Picture> next_stream = decodeAll(automatic, a, nullptr);
        const bool rearmed = !next_stream.empty() &&
            (!require_hardware || next_stream.back().hardware) &&
            next_stream.size() == reference.size();
        ok = ok && rearmed;
        std::printf("next-stream: frames=%zu mode=%s -> %s\n", next_stream.size(),
                    automatic.usingHardware() ? "hardware" : "software",
                    rearmed ? "ok" : "FAILED");
    }

    return ok ? 0 : 1;
}
