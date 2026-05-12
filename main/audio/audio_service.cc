#include "audio_service.h"
#include "board.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_aac_dec.h"
#include "impl/esp_m4a_dec.h"
#include "impl/esp_ts_dec.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <algorithm>
#include <cctype>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

namespace {

typedef union {
    esp_m4a_dec_cfg_t m4a_cfg;
    esp_ts_dec_cfg_t ts_cfg;
    esp_aac_dec_cfg_t aac_cfg;
} MusicSimpleDecoderConfig;

std::string LowerUrlPath(std::string url) {
    auto query_pos = url.find('?');
    if (query_pos != std::string::npos) {
        url.resize(query_pos);
    }
    std::transform(url.begin(), url.end(), url.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return url;
}

std::string LowerUrl(std::string url) {
    std::transform(url.begin(), url.end(), url.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return url;
}

bool EndsWith(const std::string& text, const char* suffix) {
    const auto suffix_len = std::strlen(suffix);
    return text.size() >= suffix_len && text.compare(text.size() - suffix_len, suffix_len, suffix) == 0;
}

esp_audio_simple_dec_type_t GuessMusicDecoderType(const std::string& url) {
    auto path = LowerUrlPath(url);
    auto full_url = LowerUrl(url);
    if (EndsWith(path, ".mp3") || full_url.find(".mp3") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (EndsWith(path, ".aac")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (EndsWith(path, ".wav")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    }
    if (EndsWith(path, ".ts")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    }
    if (EndsWith(path, ".m4a") || EndsWith(path, ".mp4")) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    // QQ 音乐的 C100 链接经常是 m4a，即使后续参数比较长。
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
}

void FillMusicDecoderConfig(esp_audio_simple_dec_cfg_t& cfg, MusicSimpleDecoderConfig& all_cfg) {
    switch (cfg.dec_type) {
        case ESP_AUDIO_SIMPLE_DEC_TYPE_AAC:
            all_cfg.aac_cfg.aac_plus_enable = true;
            cfg.cfg_size = sizeof(esp_aac_dec_cfg_t);
            break;
        case ESP_AUDIO_SIMPLE_DEC_TYPE_M4A:
            all_cfg.m4a_cfg.aac_plus_enable = true;
            cfg.cfg_size = sizeof(esp_m4a_dec_cfg_t);
            break;
        case ESP_AUDIO_SIMPLE_DEC_TYPE_TS:
            all_cfg.ts_cfg.aac_plus_enable = true;
            cfg.cfg_size = sizeof(esp_ts_dec_cfg_t);
            break;
        default:
            break;
    }
}

void ConvertDecodedPcmToMono16(const uint8_t* pcm_bytes, size_t byte_count, int channels, std::vector<int16_t>& out) {
    auto samples = byte_count / sizeof(int16_t);
    const auto* samples16 = reinterpret_cast<const int16_t*>(pcm_bytes);
    if (channels <= 1) {
        out.assign(samples16, samples16 + samples);
        return;
    }

    auto frames = samples / channels;
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        int mixed = 0;
        for (int ch = 0; ch < channels; ++ch) {
            mixed += samples16[i * channels + ch];
        }
        out[i] = static_cast<int16_t>(mixed / channels);
    }
}

}  // namespace

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    StopMusicPlayback();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    StopMusicPlayback();
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the wake word and/or audio processor */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    audio_processor_->Feed(std::move(data));
                }
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (music_playing_) {
            continue;
        }

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            if (!music_playing_) {
                codec_->OutputData(task->pcm);
            }
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            lock.lock();
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            if (music_playing_) {
                audio_decode_queue_.clear();
                audio_queue_cv_.notify_all();
                continue;
            }
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                    lock.lock();
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
                lock.lock();
            }
            debug_statistics_.decode_count++;
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);
    /* Push the task to the encode queue */
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    /* If the task is to send queue, we need to set the timestamp */
    if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
        if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
            task->timestamp = timestamp_queue_.front();
        } else {
            ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
        }
        timestamp_queue_.pop_front();
    }

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (music_playing_) {
        return false;
    }
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (wake_word_) {
        wake_word_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (wake_word_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "Failed to initialize wake word");
                return;
            }
            wake_word_initialized_ = true;
        }
        // Reset input resampler to clear cached data from previous mode (e.g. AudioProcessor)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        /* We should make sure no audio is playing */
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // Reset input resampler to clear cached data from previous mode (e.g. WakeWord)
        // This prevents buffer overflow when switching between different feed sizes
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    if (!audio_processor_initialized_) {
        audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
        audio_processor_initialized_ = true;
    }

    audio_processor_->EnableDeviceAec(enable);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlayMusicUrl(const std::string& url) {
    if (url.empty()) {
        ESP_LOGW(TAG, "Skip music playback: empty url");
        return;
    }

    StopMusicPlayback();
    ResetDecoder();
    music_url_ = url;
    music_stop_requested_ = false;
    music_paused_ = false;
    music_playing_ = true;
    if (xTaskCreate([](void* arg) {
            auto* audio_service = static_cast<AudioService*>(arg);
            audio_service->MusicPlaybackTask();
            audio_service->music_playback_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "music_playback", 2048 * 10, this, 5, &music_playback_task_handle_) != pdPASS) {
        music_playback_task_handle_ = nullptr;
        music_playing_ = false;
        music_paused_ = false;
        ESP_LOGE(TAG, "Failed to create music playback task");
    }
}

bool AudioService::ToggleMusicPause() {
    if (!music_playing_) {
        ESP_LOGW(TAG, "Skip music pause toggle: no active music");
        return false;
    }
    bool paused = !music_paused_.load();
    music_paused_ = paused;
    ESP_LOGI(TAG, "Music playback %s", paused ? "paused" : "resumed");
    return !paused;
}

bool AudioService::PauseMusicPlayback() {
    if (!music_playing_) {
        return false;
    }
    if (music_paused_) {
        return false;
    }
    music_paused_ = true;
    ESP_LOGI(TAG, "Music playback paused");
    return true;
}

bool AudioService::ResumeMusicPlayback() {
    if (!music_playing_) {
        return false;
    }
    if (!music_paused_) {
        return false;
    }
    music_paused_ = false;
    ESP_LOGI(TAG, "Music playback resumed");
    return true;
}

void AudioService::StopMusicPlayback() {
    if (music_playback_task_handle_ == nullptr) {
        music_playing_ = false;
        music_paused_ = false;
        return;
    }

    music_stop_requested_ = true;
    for (int i = 0; i < 100 && music_playback_task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (music_playback_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Music playback task did not stop in time");
    }
    music_playing_ = false;
    music_paused_ = false;
}

void AudioService::MusicPlaybackTask() {
    struct MusicPlaybackGuard {
        AudioService* service;
        ~MusicPlaybackGuard() {
            service->music_playing_ = false;
            service->music_paused_ = false;
            service->music_stop_requested_ = false;
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        }
    } guard{this};

    auto url = music_url_;
    ESP_LOGI(TAG, "Start music playback: %s", url.c_str());
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        ESP_LOGE(TAG, "No network for music playback");
        return;
    }

    std::unique_ptr<Http> http;
    std::string current_url = url;
    for (int redirects = 0; redirects < 4; ++redirects) {
        http = network->CreateHttp(2);
        http->SetTimeout(15000);
        http->SetHeader("User-Agent", "Mozilla/5.0");
        http->SetHeader("Referer", "https://y.qq.com");
        http->SetHeader("Accept", "*/*");
        http->SetHeader("Range", "bytes=0-");
        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to open music url, err=%d", http->GetLastError());
            return;
        }

        auto status_code = http->GetStatusCode();
        if (status_code >= 300 && status_code < 400) {
            auto location = http->GetResponseHeader("Location");
            http->Close();
            if (location.empty()) {
                ESP_LOGE(TAG, "Music url redirected without Location header");
                return;
            }
            ESP_LOGI(TAG, "Music url redirect: %s", location.c_str());
            current_url = location;
            continue;
        }
        if (status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "Music url returned status %d", status_code);
            return;
        }
        break;
    }

    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    esp_audio_simple_dec_handle_t decoder = nullptr;
    MusicSimpleDecoderConfig all_cfg = {};
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = GuessMusicDecoderType(url),
        .dec_cfg = &all_cfg,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    FillMusicDecoderConfig(dec_cfg, all_cfg);
    auto ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "Failed to open music decoder type=%d ret=%d", dec_cfg.dec_type, ret);
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        return;
    }

    constexpr int kReadSize = 4096;
    int out_size = 8192;
    auto* in_buf = static_cast<uint8_t*>(heap_caps_malloc(kReadSize, MALLOC_CAP_8BIT));
    auto* out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_size, MALLOC_CAP_8BIT));
    if (in_buf == nullptr || out_buf == nullptr) {
        ESP_LOGE(TAG, "No memory for music decoder buffers");
        if (in_buf) heap_caps_free(in_buf);
        if (out_buf) heap_caps_free(out_buf);
        esp_audio_simple_dec_close(decoder);
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        return;
    }

    esp_ae_rate_cvt_handle_t music_resampler = nullptr;
    int current_sample_rate = 0;
    bool info_ready = false;
    std::vector<int16_t> mono_pcm;
    std::vector<int16_t> output_pcm;

    while (!music_stop_requested_) {
        while (music_paused_ && !music_stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (music_stop_requested_) {
            break;
        }

        int read_len = http->Read(reinterpret_cast<char*>(in_buf), kReadSize);
        if (read_len < 0) {
            ESP_LOGE(TAG, "Failed to read music stream: %d", read_len);
            break;
        }
        if (read_len == 0) {
            break;
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = in_buf,
            .len = static_cast<uint32_t>(read_len),
            .eos = read_len < kReadSize,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while (raw.len > 0 && !music_stop_requested_) {
            while (music_paused_ && !music_stop_requested_) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (music_stop_requested_) {
                break;
            }

            esp_audio_simple_dec_out_t out_frame = {
                .buffer = out_buf,
                .len = static_cast<uint32_t>(out_size),
                .needed_size = 0,
                .decoded_size = 0,
            };
            ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                auto* new_out = static_cast<uint8_t*>(heap_caps_realloc(out_buf, out_frame.needed_size, MALLOC_CAP_8BIT));
                if (new_out == nullptr) {
                    ESP_LOGE(TAG, "No memory to grow music output buffer to %lu", out_frame.needed_size);
                    music_stop_requested_ = true;
                    break;
                }
                out_buf = new_out;
                out_size = out_frame.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Music decode failed ret=%d", ret);
                music_stop_requested_ = true;
                break;
            }

            if (out_frame.decoded_size > 0) {
                esp_audio_simple_dec_info_t dec_info = {};
                if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK) {
                    if (!info_ready) {
                        ESP_LOGI(TAG, "Music info: sample_rate=%lu bits=%d channels=%d",
                                 dec_info.sample_rate, dec_info.bits_per_sample, dec_info.channel);
                        info_ready = true;
                    }
                    if (dec_info.bits_per_sample != 16) {
                        ESP_LOGE(TAG, "Unsupported music bits per sample: %d", dec_info.bits_per_sample);
                        music_stop_requested_ = true;
                        break;
                    }

                    ConvertDecodedPcmToMono16(out_frame.buffer, out_frame.decoded_size, dec_info.channel, mono_pcm);
                    if (static_cast<int>(dec_info.sample_rate) != codec_->output_sample_rate()) {
                        if (music_resampler == nullptr || current_sample_rate != static_cast<int>(dec_info.sample_rate)) {
                            if (music_resampler != nullptr) {
                                esp_ae_rate_cvt_close(music_resampler);
                            }
                            esp_ae_rate_cvt_cfg_t cfg = RATE_CVT_CFG(dec_info.sample_rate, codec_->output_sample_rate(), ESP_AUDIO_MONO);
                            auto resampler_ret = esp_ae_rate_cvt_open(&cfg, &music_resampler);
                            if (music_resampler == nullptr) {
                                ESP_LOGE(TAG, "Failed to create music resampler ret=%d", resampler_ret);
                                music_stop_requested_ = true;
                                break;
                            }
                            current_sample_rate = dec_info.sample_rate;
                        }

                        uint32_t max_out_samples = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(music_resampler, mono_pcm.size(), &max_out_samples);
                        output_pcm.resize(max_out_samples);
                        uint32_t actual_output = max_out_samples;
                        esp_ae_rate_cvt_process(music_resampler, reinterpret_cast<esp_ae_sample_t>(mono_pcm.data()),
                                                mono_pcm.size(), reinterpret_cast<esp_ae_sample_t>(output_pcm.data()),
                                                &actual_output);
                        output_pcm.resize(actual_output);
                        {
                            std::lock_guard<std::mutex> output_lock(output_mutex_);
                            codec_->OutputData(output_pcm);
                        }
                    } else {
                        {
                            std::lock_guard<std::mutex> output_lock(output_mutex_);
                            codec_->OutputData(mono_pcm);
                        }
                    }
                    last_output_time_ = std::chrono::steady_clock::now();
                }
            }

            if (raw.consumed == 0) {
                break;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }
    }

    if (music_resampler != nullptr) {
        esp_ae_rate_cvt_close(music_resampler);
    }
    heap_caps_free(in_buf);
    heap_caps_free(out_buf);
    esp_audio_simple_dec_close(decoder);
    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();
    http->Close();
    ESP_LOGI(TAG, "Music playback stopped");
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && audio_decode_queue_.empty() && audio_playback_queue_.empty() && audio_testing_queue_.empty();
}

void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_ || (audio_decode_queue_.empty() && audio_playback_queue_.empty()); 
    });
}

void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    timestamp_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}

bool AudioService::IsAfeWakeWord() {
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    return wake_word_ != nullptr && dynamic_cast<AfeWakeWord*>(wake_word_.get()) != nullptr;
#else
    return false;
#endif
}
