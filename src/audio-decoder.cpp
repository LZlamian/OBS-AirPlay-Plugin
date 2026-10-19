// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio-decoder.hpp"

#include <climits>
#include <cstdio>

#include <obs-module.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kDefaultSampleRate = 44100;

// AAC sampling frequency indexes (ISO/IEC 14496-3, 1.6.3.4).
int aacFrequencyIndex(int sample_rate)
{
    static const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < static_cast<int>(sizeof(kRates) / sizeof(kRates[0])); ++i) {
        if (kRates[i] == sample_rate) return i;
    }
    return 15; // escape: the rate follows in 24 bits
}

class BitWriter {
public:
    void put(uint32_t value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i) {
            if (m_used % 8 == 0) m_bytes.push_back(0);
            if ((value >> i) & 1U) m_bytes.back() |= static_cast<uint8_t>(0x80U >> (m_used % 8));
            ++m_used;
        }
    }
    std::vector<uint8_t> bytes() const { return m_bytes; }

private:
    std::vector<uint8_t> m_bytes;
    int m_used = 0;
};

void putBigEndian(std::vector<uint8_t>& out, uint32_t value, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

} // namespace

namespace airplay_audio {

bool formatFromSetup(uint64_t audio_format, unsigned spf, AirPlayAudioFormat& out)
{
    out = AirPlayAudioFormat{};
    out.frame_samples = spf > 0 && spf <= 8192 ? static_cast<int>(spf) : 0;
    struct Known {
        uint64_t bit;
        int sample_rate;
        int bit_depth;
    };
    static const Known kKnown[] = {
        {0x40000, 44100, 16},   // ALAC/44100/16/2
        {0x80000, 44100, 24},   // ALAC/44100/24/2
        {0x100000, 48000, 16},  // ALAC/48000/16/2
        {0x200000, 48000, 24},  // ALAC/48000/24/2
        {0x400000, 44100, 16},  // AAC-LC/44100/2
        {0x800000, 48000, 16},  // AAC-LC/48000/2
        {0x1000000, 44100, 16}, // AAC-ELD/44100/2
        {0x2000000, 48000, 16}, // AAC-ELD/48000/2
    };
    for (const Known& known : kKnown) {
        if (audio_format == known.bit) {
            out.sample_rate = known.sample_rate;
            out.bit_depth = known.bit_depth;
            return true;
        }
    }
    return false;
}

std::string describe(const AirPlayAudioFormat& format, uint8_t ct)
{
    const char* codec = ct == 2 ? "ALAC" : ct == 4 ? "AAC-LC" : ct == 8 ? "AAC-ELD" : "codec";
    char text[96];
    if (ct == 2) {
        snprintf(text, sizeof(text), "%s %d Hz, %d-bit, %d ch", codec, format.sample_rate,
                 format.bit_depth, format.channels);
    } else {
        snprintf(text, sizeof(text), "%s %d Hz, %d ch", codec, format.sample_rate,
                 format.channels);
    }
    return text;
}

std::vector<uint8_t> aacAudioSpecificConfig(int object_type, const AirPlayAudioFormat& format)
{
    BitWriter bits;
    if (object_type >= 32) {
        bits.put(31, 5);
        bits.put(static_cast<uint32_t>(object_type - 32), 6);
    } else {
        bits.put(static_cast<uint32_t>(object_type), 5);
    }
    const int index = aacFrequencyIndex(format.sample_rate);
    bits.put(static_cast<uint32_t>(index), 4);
    if (index == 15) bits.put(static_cast<uint32_t>(format.sample_rate), 24);
    bits.put(static_cast<uint32_t>(format.channels), 4);
    if (object_type == 39) {
        // ELDSpecificConfig: frameLengthFlag (480 samples, 512 otherwise), no
        // resilience flags, no LD-SBR, ELDEXT_TERM.
        const int frame = format.frame_samples > 0 ? format.frame_samples : 480;
        bits.put(frame == 480 ? 1 : 0, 1);
        bits.put(0, 3);
        bits.put(0, 1);
        bits.put(0, 4);
    } else {
        // GASpecificConfig: frameLengthFlag (960 samples, 1024 otherwise),
        // no core coder, no extension.
        bits.put(format.frame_samples == 960 ? 1 : 0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
    }
    return bits.bytes();
}

std::vector<uint8_t> alacMagicCookie(const AirPlayAudioFormat& format)
{
    std::vector<uint8_t> cookie;
    putBigEndian(cookie, 36, 4); // atom size
    cookie.insert(cookie.end(), {'a', 'l', 'a', 'c'});
    putBigEndian(cookie, 0, 4);  // version and flags
    putBigEndian(cookie, static_cast<uint32_t>(format.frame_samples > 0 ? format.frame_samples : 352), 4);
    cookie.push_back(0);         // compatible version
    cookie.push_back(static_cast<uint8_t>(format.bit_depth));
    cookie.push_back(40);        // pb
    cookie.push_back(10);        // mb
    cookie.push_back(14);        // kb
    cookie.push_back(static_cast<uint8_t>(format.channels));
    putBigEndian(cookie, 255, 2); // max run
    putBigEndian(cookie, 0, 4);   // max frame bytes (unknown)
    putBigEndian(cookie, 0, 4);   // average bit rate (unknown)
    putBigEndian(cookie, static_cast<uint32_t>(format.sample_rate), 4);
    return cookie;
}

} // namespace airplay_audio

AudioDecoder::AudioDecoder()
    : m_codec_context(nullptr)
    , m_frame(av_frame_alloc())
    , m_packet(av_packet_alloc())
    , m_swr_context(nullptr)
    , m_codec_type(0)
    , m_output_sample_rate(kDefaultSampleRate)
    , m_input_sample_rate(0)
    , m_input_sample_format(AV_SAMPLE_FMT_NONE)
    , m_resampler_configured(false)
#if !OBS_AIRPLAY_HAVE_CH_LAYOUT
    , m_input_channel_layout(0)
#endif
{
    if (!m_frame || !m_packet) {
        blog(LOG_ERROR, "Failed to allocate FFmpeg audio frame/packet");
    }
    // Pre-warm AAC-ELD decoder — iOS always uses this for screen mirroring.
    // Calling avcodec_open2 eagerly here removes it from the first-packet
    // critical path, saving ~100-200ms on initial connection.
    configureDecoder(8);
}

AudioDecoder::~AudioDecoder()
{
    resetResampler();
    av_freep(&m_convert[0]);

    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }

    if (m_frame) {
        av_frame_free(&m_frame);
    }

    if (m_packet) {
        av_packet_free(&m_packet);
    }
}

void AudioDecoder::flush()
{
    if (m_codec_context)
        avcodec_flush_buffers(m_codec_context);
    if (m_frame)
        av_frame_unref(m_frame);
    resetResampler();
}

void AudioDecoder::setStreamFormat(const AirPlayAudioFormat& format)
{
    m_format = format;
}

bool AudioDecoder::configureDecoder(uint8_t ct)
{
    if (m_codec_type == ct && m_codec_context && m_opened_format == m_format) {
        return true;
    }
    // A decoder that could not be opened is tried again about every 10 s of
    // packets, not on every one (each attempt logs at ERROR).
    constexpr unsigned kRetryEveryCalls = 500;
    if (ct == m_failed_codec_type && (++m_failed_calls % kRetryEveryCalls) != 0) {
        return false;
    }
    const bool opened = openDecoder(ct);
    if (opened) {
        m_failed_codec_type = 0;
        m_failed_calls = 0;
    } else if (ct != m_failed_codec_type) {
        m_failed_codec_type = ct;
        m_failed_calls = 0;
    }
    return opened;
}

bool AudioDecoder::openDecoder(uint8_t ct)
{
    const AVCodec* codec = nullptr;
    // Built for the negotiated format (rate, channels, depth, frame length).
    std::vector<uint8_t> setup;

    switch (ct) {
    case 8: // AAC-ELD
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        setup = airplay_audio::aacAudioSpecificConfig(39, m_format);
        break;
    case 4: // AAC-LC
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        setup = airplay_audio::aacAudioSpecificConfig(2, m_format);
        break;
    case 2: // ALAC
        codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
        setup = airplay_audio::alacMagicCookie(m_format);
        break;
    default:
        blog(LOG_WARNING, "Unsupported AirPlay audio codec type: %u", static_cast<unsigned>(ct));
        return false;
    }

    if (!codec) {
        blog(LOG_ERROR, "Failed to find FFmpeg decoder for AirPlay codec type %u", static_cast<unsigned>(ct));
        return false;
    }

    resetResampler();

    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }

    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        blog(LOG_ERROR, "Failed to allocate audio codec context");
        return false;
    }

    m_codec_context->sample_rate = m_format.sample_rate;
#if OBS_AIRPLAY_HAVE_CH_LAYOUT
    av_channel_layout_default(&m_codec_context->ch_layout, m_format.channels);
#else
    m_codec_context->channels = m_format.channels;
    m_codec_context->channel_layout = av_get_default_channel_layout(m_format.channels);
#endif

    const uint8_t* extradata = setup.data();
    const int extradata_size = static_cast<int>(setup.size());
    if (extradata && extradata_size > 0) {
        m_codec_context->extradata = static_cast<uint8_t*>(av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!m_codec_context->extradata) {
            blog(LOG_ERROR, "Failed to allocate audio decoder extradata");
            avcodec_free_context(&m_codec_context);
            return false;
        }
        std::memcpy(m_codec_context->extradata, extradata, static_cast<size_t>(extradata_size));
        m_codec_context->extradata_size = extradata_size;
    }

    if (avcodec_open2(m_codec_context, codec, nullptr) < 0) {
        blog(LOG_ERROR, "Failed to open FFmpeg decoder for AirPlay codec type %u", static_cast<unsigned>(ct));
        avcodec_free_context(&m_codec_context);
        return false;
    }

    m_codec_type = ct;
    m_opened_format = m_format;
    blog(LOG_INFO, "Configured audio decoder for %s", airplay_audio::describe(m_format, ct).c_str());
    return true;
}

bool AudioDecoder::configureResampler(const AVFrame* frame)
{
    if (!frame || !m_codec_context) {
        return false;
    }

    const int inputSampleRate = frame->sample_rate > 0
        ? frame->sample_rate : kDefaultSampleRate;
    const AVSampleFormat inputSampleFormat =
        static_cast<AVSampleFormat>(frame->format);
    m_output_sample_rate = inputSampleRate;

#if OBS_AIRPLAY_HAVE_CH_LAYOUT
    const AVChannelLayout* inputLayout = frame->ch_layout.nb_channels > 0
        ? &frame->ch_layout : &m_codec_context->ch_layout;
    if (inputLayout->nb_channels <= 0)
        return false;

    if (m_resampler_configured &&
        m_input_sample_rate == inputSampleRate &&
        m_input_sample_format == inputSampleFormat &&
        av_channel_layout_compare(&m_input_channel_layout, inputLayout) == 0) {
        return true;
    }

    resetResampler();
    if (av_channel_layout_copy(&m_input_channel_layout, inputLayout) < 0)
        return false;

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);

    const int allocResult = swr_alloc_set_opts2(
        &m_swr_context,
        &out_layout,
        AV_SAMPLE_FMT_FLTP,
        m_output_sample_rate,
        &m_input_channel_layout,
        inputSampleFormat,
        inputSampleRate,
        0,
        nullptr);

    av_channel_layout_uninit(&out_layout);
    if (allocResult < 0) {
        resetResampler();
        return false;
    }
#else
    const uint64_t inputLayout = frame->channel_layout
        ? frame->channel_layout
        : av_get_default_channel_layout(frame->channels);
    if (m_resampler_configured &&
        m_input_sample_rate == inputSampleRate &&
        m_input_sample_format == inputSampleFormat &&
        m_input_channel_layout == inputLayout) {
        return true;
    }

    resetResampler();
    m_input_channel_layout = inputLayout;
    m_swr_context = swr_alloc_set_opts(
        nullptr,
        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_FLTP,
        m_output_sample_rate,
        inputLayout,
        inputSampleFormat,
        inputSampleRate,
        0,
        nullptr);
#endif

    if (!m_swr_context || swr_init(m_swr_context) < 0) {
        blog(LOG_ERROR, "Failed to initialize audio resampler");
        resetResampler();
        return false;
    }

    m_input_sample_rate = inputSampleRate;
    m_input_sample_format = inputSampleFormat;
    m_resampler_configured = true;
    return true;
}

void AudioDecoder::resetResampler()
{
    if (m_swr_context)
        swr_free(&m_swr_context);
#if OBS_AIRPLAY_HAVE_CH_LAYOUT
    av_channel_layout_uninit(&m_input_channel_layout);
#else
    m_input_channel_layout = 0;
#endif
    m_input_sample_rate = 0;
    m_input_sample_format = AV_SAMPLE_FMT_NONE;
    m_resampler_configured = false;
}

bool AudioDecoder::decode(const uint8_t* data, size_t size, uint8_t ct,
                          std::vector<float>& left, std::vector<float>& right,
                          int& sample_rate)
{
    left.clear();
    right.clear();
    sample_rate = kDefaultSampleRate;

    if (!data || size == 0 || !m_packet || !m_frame ||
        size > static_cast<size_t>(INT_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) {
        return false;
    }

    if (!configureDecoder(ct)) {
        return false;
    }

    av_packet_unref(m_packet);
    if (av_new_packet(m_packet, static_cast<int>(size)) < 0) {
        static uint64_t alloc_errors = 0;
        if ((alloc_errors++ % 60) == 0) {
            blog(LOG_ERROR, "Failed to allocate audio packet buffer");
        }
        return false;
    }
    memcpy(m_packet->data, data, size);

    int ret = avcodec_send_packet(m_codec_context, m_packet);
    if (ret < 0) {
        static uint64_t logged_errors = 0;
        if ((logged_errors++ % 60) == 0) {
            blog(LOG_WARNING, "Audio packet send failed for codec type %u (ret=%d)", static_cast<unsigned>(ct), ret);
        }
        return false;
    }

    bool produced = false;
    while (true) {
        ret = avcodec_receive_frame(m_codec_context, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            static uint64_t logged_decode_errors = 0;
            if ((logged_decode_errors++ % 60) == 0) {
                blog(LOG_WARNING, "Audio frame decode failed for codec type %u (ret=%d)", static_cast<unsigned>(ct), ret);
            }
            break;
        }

        // AAC and ALAC decode to planar float: stereo needs no conversion.
        const int channels =
#if OBS_AIRPLAY_HAVE_CH_LAYOUT
            m_frame->ch_layout.nb_channels;
#else
            m_frame->channels;
#endif
        if (m_frame->format == AV_SAMPLE_FMT_FLTP && channels == 2 && m_frame->nb_samples > 0 &&
            m_frame->extended_data[0] && m_frame->extended_data[1]) {
            const float* l = reinterpret_cast<const float*>(m_frame->extended_data[0]);
            const float* r = reinterpret_cast<const float*>(m_frame->extended_data[1]);
            left.insert(left.end(), l, l + m_frame->nb_samples);
            right.insert(right.end(), r, r + m_frame->nb_samples);
            sample_rate = m_frame->sample_rate > 0 ? m_frame->sample_rate : kDefaultSampleRate;
            produced = true;
            av_frame_unref(m_frame);
            continue;
        }

        if (!configureResampler(m_frame)) {
            av_frame_unref(m_frame);
            break;
        }

        const int out_samples = av_rescale_rnd(
            swr_get_delay(m_swr_context, m_input_sample_rate) + m_frame->nb_samples,
            m_output_sample_rate,
            m_input_sample_rate,
            AV_ROUND_UP);

        if (out_samples <= 0) {
            av_frame_unref(m_frame);
            continue;
        }

        // One conversion buffer, grown when needed, instead of one per packet.
        if (out_samples > m_convert_capacity) {
            av_freep(&m_convert[0]);
            m_convert[1] = nullptr;
            int out_linesize = 0;
            if (av_samples_alloc(m_convert, &out_linesize, 2, out_samples, AV_SAMPLE_FMT_FLTP, 0) < 0) {
                m_convert_capacity = 0;
                av_frame_unref(m_frame);
                break;
            }
            m_convert_capacity = out_samples;
        }
        uint8_t** out_data = m_convert;

        const int converted = swr_convert(
            m_swr_context,
            out_data,
            out_samples,
            const_cast<const uint8_t**>(m_frame->extended_data),
            m_frame->nb_samples);

        if (converted > 0 && out_data[0] && out_data[1]) {
            const float* l = reinterpret_cast<const float*>(out_data[0]);
            const float* r = reinterpret_cast<const float*>(out_data[1]);
            left.insert(left.end(), l, l + converted);
            right.insert(right.end(), r, r + converted);
            sample_rate = m_output_sample_rate;
            produced = true;
        }

        av_frame_unref(m_frame);
    }

    return produced;
}
