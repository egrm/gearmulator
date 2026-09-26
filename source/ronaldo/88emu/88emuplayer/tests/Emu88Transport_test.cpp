// Real-ROM integration: host Stop must silence the same note as explicit All Sound Off.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "baseLib/os.h"
#include "common/test_util.hpp"
#include <iostream>

namespace
{
	struct PlayHead final : juce::AudioPlayHead
	{
		bool playing = true;
		mutable unsigned reads = 0;
		juce::Optional<PositionInfo> getPosition() const override
		{
			++reads;
			PositionInfo position;
			position.setIsPlaying(playing);
			return position;
		}
	};

	std::vector<float> render(bool hostStop)
	{
		juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
		emu88Player::Processor processor;
		CHECK(processor.hasValidRom());
		PlayHead playHead;
		processor.setPlayHead(&playHead);
		processor.setRateAndBufferSizeDetails(32000.0, 256);
		processor.prepareToPlay(32000.0, 256);
		juce::AudioBuffer<float> audio(2, 256);
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::programChange(1, 16), 0);
		midi.addEvent(juce::MidiMessage::controllerEvent(1, 91, 0), 0);
		midi.addEvent(juce::MidiMessage::controllerEvent(1, 93, 0), 0);
		// Let the program change finish before the note, without any audio device.
		for(unsigned block = 0; block < 32; ++block)
		{
			processor.processBlock(audio, midi);
			midi.clear();
		}
		midi.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(100)), 0);
		float peak = 0;
		for(unsigned block = 0; block < 64; ++block)
		{
			processor.processBlock(audio, midi);
			midi.clear();
			peak = std::max(peak, audio.getMagnitude(0, audio.getNumSamples()));
		}
		CHECK(peak > 0.01f);
		if(hostStop) playHead.playing = false;
		else midi.addEvent(juce::MidiMessage::allSoundOff(1), 0);
		std::vector<float> result;
		for(unsigned block = 0; block < 128; ++block)
		{
			processor.processBlock(audio, midi);
			midi.clear();
			for(int channel = 0; channel < audio.getNumChannels(); ++channel)
				result.insert(result.end(), audio.getReadPointer(channel),
					audio.getReadPointer(channel) + audio.getNumSamples());
		}
		CHECK(playHead.reads > 0);
		processor.setPlayHead(nullptr);
		processor.releaseResources();
		return result;
	}
}

int main()
{
	baseLib::disableErrorDialogs();
	const auto* romFolder = std::getenv("TUS_TEST_ROM_DIR");
	if(!romFolder || !std::getenv("TUS_DATA_FOLDER")) return 77;
	juce::ScopedJuceInitialiser_GUI juceLifetime;
	emu88Player::LaunchOptions launch;
	launch.values["rom-dir"] = romFolder;
	emu88Player::standaloneLaunch = &launch;
	{
		auto config = emu88Player::Processor::createConfig(emu88Player::defaultDataFolder());
		config->setValue("fastBoot", true);
		config->setValue("factoryResetOnLoad", true);
		config->setValue("deviceModel", static_cast<int>(emu88Lib::DeviceModel::Sc88Pro));
		config->setValue("outputGain", 1.0);
		config->setValue("outputLimiter", false);
		CHECK(config->saveIfNeeded());
	}
	const auto reference = render(false);
	const auto stopped = render(true);
	CHECK(reference == stopped);
	std::cout << "Compared " << reference.size() << " samples after host Stop against All Sound Off\n";
	emu88Player::standaloneLaunch = nullptr;
	return test::finish("88emuTransport");
}
