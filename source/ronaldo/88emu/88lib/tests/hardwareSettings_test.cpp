#include "88lib/hardwareDevice.h"
#include "88lib/settingsChunk.h"
#include "88lib/boards/sc88pro.h"
#include "88lib/boards/sc88proSettings.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"
#include "common/test_util.hpp"
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <iostream>

using namespace emu88Lib;

int main()
{
	baseLib::disableErrorDialogs();
	const auto* folder = std::getenv("TUS_TEST_ROM_DIR");
	if(!folder) return 77;
	synthLib::RomLoader::setSearchPath(folder);
	RomLoader::rescan();
	const auto roms = RomLoader::findSc88ProRomSet();
	if(!roms.isValid()) return 77;
	auto source = std::make_unique<Sc88Pro>(roms.firmware);
	for(unsigned sample = 0; sample < g_sampleRate * 10; ++sample) source->renderSample();
	synthLib::SMidiEvent program(synthLib::MidiEventSource::Host, 0xc0, 40, 0);
	source->addMidiEvent(program);
	for(unsigned sample = 0; sample < g_sampleRate; ++sample) source->renderSample();
	SettingsChunk image;
	image.model = DeviceModel::Sc88Pro;
	image.layout = 1;
	image.firmware = baseLib::MD5(roms.firmware).getWords();
	CHECK(Sc88ProSettings::capture(*source, image.memory) == Sc88ProSettings::Result::Success);
	const auto encoded = image.encode();
	const auto decoded = SettingsChunk::decode(encoded.data(), encoded.size());
	CHECK(decoded.has_value());
	if(!decoded) return test::finish("hardwareSettings");
	synthLib::DeviceCreateParams params;
	params.customData = static_cast<uint32_t>(DeviceModel::Sc88Pro);
	BootOptions boot; // Its factory-reset default must not overwrite the supplied image.
	HardwareDevice restored(params, boot, {}, &*decoded);
	CHECK(restored.isValid());
	const auto snapshot = restored.displaySnapshot();
	std::string display;
	for(const auto& line : snapshot.screens.front().text) display += line;
	std::cout << "Restored display: " << display << '\n';
	CHECK(display.find("Violin") != std::string::npos);
	std::array<float, 4096> left{}, right{};
	synthLib::TAudioInputs inputs{};
	synthLib::TAudioOutputs outputs{left.data(), right.data()};
	std::vector<synthLib::SMidiEvent> midiOut;
	std::vector<synthLib::SMidiEvent> notes{{synthLib::MidiEventSource::Host, 0x90, 60, 100}};
	restored.process(inputs, outputs, left.size(), notes, midiOut);
	float peak = 0;
	for(const auto value : left) peak = std::max(peak, std::abs(value));
	CHECK(peak > 0.001f);
	auto wrong = image;
	wrong.firmware.front() ^= 1;
	HardwareDevice wrongFirmware(params, boot, {}, &wrong);
	CHECK(!wrongFirmware.isValid());
	wrong = image; wrong.layout = 2;
	HardwareDevice wrongLayout(params, boot, {}, &wrong);
	CHECK(!wrongLayout.isValid());
	wrong = image; wrong.model = DeviceModel::Sc88;
	HardwareDevice wrongModel(params, boot, {}, &wrong);
	CHECK(!wrongModel.isValid());
	wrong = image; wrong.memory.pop_back();
	HardwareDevice wrongSize(params, boot, {}, &wrong);
	CHECK(!wrongSize.isValid());
	CHECK(restored.isValid()); // Failed candidates cannot affect the live instance.
	const auto empty = std::filesystem::current_path() / "settings-missing-rom-fixture";
	std::filesystem::create_directory(empty);
	synthLib::RomLoader::setSearchPath(empty.string());
	RomLoader::rescan();
	HardwareDevice missing(params, boot, {}, &image);
	CHECK(!missing.isValid());
	CHECK(image.encode() == encoded); // Caller retains the complete state when assets are missing.
	return test::finish("hardwareSettings");
}
