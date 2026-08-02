#pragma once

#include <xaudio2.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace firestar::editor
{
    class AudioPreviewPlayer
    {
    public:
        AudioPreviewPlayer() = default;
        ~AudioPreviewPlayer();

        AudioPreviewPlayer(const AudioPreviewPlayer&) = delete;
        AudioPreviewPlayer& operator=(const AudioPreviewPlayer&) = delete;

        [[nodiscard]] bool Load(const std::filesystem::path& path, std::string& error);
        void Reset();
        [[nodiscard]] bool Play(std::string& error);
        void Pause();
        void Stop();
        void Seek(double seconds);
        void Update();
        void SetVolume(float volume);

        [[nodiscard]] bool IsLoaded() const { return !pcmSamples_.empty(); }
        [[nodiscard]] bool IsPlaying() const { return playing_; }
        [[nodiscard]] double PositionSeconds() const;
        [[nodiscard]] double DurationSeconds() const;
        [[nodiscard]] std::uint32_t SampleRate() const { return sampleRate_; }
        [[nodiscard]] std::uint16_t Channels() const { return channels_; }
        [[nodiscard]] float Volume() const { return volume_; }
        [[nodiscard]] const std::vector<float>& Waveform() const { return waveform_; }
        [[nodiscard]] const std::filesystem::path& SourcePath() const { return sourcePath_; }

    private:
        [[nodiscard]] bool EnsureEngine(std::string& error);
        [[nodiscard]] bool CreateVoiceAt(std::uint64_t frame, std::string* error = nullptr);
        void DestroySourceVoice();
        [[nodiscard]] std::uint64_t PositionFrame() const;
        void BuildWaveform();

        IXAudio2* engine_{};
        IXAudio2MasteringVoice* masteringVoice_{};
        IXAudio2SourceVoice* sourceVoice_{};
        std::filesystem::path sourcePath_;
        std::vector<std::int16_t> pcmSamples_;
        std::vector<float> waveform_;
        std::uint32_t sampleRate_{};
        std::uint16_t channels_{};
        std::uint64_t pausedFrame_{};
        float volume_{0.8f};
        bool playing_{};
    };
}
