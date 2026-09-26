#include "88lib/boards/sc88proSettings.h"
#include "88lib/boards/sc88pro.h"

#include <algorithm>
#include <array>
#include <limits>

namespace emu88Lib
{
	namespace
	{
		// CPU-order hash from romRegistry.h. Other revisions need their own verified map.
		constexpr baseLib::MD5 g_supportedFirmware("9d4c2f123b4451d8ee75c3b982760f28");
		constexpr auto g_wordBytes = sizeof(uint16_t);
		constexpr auto g_byteBits = std::numeric_limits<uint8_t>::digits;
		// SC-88Pro Owner's Manual MIDI implementation: two groups of sixteen parts.
		constexpr size_t g_partCount = 32;
		constexpr uint8_t g_midiDataMask = 0x7f;
		// MIDI key numbers span0..127, inclusive.
		constexpr size_t g_keyCount = 128;
		// Same offline startup as HardwareDevice::g_fastBootSeconds. The additional native
		// second is the controlled settling interval verified by sc88pro_state_probe.cpp.
		constexpr unsigned g_bootSeconds = 10;
		constexpr unsigned g_bootSamples = g_sampleRate * g_bootSeconds;
		constexpr unsigned g_settleSamples = g_sampleRate;

		// ROM 1.02 handlers: bend3F19, pressure3EC0, modulation40A5, portamento40F1,
		// expression41B5, hold41F5, soft42D4, assignable3F96/3FAB, bank4001/4037,
		// RPN/NRPN4525..455A and CC84 43F8. Each part has a two-byte stride.
		constexpr std::array g_receiveBytes{
			0xc660, 0xc661, 0xc6a0, 0xc6a1, 0xc6e0, 0xc6e1, 0xc720, 0xc721,
			0xc760, 0xc761, 0xc820, 0xc860, 0xc861, 0xc8a0, 0xc8e0, 0xc8e1, 0xc920, 0xc921};
		// Consumer handlers2AF2/2AF7,2B9A/2BD2,2B32/2B37. Other bits and the
		// sostenuto active-key table D37A are voice history and remain freshly booted.
		constexpr size_t g_switchBase = 0xd5ba;
		constexpr uint8_t g_switchMask = 0xe0;
		// Poly pressure handler3E98 and reset4DF6 establish one byte per key per part.
		constexpr size_t g_polyPressureBase = 0xb660;
		// The six MIDI handlers OR their modulation dependency masks into work flags.
		constexpr std::array g_dirtyMasks{
			std::pair{0xb3a0, 0xb520}, std::pair{0xb360, 0xb4e0}, std::pair{0xb3e0, 0xb560},
			std::pair{0xb420, 0xb5a0}, std::pair{0xb460, 0xb5e0}, std::pair{0xb4a0, 0xb620}};
		// EFX receive callbacks01:AE74/01:AEB4 retain these global inputs separately.
		constexpr std::array g_effectControls{0x4632, 0x4633};
		// ROM01:AEF4 maps the20 base parameters (manual pp196-197,40 03 03..16).
		constexpr size_t g_effectParameterTable = 0x1aef4;
		constexpr size_t g_effectParameterCount = 20;
		// Roland GS DT1 framing. Header's device ID is replaced with the runtime register.
		constexpr std::array<uint8_t, 8> g_effectHeader{0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x03, 0x03};
		constexpr size_t g_deviceIdIndex = 2;
		constexpr size_t g_addressIndex = 5;
		constexpr uint8_t g_sysexEnd = 0xf7;
		// Firmware00:52AE programs the port-A device ID into sub-MCU register D0.
		constexpr uint32_t g_deviceIdRegister = 0xe000d0;

		uint16_t readWord(const std::vector<uint8_t>& _bytes, const size_t _offset)
		{
			return static_cast<uint16_t>((uint16_t{_bytes[_offset]} << g_byteBits) |
			                            _bytes[_offset + sizeof(uint8_t)]);
		}
	}

	bool Sc88ProSettings::isCaptureBoundary(const Sc88Pro& board, const bool allowHeld)
	{
		if(!board.isValid() || board.m_firmwareHash != g_supportedFirmware || board.m_serialMidi ||
		   board.m_machine.cpu().in_slice()) return false;
		// Firmware 1.02 disassembly and executed boundary fixtures in
		// docs/research/88emu-state-capture-design.md: empty dispatcher loop,
		// interrupt nesting, scanned/stable matrix, and scanner/UI task sleep records.
		constexpr std::array<uint32_t, 3> idlePc{0x065b, 0x065f, 0x0662};
		constexpr size_t interruptNesting = 0xf82e, matrix = 0x466a, stableMatrix = 0x466e;
		constexpr size_t panelWord = 0x4666, stablePanelWord = 0x4668, deferredKeys = 0x4696;
		constexpr size_t uiTask = 0xf850, scannerTask = 0xf840;
		constexpr uint8_t uiSleeping = 0x89, scannerSleeping = 0x81;
		constexpr uint8_t uiWorkMask = 9, scannerWorkMask = 1, deferredMask = 1;
		const auto& registers = board.m_machine.cpu().regs();
		const auto pc = board.m_machine.cpu().code_addr(registers.pc);
		const auto& ram = board.m_sram;
		if(std::find(idlePc.begin(), idlePc.end(), pc) == idlePc.end() ||
		   readWord(ram, interruptNesting) != uint16_t{} || board.midiInBacklog() != size_t{}) return false;
		if(!allowHeld && board.m_buttons != uint32_t{}) return false;
		for(size_t column{}; column < sizeof(board.m_buttons); ++column)
			if(ram[matrix + column] != static_cast<uint8_t>(~(board.m_buttons >> (column * g_byteBits))) ||
			   ram[stableMatrix + column] != ram[matrix + column]) return false;
		if(!allowHeld)
			for(size_t key{}; key < std::numeric_limits<decltype(board.m_buttons)>::digits; ++key)
				if(ram[deferredKeys + key] & deferredMask) return false;
		return readWord(ram, panelWord) == readWord(ram, stablePanelWord) &&
		       ram[uiTask] == uiSleeping && !(ram[uiTask + sizeof(uint8_t)] & uiWorkMask) &&
		       ram[scannerTask] == scannerSleeping && !(ram[scannerTask + sizeof(uint8_t)] & scannerWorkMask);
	}

	Sc88ProSettings::Result Sc88ProSettings::capture(const Sc88Pro& _board, std::vector<uint8_t>& _image)
	{
		if(!_board.isValid() || _board.m_firmwareHash != g_supportedFirmware)
			return Result::UnsupportedFirmware;
		_image = _board.m_sram;
		return Result::Success;
	}

	Sc88ProSettings::Result Sc88ProSettings::restore(Sc88Pro& _board, const std::vector<uint8_t>& _image)
	{
		if(!_board.isValid() || _board.m_firmwareHash != g_supportedFirmware)
			return Result::UnsupportedFirmware;
		if(_image.size() != Sc88Pro::SramSize)
			return Result::InvalidImage;
		if(_board.m_samplesRendered != unsigned{})
			return Result::RequiresFreshBoard;

		// Resolve and validate refresh data before mutating the candidate. The outer state
		// envelope must additionally validate version, firmware identity and payload integrity.
		synthLib::SMidiEvent refresh(synthLib::MidiEventSource::Host);
		refresh.sysex.assign(g_effectHeader.begin(), g_effectHeader.end());
		for(size_t parameter{}; parameter < g_effectParameterCount; ++parameter)
		{
			const auto address = readWord(_board.m_rom, g_effectParameterTable + parameter * g_wordBytes);
			const auto value = _image[address];
			if(value > g_midiDataMask) return Result::InvalidImage;
			refresh.sysex.push_back(value);
		}
		int checksum{};
		for(size_t index = g_addressIndex; index < refresh.sysex.size(); ++index)
			checksum += refresh.sysex[index];
		refresh.sysex.push_back(static_cast<uint8_t>(-checksum & g_midiDataMask));
		refresh.sysex.push_back(g_sysexEnd);

		_board.m_sram = _image;
		for(unsigned sample{}; sample < g_bootSamples; ++sample) _board.renderSample();
		auto& memory = _board.m_sram;
		for(size_t part{}; part < g_partCount; ++part)
		{
			const auto stride = part * g_wordBytes;
			for(const auto base : g_receiveBytes) memory[base + stride] = _image[base + stride];
			const auto switches = g_switchBase + stride;
			memory[switches] = static_cast<uint8_t>((memory[switches] & ~g_switchMask) | (_image[switches] & g_switchMask));
			const auto poly = g_polyPressureBase + part * g_keyCount;
			std::copy_n(_image.begin() + poly, g_keyCount, memory.begin() + poly);
			for(const auto& [source, target] : g_dirtyMasks)
			{
				const auto value = readWord(memory, source + stride) | readWord(memory, target + stride);
				memory[target + stride] = static_cast<uint8_t>(value >> g_byteBits);
				memory[target + stride + sizeof(uint8_t)] = static_cast<uint8_t>(value);
			}
		}
		bool effectsChanged = false;
		for(const auto address : g_effectControls)
		{
			effectsChanged |= memory[address] != _image[address];
			memory[address] = _image[address];
		}
		if(effectsChanged)
		{
			refresh.sysex[g_deviceIdIndex] = _board.extRead8(g_deviceIdRegister);
			_board.addMidiEvent(refresh);
		}
		for(unsigned sample{}; sample < g_settleSamples; ++sample) _board.renderSample();
		return Result::Success;
	}
}
