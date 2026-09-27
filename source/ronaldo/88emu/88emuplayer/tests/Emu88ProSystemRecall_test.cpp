#include "88emuplayer/Emu88Processor.h"
#include "88emuplayer/app/Emu88LaunchOptions.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88pro.h"
#include "common/test_util.hpp"
#include "baseLib/os.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	// The native --system-menu-recall fixture holds each key for 3200
	// firmware samples and releases for 8000 at 32 kHz.
	constexpr double g_hostRate = 44100;
	constexpr int g_hostBlock = 128;
	constexpr int g_heldFrames = 4410;
	constexpr int g_releasedFrames = 11025;
	constexpr size_t g_pageAddress = 0x4b47;
	constexpr size_t g_systemCursorAddress = 0x4c68;
	constexpr size_t g_selectedPartAddress = 0x4d78;
	// Firmware 1.02 table 0D:9C98 + cursor 0010 contains descriptor 4BFD;
	// 0C:9293..929C reads its current System value from page C0.
	constexpr size_t g_previewVelocityAddress = 0x4bfd;
	constexpr uint8_t g_systemPage = 1;
	constexpr uint16_t g_previewVelocityCursor = 0x10;
	constexpr uint16_t g_partA2 = 1;

	void render(emu88Player::Processor& processor, int frames)
	{
		juce::AudioBuffer<float> audio(2, frames);
		juce::MidiBuffer midi;
		processor.processBlock(audio, midi);
	}

	juce::MemoryBlock save(emu88Player::Processor& processor)
	{
		juce::MemoryBlock state;
		processor.getStateInformation(state);
		CHECK(processor.lastStateOperationSucceeded());
		CHECK(state.getSize() > 0);
		if(!state.getSize()) throw std::runtime_error("Pro System capture returned no state");
		return state;
	}

	emu88Lib::SettingsChunk hardware(const juce::MemoryBlock& state)
	{
		auto root = juce::parseXML(juce::String::fromUTF8(
			static_cast<const char*>(state.getData()), static_cast<int>(state.getSize())));
		CHECK(root != nullptr);
		if(!root) throw std::runtime_error("Pro System state has no XML envelope");
		const auto* payload = root->getChildByName("Payload");
		const auto* encoded = payload ? payload->getChildByName("Hardware") : nullptr;
		CHECK(encoded != nullptr);
		if(!encoded) throw std::runtime_error("Pro System state has no hardware image");
		juce::MemoryBlock bytes;
		CHECK(bytes.fromBase64Encoding(encoded->getAllSubText()));
		auto chunk = emu88Lib::SettingsChunk::decode(bytes.getData(), bytes.getSize());
		CHECK(chunk.has_value());
		if(!chunk) throw std::runtime_error("Pro System hardware image is invalid");
		return *chunk;
	}

	uint16_t word(const std::vector<uint8_t>& bytes, size_t address)
	{
		return uint16_t((uint16_t(bytes.at(address)) << 8) | bytes.at(address + 1));
	}

	std::string visible(emu88Player::Processor& processor)
	{
		const auto display = processor.hardwareDisplaySnapshot();
		CHECK(display.has_value());
		if(!display || display->screens.empty() || display->screens[0].text.empty())
			throw std::runtime_error("Pro System display is unavailable");
		return display->screens[0].text.front();
	}

	void press(emu88Player::Processor& processor, uint32_t buttons)
	{
		processor.setPanelButtons(buttons);
		render(processor, g_heldFrames);
		processor.setPanelButtons(0);
		render(processor, g_releasedFrames);
	}

	uint32_t button(emu88Lib::Sc88ProButton key)
	{
		return uint32_t{1} << static_cast<unsigned>(key);
	}

	void checkContext(const emu88Lib::SettingsChunk& chunk)
	{
		CHECK(chunk.model == emu88Lib::DeviceModel::Sc88Pro);
		CHECK_EQ(chunk.memory.at(g_pageAddress), g_systemPage);
		CHECK_EQ(word(chunk.memory, g_systemCursorAddress), g_previewVelocityCursor);
		CHECK_EQ(word(chunk.memory, g_selectedPartAddress), g_partA2);
	}
}

int main()
{
	if(!std::getenv("TUS_DATA_FOLDER") || !std::getenv("TUS_TEST_ROM_DIR")) return 77;
	baseLib::disableErrorDialogs();
	juce::ScopedJuceInitialiser_GUI juceLifetime;
	emu88Player::LaunchOptions launch;
	launch.values["rom-dir"] = std::getenv("TUS_TEST_ROM_DIR");
	emu88Player::standaloneLaunch = &launch;
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_VST3);

	emu88Player::Processor source;
	CHECK(source.setDeviceModel(emu88Lib::DeviceModel::Sc88Pro));
	source.setRateAndBufferSizeDetails(g_hostRate, g_hostBlock);
	source.prepareToPlay(g_hostRate, g_hostBlock);
	press(source, button(emu88Lib::Sc88ProButton::PartR));
	press(source, button(emu88Lib::Sc88ProButton::Sc55Map) |
	              button(emu88Lib::Sc88ProButton::Sc88Map));
	press(source, button(emu88Lib::Sc88ProButton::Sc88Map));
	CHECK(visible(source).find("Prevw Velo: 100") != std::string::npos);
	const auto before = hardware(save(source));
	checkContext(before);
	CHECK_EQ(before.memory.at(g_previewVelocityAddress), 100);
	press(source, button(emu88Lib::Sc88ProButton::InstR));
	CHECK(visible(source).find("Prevw Velo: 101") != std::string::npos);
	const auto edited = save(source);
	const auto editedHardware = hardware(edited);
	checkContext(editedHardware);
	CHECK_EQ(editedHardware.memory.at(g_previewVelocityAddress), 101);

	emu88Player::Processor reopened;
	reopened.setStateInformation(edited.getData(), static_cast<int>(edited.getSize()));
	CHECK(reopened.lastStateOperationSucceeded());
	reopened.setRateAndBufferSizeDetails(g_hostRate, g_hostBlock);
	reopened.prepareToPlay(g_hostRate, g_hostBlock);
	render(reopened, g_hostBlock);
	const auto reopenedHardware = hardware(save(reopened));
	checkContext(reopenedHardware);
	CHECK_EQ(reopenedHardware.memory.at(g_previewVelocityAddress), 101);
	CHECK(visible(reopened) == visible(source));
	press(source, button(emu88Lib::Sc88ProButton::InstR));
	press(reopened, button(emu88Lib::Sc88ProButton::InstR));
	CHECK(visible(source).find("Prevw Velo: 102") != std::string::npos);
	CHECK(visible(reopened) == visible(source));
	CHECK_EQ(hardware(save(source)).memory.at(g_previewVelocityAddress), 102);
	CHECK_EQ(hardware(save(reopened)).memory.at(g_previewVelocityAddress), 102);

	const auto fixture = juce::File(emu88Player::defaultDataFolder())
		.getChildFile("system-model-2.component");
	CHECK(fixture.replaceWithData(edited.getData(), edited.getSize()));
	std::cout << "Pro System Prevw Velo native component emitted\n";
	emu88Player::standaloneLaunch = nullptr;
	return test::finish("88emuProSystemRecall");
}
