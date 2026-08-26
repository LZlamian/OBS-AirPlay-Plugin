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
    , m_input_sample_rate(0)
    , m_input_sample_format(AV_SAMPLE_FMT_NONE)
    , m_resampler_configured(false)
#if LIBAVUTIL_VERSION_MAJOR < 57
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

    resetResampler();

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

#if LIBAVUTIL_VERSION_MAJOR >= 57
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
#if LIBAVUTIL_VERSION_MAJOR >= 57
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

    if (!data || size == 0 || !m_packet || !m_frame) {
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

        uint8_t* out_data[2] = {nullptr, nullptr};
        int out_linesize = 0;
        if (av_samples_alloc(out_data, &out_linesize, 2, out_samples, AV_SAMPLE_FMT_FLTP, 0) < 0) {
            av_frame_unref(m_frame);
            break;
        }

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

        if (out_data[0]) {
            av_freep(&out_data[0]);
        }

        av_frame_unref(m_frame);
    }

    return produced;
}
