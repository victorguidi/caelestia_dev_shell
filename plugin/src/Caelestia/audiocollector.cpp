#include "audiocollector.hpp"

#include "service.hpp"
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <pipewire/pipewire.h>
#include <qdebug.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <stop_token>
#include <vector>

namespace caelestia {

PipeWireWorker::PipeWireWorker(std::stop_token token, AudioCollector* collector)
    : m_loop(nullptr)
    , m_stream(nullptr)
    , m_timer(nullptr)
    , m_idle(true)
    , m_token(token)
    , m_collector(collector) {
    pw_init(nullptr, nullptr);

    m_loop = pw_main_loop_new(nullptr);
    if (!m_loop) {
        qWarning() << "PipeWireWorker::init: failed to create PipeWire main loop";
        return;
    }

    timespec timeout = { 0, 10 * SPA_NSEC_PER_MSEC };
    m_timer = pw_loop_add_timer(pw_main_loop_get_loop(m_loop), handleTimeout, this);
    pw_loop_update_timer(pw_main_loop_get_loop(m_loop), m_timer, &timeout, &timeout, false);

    auto props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music", nullptr);
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", nextPowerOf2(512 * collector->sampleRate() / 48000),
        collector->sampleRate());
    pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
    pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    pw_properties_set(props, PW_KEY_STREAM_DONT_REMIX, "false");
    pw_properties_set(props, "channelmix.upmix", "true");

    std::vector<uint8_t> buffer(collector->chunkSize());
    spa_pod_builder b;
    spa_pod_builder_init(&b, buffer.data(), static_cast<uint32_t>(buffer.size()));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = collector->sampleRate();
    info.channels = 1;

    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_events events{};
    events.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char*) {
        auto* self = static_cast<PipeWireWorker*>(data);
        self->streamStateChanged(state);
    };
    events.process = [](void* data) {
        auto* self = static_cast<PipeWireWorker*>(data);
        self->processStream();
    };

    m_stream = pw_stream_new_simple(pw_main_loop_get_loop(m_loop), "caelestia-shell", props, &events, this);

    pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);

    pw_main_loop_run(m_loop);

    pw_stream_destroy(m_stream);
    pw_main_loop_destroy(m_loop);
    pw_deinit();
}

void PipeWireWorker::handleTimeout(void* data, uint64_t expirations) {
    auto* self = static_cast<PipeWireWorker*>(data);

    if (self->m_token.stop_requested()) {
        pw_main_loop_quit(self->m_loop);
        return;
    }

    if (!self->m_idle) {
        if (expirations < 10) {
            self->m_collector->clearBuffer();
        } else {
            self->m_idle = true;
            timespec timeout = { 0, 500 * SPA_NSEC_PER_MSEC };
            pw_loop_update_timer(pw_main_loop_get_loop(self->m_loop), self->m_timer, &timeout, &timeout, false);
        }
    }
}

void PipeWireWorker::streamStateChanged(pw_stream_state state) {
    m_idle = false;
    switch (state) {
    case PW_STREAM_STATE_PAUSED: {
        timespec timeout = { 0, 10 * SPA_NSEC_PER_MSEC };
        pw_loop_update_timer(pw_main_loop_get_loop(m_loop), m_timer, &timeout, &timeout, false);
        break;
    }
    case PW_STREAM_STATE_STREAMING:
        pw_loop_update_timer(pw_main_loop_get_loop(m_loop), m_timer, nullptr, nullptr, false);
        break;
    case PW_STREAM_STATE_ERROR:
        pw_main_loop_quit(m_loop);
        break;
    default:
        break;
    }
}

void PipeWireWorker::processStream() {
    if (m_token.stop_requested()) {
        pw_main_loop_quit(m_loop);
        return;
    }

    pw_buffer* buffer = pw_stream_dequeue_buffer(m_stream);
    if (buffer == nullptr) {
        return;
    }

    const spa_buffer* buf = buffer->buffer;
    const int16_t* samples = reinterpret_cast<const int16_t*>(buf->datas[0].data);
    if (samples == nullptr) {
        return;
    }

    const uint32_t count = buf->datas[0].chunk->size / 2;
    m_collector->loadChunk(samples, count);

    pw_stream_queue_buffer(m_stream, buffer);
}

unsigned int PipeWireWorker::nextPowerOf2(unsigned int n) {
    if (n == 0) {
        return 1;
    }

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;

    return n;
}

AudioCollector::AudioCollector(uint32_t sampleRate, uint32_t chunkSize, QObject* parent)
    : Service(parent)
    , m_buffer1(chunkSize)
    , m_buffer2(chunkSize)
    , m_readBuffer(&m_buffer1)
    , m_writeBuffer(&m_buffer2)
    , m_sampleRate(sampleRate)
    , m_chunkSize(chunkSize) {}

AudioCollector::~AudioCollector() {
    stop();
}

AudioCollector* AudioCollector::instance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_instance == nullptr) {
        s_instance = new AudioCollector();
    }
    return s_instance;
}

uint32_t AudioCollector::sampleRate() const {
    return m_sampleRate;
}

uint32_t AudioCollector::chunkSize() const {
    return m_chunkSize;
}

void AudioCollector::clearBuffer() {
    auto* writeBuffer = m_writeBuffer.load(std::memory_order_relaxed);
    std::fill(writeBuffer->begin(), writeBuffer->end(), 0.0f);

    auto* oldRead = m_readBuffer.exchange(writeBuffer, std::memory_order_acq_rel);
    m_writeBuffer.store(oldRead, std::memory_order_release);
}

void AudioCollector::loadChunk(const int16_t* samples, uint32_t count) {
    if (count > m_chunkSize) {
        count = m_chunkSize;
    }

    auto* writeBuffer = m_writeBuffer.load(std::memory_order_relaxed);
    std::transform(samples, samples + count, writeBuffer->begin(), [](int16_t sample) {
        return sample / 32768.0f;
    });

    auto* oldRead = m_readBuffer.exchange(writeBuffer, std::memory_order_acq_rel);
    m_writeBuffer.store(oldRead, std::memory_order_release);
}

uint32_t AudioCollector::readChunk(float* out, uint32_t count) {
    if (count == 0 || count > m_chunkSize) {
        count = m_chunkSize;
    }

    auto* readBuffer = m_readBuffer.load(std::memory_order_acquire);
    std::memcpy(out, readBuffer->data(), count * sizeof(float));

    return count;
}

uint32_t AudioCollector::readChunk(double* out, uint32_t count) {
    if (count == 0 || count > m_chunkSize) {
        count = m_chunkSize;
    }

    auto* readBuffer = m_readBuffer.load(std::memory_order_acquire);
    std::transform(readBuffer->begin(), readBuffer->begin() + count, out, [](float sample) {
        return static_cast<double>(sample);
    });

    return count;
}

void AudioCollector::start() {
    if (m_thread.joinable()) {
        return;
    }

    clearBuffer();

    m_thread = std::jthread([this](std::stop_token token) {
        PipeWireWorker worker(token, this);
    });
}

void AudioCollector::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_thread.join();
    }
}

} // namespace caelestia
