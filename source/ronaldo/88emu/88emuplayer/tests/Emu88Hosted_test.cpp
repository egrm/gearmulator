// Hosted controls must never rewrite the standalone defaults or another instance.
#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"

int main()
{
	baseLib::disableErrorDialogs();
	juce::ScopedJuceInitialiser_GUI juceLifetime;
	const auto folder = emu88Player::defaultDataFolder();
	// CTest supplies a dedicated build-tree data root, never the user's real configuration.
	if(!std::getenv("TUS_DATA_FOLDER")) return 77;
	{
		auto defaults = emu88Player::Processor::createConfig(folder);
		defaults->setValue("outputGain", 0.75);
		defaults->setValue("outputLimiter", false);
		CHECK(defaults->saveIfNeeded());
	}
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
	{
		emu88Player::Processor first;
		CHECK_EQ(first.outputGain(), 0.75f);
		first.setOutputGain(0.25f);
		first.setOutputLimiterEnabled(true);
		{
			auto disk = emu88Player::Processor::createConfig(folder);
			CHECK_EQ(disk->getDoubleValue("outputGain"), 0.75);
			CHECK(!disk->getBoolValue("outputLimiter"));
		}
		juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);
		emu88Player::Processor second;
		CHECK_EQ(second.outputGain(), 0.75f);
		CHECK(!second.outputLimiterEnabled());
		CHECK_EQ(first.outputGain(), 0.25f);
		CHECK(first.outputLimiterEnabled());
		CHECK(!first.portMidiEnabled());
	}
	{
		auto disk = emu88Player::Processor::createConfig(folder);
		CHECK_EQ(disk->getDoubleValue("outputGain"), 0.75);
	}
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
	return test::finish("88emuHosted");
}
