#include "audio-decoder.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kDefaultSampleRate = 44100;

constexpr uint8_t kAacEldAsc[] = {0xF8, 0xE8, 0x50, 0x00};
constexpr uint8_t kAacLcAsc[] = {0x12, 0x10};
constexpr uint8_t kAlacCookie[] = {
    0x00, 0x00, 0x00, 0x24, 0x61, 0x6C, 0x61, 0x63,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60,
    0x00, 0x10, 0x28, 0x0A, 0x0E, 0x02, 0x00, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xAC, 0x44
};

} // namespace

AudioDecoder::AudioDecoder()
    : m_codec_context(nullptr)
    , m_frame(av_frame_alloc())
    , m_packet(av_packet_alloc())
    , m_swr_context(nullptr)
    , m_codec_type(0)
    , m_output_sample_rate(kDefaultSampleRate)
    , m_swr_in_rate(0)
    , m_swr_in_format(AV_SAMPLE_FMT_NONE)
    , m_swr_in_channels(0)
    , m_swr_buf_capacity(0)
{
    if (!m_frame || !m_packet) {
        blog(LOG_ERROR, "Failed to allocate FFmpeg audio frame/packet");
    }
}

AudioDecoder::~AudioDecoder()
{
    if (m_swr_context) {
        swr_free(&m_swr_context);
    }

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

bool AudioDecoder::configureDecoder(uint8_t ct)
{
    if (m_codec_type == ct && m_codec_context) {
        return true;
    }

    const AVCodec* codec = nullptr;
    const uint8_t* extradata = nullptr;
    int extradata_size = 0;

    switch (ct) {
    case 8: // AAC-ELD
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        extradata = kAacEldAsc;
        extradata_size = static_cast<int>(sizeof(kAacEldAsc));
        break;
    case 4: // AAC-LC
        codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        extradata = kAacLcAsc;
        extradata_size = static_cast<int>(sizeof(kAacLcAsc));
        break;
    case 2: // ALAC
        codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
        extradata = kAlacCookie;
        extradata_size = static_cast<int>(sizeof(kAlacCookie));
        break;
    default:
        blog(LOG_WARNING, "Unsupported AirPlay audio codec type: %u", static_cast<unsigned>(ct));
        return false;
    }

    if (!codec) {
        blog(LOG_ERROR, "Failed to find FFmpeg decoder for AirPlay codec type %u", static_cast<unsigned>(ct));
        return false;
    }

    if (m_swr_context) {
        swr_free(&m_swr_context);
    }

    if (m_codec_context) {
        avcodec_free_context(&m_codec_context);
    }

    m_codec_context = avcodec_alloc_context3(codec);
    if (!m_codec_context) {
        blog(LOG_ERROR, "Failed to allocate audio codec context");
        return false;
    }

    m_codec_context->sample_rate = kDefaultSampleRate;
#if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&m_codec_context->ch_layout, 2);
#else
    m_codec_context->channels = 2;
    m_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
#endif

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
    blog(LOG_INFO, "Configured audio decoder for codec type %u", static_cast<unsigned>(ct));
    return true;
}

bool AudioDecoder::inputMatchesOutput(const AVFrame* frame) const
{
    if (!frame) {
        return false;
    }
    if (frame->format != AV_SAMPLE_FMT_FLTP) {
        return false;
    }
    if (frame->sample_rate != m_output_sample_rate) {
        return false;
    }
#if LIBAVUTIL_VERSION_MAJOR >= 57
    if (frame->ch_layout.nb_channels != 2) {
        return false;
    }
#else
    if (frame->channels != 2) {
        return false;
    }
#endif
    return true;
}

bool AudioDecoder::configureResampler(const AVFrame* frame)
{
    if (!frame || !m_codec_context) {
        return false;
    }

    const int new_rate = frame->sample_rate > 0 ? frame->sample_rate : kDefaultSampleRate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
    const int new_channels = frame->ch_layout.nb_channels;
#else
    const int new_channels = frame->channels;
#endif

    // Reuse existing swr context if the source description has not changed.
    if (m_swr_context && m_swr_in_rate == frame->sample_rate &&
        m_swr_in_format == frame->format && m_swr_in_channels == new_channels) {
        m_output_sample_rate = new_rate;
        return true;
    }

    if (m_swr_context) {
        swr_free(&m_swr_context);
    }

    m_output_sample_rate = new_rate;

#if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);

    swr_alloc_set_opts2(
        &m_swr_context,
        &out_layout,
        AV_SAMPLE_FMT_FLTP,
        m_output_sample_rate,
        &frame->ch_layout,
        static_cast<AVSampleFormat>(frame->format),
        frame->sample_rate,
        0,
        nullptr);

    av_channel_layout_uninit(&out_layout);
#else
    m_swr_context = swr_alloc_set_opts(
        nullptr,
        AV_CH_LAYOUT_STEREO,
        AV_SAMPLE_FMT_FLTP,
        m_output_sample_rate,
        frame->channel_layout,
        static_cast<AVSampleFormat>(frame->format),
        frame->sample_rate,
        0,
        nullptr);
#endif

    if (!m_swr_context || swr_init(m_swr_context) < 0) {
        blog(LOG_ERROR, "Failed to initialize audio resampler");
        if (m_swr_context) {
            swr_free(&m_swr_context);
        }
        return false;
    }

    m_swr_in_rate = frame->sample_rate;
    m_swr_in_format = frame->format;
    m_swr_in_channels = new_channels;

    return true;
}

bool AudioDecoder::decode(const uint8_t* data, size_t size, uint8_t ct,
                          std::vector<float>& left, std::vector<float>& right,
                          int& sample_rate)
{
    left.clear();
    right.clear();
    sample_rate = kDefaultSampleRate;
    if (!data || size == 0 || !m_packet || !m_frame) {
        return false;
    }

    if (!configureDecoder(ct)) {
        return false;
    }

    av_packet_unref(m_packet);
    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = static_cast<int>(size);

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

        // Fast path: AAC-LC and AAC-ELD from AirPlay are usually already
        // 44.1 kHz / FLTP / stereo, which is the OBS output format. In that
        // case skip the resampler entirely and copy planar floats directly.
        if (inputMatchesOutput(m_frame)) {
            const int n = m_frame->nb_samples;
            if (n > 0 && m_frame->extended_data && m_frame->extended_data[0] &&
                m_frame->extended_data[1]) {
                const float* l = reinterpret_cast<const float*>(m_frame->extended_data[0]);
                const float* r = reinterpret_cast<const float*>(m_frame->extended_data[1]);
                left.insert(left.end(), l, l + n);
                right.insert(right.end(), r, r + n);
                sample_rate = m_frame->sample_rate;
                produced = true;
            }
            av_frame_unref(m_frame);
            continue;
        }

        if (!configureResampler(m_frame)) {
            av_frame_unref(m_frame);
            break;
        }

        const int out_samples = av_rescale_rnd(
            swr_get_delay(m_swr_context, m_frame->sample_rate) + m_frame->nb_samples,
            m_output_sample_rate,
            m_frame->sample_rate,
            AV_ROUND_UP);

        if (out_samples <= 0) {
            av_frame_unref(m_frame);
            continue;
        }

        // Reuse a persistent planar output buffer instead of av_samples_alloc
        // / av_freep on every packet. Grow only when the per-packet sample
        // count exceeds the cached capacity.
        if (out_samples > m_swr_buf_capacity) {
            const int needed = static_cast<int>(out_samples) * sizeof(float);
            m_swr_buf_left.resize(static_cast<size_t>(needed));
            m_swr_buf_right.resize(static_cast<size_t>(needed));
            m_swr_buf_capacity = out_samples;
        }

        uint8_t* out_data[2] = {m_swr_buf_left.data(), m_swr_buf_right.data()};

        const int converted = swr_convert(
            m_swr_context,
            out_data,
            out_samples,
            const_cast<const uint8_t**>(m_frame->extended_data),
            m_frame->nb_samples);

        if (converted > 0) {
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
