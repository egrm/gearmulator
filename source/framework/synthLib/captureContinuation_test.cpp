#include "device.h"
#include "plugin.h"
#include "baseLib/os.h"
#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace
{
    class Recorder final : public synthLib::Device
    {
    public:
        Recorder() : Device({}) {}
        uint64_t sample{};
        std::vector<std::tuple<uint64_t, uint8_t, uint8_t, uint8_t>> received;
        float getSamplerate() const override { return 32000; }
        bool isValid() const override { return true; }
        bool getState(std::vector<uint8_t>&, synthLib::StateType) override { return false; }
        bool setState(const std::vector<uint8_t>&, synthLib::StateType) override { return false; }
        uint32_t getChannelCountIn() override { return 0; }
        uint32_t getChannelCountOut() override { return 2; }
        bool setDspClockPercent(uint32_t) override { return false; }
        uint32_t getDspClockPercent() const override { return 100; }
        uint64_t getDspClockHz() const override { return 0; }
    protected:
        bool sendMidi(const synthLib::SMidiEvent& event, std::vector<synthLib::SMidiEvent>&) override
        {
            received.emplace_back(sample + event.offset, event.a, event.b, event.c);
            return true;
        }
        void readMidiOut(std::vector<synthLib::SMidiEvent>&) override {}
        void processAudio(const synthLib::TAudioInputs&, const synthLib::TAudioOutputs& out, size_t count) override
        {
            for(size_t index{}; index < count; ++index, ++sample)
                for(size_t channel{}; channel < 2; ++channel) out[channel][index] = static_cast<float>(sample % 127) / 127;
        }
    };

    void require(bool condition, const char* message)
    {
        if(!condition) throw std::runtime_error(message);
    }
}

int main()
{
    baseLib::disableErrorDialogs();
    try
    {
        for(const auto mode : {synthLib::Resampler::Mode::Legacy, synthLib::Resampler::Mode::MameHq, synthLib::Resampler::Mode::MameLofi})
        {
            Recorder live;
            synthLib::Plugin engine(&live, [](auto*) { return nullptr; });
            engine.setMidiClockEnabled(false);
            engine.setResamplerMode(mode);
            engine.setHostSamplerate(48000, 0);
            engine.setBlockSize(128);
            float left[128]{}, right[128]{};
            synthLib::TAudioOutputs outputs{left, right};
            for(size_t phase{}; phase < 19; ++phase)
            {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host, 0xb0, 7, static_cast<uint8_t>(phase));
                event.offset = static_cast<uint32_t>(phase % 3);
                engine.addMidiEvent(event);
                // Tiny callback leaves previously accepted input in resampler staging.
                engine.process({}, outputs, 1, 0, 0, false, false);
                event.b = 11;
                event.offset = 37;
                engine.addMidiEvent(event);
                Recorder copy;
                copy.sample = live.sample;
                auto captured = engine.cloneForCapture(&copy);
                const auto before = live.sample;
                const auto inputBefore = live.received.size();
                captured->processCapture(128);
                require(live.sample == before && live.received.size() == inputBefore, "capture mutated live stream");
                engine.process({}, outputs, 128, 0, 0, false, false);
                std::vector<std::tuple<uint64_t, uint8_t, uint8_t, uint8_t>> expected(live.received.begin() + inputBefore, live.received.end());
                require(copy.received == expected, "pending input lost exact native offsets/order");
                require(copy.sample == live.sample, "resampler execution phase diverged");
            }
            // Ring overflow stages input in m_midiIn; both queues must be captured.
            for(unsigned index{}; index < 1100; ++index)
                engine.addMidiEvent({synthLib::MidiEventSource::Host, 0xb0, 1, static_cast<uint8_t>(index % 128)});
            Recorder copy;
            copy.sample = live.sample;
            auto captured = engine.cloneForCapture(&copy);
            const auto before = live.received.size();
            captured->processCapture(128);
            engine.process({}, outputs, 128, 0, 0, false, false);
            std::vector<std::tuple<uint64_t, uint8_t, uint8_t, uint8_t>> expected(live.received.begin() + before, live.received.end());
            require(copy.received == expected && copy.received.size() == 1100, "ring overflow capture lost or reordered MIDI");
        }
        std::puts("captureContinuation: all converter modes, 57 staging phases and ring overflow passed");
        return 0;
    }
    catch(const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
