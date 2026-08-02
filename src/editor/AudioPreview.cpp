#include "AudioPreview.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

#define STB_VORBIS_HEADER_ONLY
#pragma warning(push, 2)
#include "repak/thirdparty/stb/stb_vorbis.c"
#pragma warning(pop)
#undef STB_VORBIS_HEADER_ONLY

namespace fs = std::filesystem;

namespace firestar::editor
{
    namespace
    {
        template <typename T>
        [[nodiscard]] T ReadLittleEndian(const std::vector<std::uint8_t>& bytes, const size_t offset)
        {
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        [[nodiscard]] bool ReadFile(const fs::path& path, std::vector<std::uint8_t>& bytes, std::string& error)
        {
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
            {
                error = "Could not open the audio source.";
                return false;
            }
            const std::streamoff length = input.tellg();
            if (length <= 0 || static_cast<std::uint64_t>(length) > (std::numeric_limits<size_t>::max)())
            {
                error = "The audio source is empty or too large.";
                return false;
            }
            bytes.resize(static_cast<size_t>(length));
            input.seekg(0, std::ios::beg);
            if (!input.read(reinterpret_cast<char*>(bytes.data()), length))
            {
                error = "The audio source could not be read.";
                return false;
            }
            return true;
        }

        [[nodiscard]] bool DecodeWave(const std::vector<std::uint8_t>& bytes,
            std::vector<std::int16_t>& output, std::uint32_t& sampleRate,
            std::uint16_t& channels, std::string& error)
        {
            constexpr std::uint32_t Riff = 0x46464952;
            constexpr std::uint32_t Wave = 0x45564157;
            constexpr std::uint32_t Format = 0x20746D66;
            constexpr std::uint32_t Data = 0x61746164;
            if (bytes.size() < 12 || ReadLittleEndian<std::uint32_t>(bytes, 0) != Riff ||
                ReadLittleEndian<std::uint32_t>(bytes, 8) != Wave)
            {
                error = "The file is not a RIFF/WAVE audio source.";
                return false;
            }

            std::uint16_t formatTag{};
            std::uint16_t bitsPerSample{};
            std::uint16_t blockAlign{};
            size_t dataOffset{};
            size_t dataSize{};
            bool foundFormat{};
            for (size_t cursor = 12; cursor + 8 <= bytes.size();)
            {
                const std::uint32_t id = ReadLittleEndian<std::uint32_t>(bytes, cursor);
                const std::uint32_t chunkSize = ReadLittleEndian<std::uint32_t>(bytes, cursor + 4);
                const size_t payload = cursor + 8;
                if (chunkSize > bytes.size() - payload)
                {
                    error = "The WAV contains a truncated chunk.";
                    return false;
                }
                if (id == Format && chunkSize >= 16)
                {
                    formatTag = ReadLittleEndian<std::uint16_t>(bytes, payload);
                    channels = ReadLittleEndian<std::uint16_t>(bytes, payload + 2);
                    sampleRate = ReadLittleEndian<std::uint32_t>(bytes, payload + 4);
                    blockAlign = ReadLittleEndian<std::uint16_t>(bytes, payload + 12);
                    bitsPerSample = ReadLittleEndian<std::uint16_t>(bytes, payload + 14);
                    foundFormat = true;
                }
                else if (id == Data && dataSize == 0)
                {
                    dataOffset = payload;
                    dataSize = chunkSize;
                }
                cursor = payload + chunkSize + (chunkSize & 1u);
            }
            if (!foundFormat || dataSize == 0 || channels == 0 || sampleRate == 0)
            {
                error = "The WAV is missing valid format or sample data.";
                return false;
            }
            if (formatTag != 1 && formatTag != 3)
            {
                error = "The WAV must contain PCM or 32-bit floating-point samples.";
                return false;
            }
            if ((formatTag == 3 && bitsPerSample != 32) ||
                (formatTag == 1 && bitsPerSample != 8 && bitsPerSample != 16 &&
                    bitsPerSample != 24 && bitsPerSample != 32))
            {
                error = "The WAV sample depth is not supported by the preview player.";
                return false;
            }
            const size_t bytesPerSample = bitsPerSample / 8;
            const size_t expectedBlockAlign = static_cast<size_t>(channels) * bytesPerSample;
            if (expectedBlockAlign == 0 || expectedBlockAlign > (std::numeric_limits<std::uint16_t>::max)())
            {
                error = "The WAV block alignment is invalid.";
                return false;
            }
            // Some source files in the Apex audio tooling have a stale mono
            // block-align value even though the sample data is interleaved
            // stereo. RePak already normalises that metadata when building;
            // decode the preview with the same canonical frame stride.
            blockAlign = static_cast<std::uint16_t>(expectedBlockAlign);
            const size_t frames = dataSize / blockAlign;
            if (frames == 0 || frames > (std::numeric_limits<size_t>::max)() / channels)
            {
                error = "The WAV contains no playable sample frames.";
                return false;
            }
            output.resize(frames * channels);
            for (size_t frame = 0; frame < frames; ++frame)
            {
                const size_t frameOffset = dataOffset + frame * blockAlign;
                for (size_t channel = 0; channel < channels; ++channel)
                {
                    const size_t offset = frameOffset + channel * bytesPerSample;
                    std::int32_t sample{};
                    if (formatTag == 3)
                    {
                        float value{};
                        std::memcpy(&value, bytes.data() + offset, sizeof(value));
                        value = (std::clamp)(value, -1.0f, 1.0f);
                        sample = static_cast<std::int32_t>(std::lround(value * 32767.0f));
                    }
                    else if (bitsPerSample == 8)
                        sample = (static_cast<std::int32_t>(bytes[offset]) - 128) << 8;
                    else if (bitsPerSample == 16)
                        sample = ReadLittleEndian<std::int16_t>(bytes, offset);
                    else if (bitsPerSample == 24)
                    {
                        sample = static_cast<std::int32_t>(bytes[offset]) |
                            (static_cast<std::int32_t>(bytes[offset + 1]) << 8) |
                            (static_cast<std::int32_t>(bytes[offset + 2]) << 16);
                        if ((sample & 0x00800000) != 0) sample |= static_cast<std::int32_t>(0xFF000000);
                        sample >>= 8;
                    }
                    else
                        sample = ReadLittleEndian<std::int32_t>(bytes, offset) >> 16;
                    output[frame * channels + channel] = static_cast<std::int16_t>((std::clamp)(sample, -32768, 32767));
                }
            }
            return true;
        }

        [[nodiscard]] bool DecodeVorbis(const std::vector<std::uint8_t>& bytes,
            std::vector<std::int16_t>& output, std::uint32_t& sampleRate,
            std::uint16_t& channels, std::string& error)
        {
            if (bytes.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
            {
                error = "The OGG source is too large for the preview decoder.";
                return false;
            }
            int decodedChannels{};
            int decodedRate{};
            short* decoded{};
            const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()),
                &decodedChannels, &decodedRate, &decoded);
            if (frames <= 0 || !decoded || decodedChannels <= 0 || decodedChannels > 64 || decodedRate <= 0)
            {
                std::free(decoded);
                error = "The OGG/Vorbis source could not be decoded.";
                return false;
            }
            const size_t sampleCount = static_cast<size_t>(frames) * static_cast<size_t>(decodedChannels);
            output.assign(decoded, decoded + sampleCount);
            std::free(decoded);
            channels = static_cast<std::uint16_t>(decodedChannels);
            sampleRate = static_cast<std::uint32_t>(decodedRate);
            return true;
        }
    }

    AudioPreviewPlayer::~AudioPreviewPlayer()
    {
        Reset();
    }

    bool AudioPreviewPlayer::Load(const fs::path& path, std::string& error)
    {
        Reset();
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(path, bytes, error)) return false;
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        bool decoded{};
        if (extension == ".ogg" || extension == ".oga")
            decoded = DecodeVorbis(bytes, pcmSamples_, sampleRate_, channels_, error);
        else
            decoded = DecodeWave(bytes, pcmSamples_, sampleRate_, channels_, error);
        if (!decoded)
        {
            pcmSamples_.clear();
            return false;
        }
        sourcePath_ = path;
        BuildWaveform();
        if (!EnsureEngine(error))
        {
            Reset();
            return false;
        }
        return true;
    }

    void AudioPreviewPlayer::Reset()
    {
        DestroySourceVoice();
        if (masteringVoice_)
        {
            masteringVoice_->DestroyVoice();
            masteringVoice_ = nullptr;
        }
        if (engine_)
        {
            engine_->Release();
            engine_ = nullptr;
        }
        sourcePath_.clear();
        pcmSamples_.clear();
        waveform_.clear();
        sampleRate_ = 0;
        channels_ = 0;
        pausedFrame_ = 0;
        playing_ = false;
    }

    bool AudioPreviewPlayer::EnsureEngine(std::string& error)
    {
        if (engine_ && masteringVoice_) return true;
        HRESULT result = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(result) || !engine_)
        {
            error = "Windows could not initialise the audio preview engine.";
            return false;
        }
        result = engine_->CreateMasteringVoice(&masteringVoice_);
        if (FAILED(result) || !masteringVoice_)
        {
            error = "Windows could not open an audio output device.";
            return false;
        }
        return true;
    }

    bool AudioPreviewPlayer::CreateVoiceAt(const std::uint64_t frame, std::string* const error)
    {
        DestroySourceVoice();
        if (!engine_ || pcmSamples_.empty() || channels_ == 0 || sampleRate_ == 0) return false;
        const std::uint64_t totalFrames = pcmSamples_.size() / channels_;
        if (frame >= totalFrames) return false;
        const std::uint64_t startSample = frame * channels_;
        const std::uint64_t byteCount = (pcmSamples_.size() - startSample) * sizeof(std::int16_t);
        if (byteCount > (std::numeric_limits<UINT32>::max)())
        {
            if (error) *error = "The decoded audio is too large for the preview player.";
            return false;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = channels_;
        format.nSamplesPerSec = sampleRate_;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(channels_ * sizeof(std::int16_t));
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        HRESULT result = engine_->CreateSourceVoice(&sourceVoice_, &format);
        if (FAILED(result) || !sourceVoice_)
        {
            if (error) *error = "Windows could not create an audio preview voice.";
            return false;
        }
        XAUDIO2_BUFFER buffer{};
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = static_cast<UINT32>(byteCount);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(pcmSamples_.data() + startSample);
        result = sourceVoice_->SubmitSourceBuffer(&buffer);
        if (FAILED(result))
        {
            if (error) *error = "Windows could not queue the decoded audio.";
            DestroySourceVoice();
            return false;
        }
        result = sourceVoice_->SetVolume(volume_);
        if (FAILED(result))
        {
            if (error) *error = "Windows could not set the audio preview volume.";
            DestroySourceVoice();
            return false;
        }
        pausedFrame_ = frame;
        return true;
    }

    void AudioPreviewPlayer::DestroySourceVoice()
    {
        if (!sourceVoice_) return;
        sourceVoice_->Stop(0);
        sourceVoice_->DestroyVoice();
        sourceVoice_ = nullptr;
    }

    std::uint64_t AudioPreviewPlayer::PositionFrame() const
    {
        const std::uint64_t totalFrames = channels_ ? pcmSamples_.size() / channels_ : 0;
        if (!sourceVoice_ || !playing_) return (std::min)(pausedFrame_, totalFrames);
        XAUDIO2_VOICE_STATE state{};
        sourceVoice_->GetState(&state);
        return (std::min)(pausedFrame_ + state.SamplesPlayed, totalFrames);
    }

    bool AudioPreviewPlayer::Play(std::string& error)
    {
        error.clear();
        if (!IsLoaded())
        {
            error = "Load an audio source before pressing Play.";
            return false;
        }
        if (playing_) return true;
        const std::uint64_t totalFrames = pcmSamples_.size() / channels_;
        if (pausedFrame_ >= totalFrames) pausedFrame_ = 0;
        if (!sourceVoice_ && !CreateVoiceAt(pausedFrame_, &error)) return false;
        const HRESULT result = sourceVoice_->Start(0);
        if (FAILED(result))
        {
            error = "Windows could not start the audio preview output.";
            DestroySourceVoice();
            return false;
        }
        playing_ = true;
        return true;
    }

    void AudioPreviewPlayer::Pause()
    {
        if (!playing_) return;
        pausedFrame_ = PositionFrame();
        playing_ = false;
        DestroySourceVoice();
    }

    void AudioPreviewPlayer::Stop()
    {
        playing_ = false;
        pausedFrame_ = 0;
        DestroySourceVoice();
    }

    void AudioPreviewPlayer::Seek(const double seconds)
    {
        if (!IsLoaded()) return;
        const bool resume = playing_;
        const std::uint64_t totalFrames = pcmSamples_.size() / channels_;
        const double clamped = (std::clamp)(seconds, 0.0, DurationSeconds());
        pausedFrame_ = (std::min)(static_cast<std::uint64_t>(clamped * sampleRate_), totalFrames);
        playing_ = false;
        DestroySourceVoice();
        if (resume && pausedFrame_ < totalFrames)
        {
            if (CreateVoiceAt(pausedFrame_) && SUCCEEDED(sourceVoice_->Start(0))) playing_ = true;
        }
    }

    void AudioPreviewPlayer::Update()
    {
        if (!playing_ || !sourceVoice_) return;
        const std::uint64_t totalFrames = pcmSamples_.size() / channels_;
        if (PositionFrame() < totalFrames) return;
        pausedFrame_ = totalFrames;
        playing_ = false;
        DestroySourceVoice();
    }

    void AudioPreviewPlayer::SetVolume(const float volume)
    {
        volume_ = (std::clamp)(volume, 0.0f, 1.0f);
        if (sourceVoice_) sourceVoice_->SetVolume(volume_);
    }

    double AudioPreviewPlayer::PositionSeconds() const
    {
        return sampleRate_ ? static_cast<double>(PositionFrame()) / sampleRate_ : 0.0;
    }

    double AudioPreviewPlayer::DurationSeconds() const
    {
        return sampleRate_ && channels_
            ? static_cast<double>(pcmSamples_.size() / channels_) / sampleRate_ : 0.0;
    }

    void AudioPreviewPlayer::BuildWaveform()
    {
        waveform_.clear();
        if (pcmSamples_.empty() || channels_ == 0) return;
        const size_t frames = pcmSamples_.size() / channels_;
        const size_t bins = (std::min<size_t>)(2048, frames);
        waveform_.resize(bins);
        for (size_t bin = 0; bin < bins; ++bin)
        {
            const size_t first = bin * frames / bins;
            const size_t last = (std::max)(first + 1, (bin + 1) * frames / bins);
            int peak{};
            for (size_t frame = first; frame < last; ++frame)
            {
                for (size_t channel = 0; channel < channels_; ++channel)
                {
                    const int sample = pcmSamples_[frame * channels_ + channel];
                    peak = (std::max)(peak, sample < 0 ? -sample : sample);
                }
            }
            waveform_[bin] = (std::min)(1.0f, static_cast<float>(peak) / 32768.0f);
        }
    }
}
