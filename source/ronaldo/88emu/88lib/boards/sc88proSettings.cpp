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
		// ROM 0C:8B25/8BC3 select a zero-based part; 0C:8AAE changes group
		// with XOR 0010. Manual "Assigning a sound to a Part" specifies ALL +
		// PART-left for group selection.
		// These are semantic selection, not descriptor pointers.
		constexpr size_t g_selectedPart = 0x4d78;
		constexpr size_t g_partsPerGroup = g_partCount / 2;
		// ROM dispatch 0C:8748 and table 0D:8912: UserInst toggles normal
		// page 0 and effect page 7 through 0C:8AFB/8B0D. The firmware rebuilds
		// its own display and descriptors; no task, timer, or LCD bytes are copied.
		constexpr size_t g_panelPage = 0x4b47;
		// ROM 0C:94C9/9500 enter normal-part edit page 2 with both PART keys;
		// 0C:954C returns to page 0. The menu cursor is the word at 4C60,
		// committed from 4C62 by 0C:5CC8..5CCC. SC-88 MAP dispatches through
		// 0D:88C2[2] to 0C:89FF/8A0F, stepping it by 0010. The native upper
		// endpoint is the 03C0 comparison at 0C:8A13. The alternate branch when
		// FE2E bit 0 is set also permits cursor 0000 (0C:8956); this validation
		// accepts that legitimate target without claiming alternate-mode coverage.
		constexpr uint8_t g_normalPage = 0;
		constexpr uint8_t g_partEditPage = 2;
		constexpr size_t g_menuCursor = 0x4c60;
		constexpr uint16_t g_firstMenuCursor = 0x10;
		constexpr uint16_t g_lastMenuCursor = 0x3c0;
		constexpr uint16_t g_menuCursorStep = 0x10;
		constexpr size_t g_maxMenuMoves =
			(g_lastMenuCursor - g_firstMenuCursor) / g_menuCursorStep + 1;
		// ROM 0C:9507/950D enters System page 1 with both MAP keys; 0C:954C
		// exits. Page-1 handlers 0C:88FC/8908 and 0C:89CE/89E2..89F3
		// move its separate cursor 4C68 by 0010 within 0000..00C0.
		constexpr uint8_t g_systemPage = 1;
		constexpr size_t g_systemCursor = 0x4c68;
		constexpr uint16_t g_lastSystemCursor = 0xc0;
		constexpr uint16_t g_systemCursorStep = 0x10;
		constexpr size_t g_maxSystemMoves = g_lastSystemCursor / g_systemCursorStep;
		constexpr uint8_t g_userInstrumentPage = 7;
		// Same native press/gap timing as Sc88Pro::runFactoryReset, which drives
		// the actual scanner rather than invoking firmware routines out of context.
		constexpr unsigned g_panelPressSamples = g_sampleRate / 10;
		constexpr unsigned g_panelReleaseSamples = g_sampleRate / 4;
		// Firmware 0C:76B6..771D walks the 32 level-meter decay counters
		// backwards from 4ACB; 0C:5BFA decrements them on the display timer.
		// These are animation history, not sound settings. Reusing their remaining
		// ticks changes boot task timing and consequently the fresh DSP history.
		constexpr size_t g_levelMeterCountdownBase = 0x4aac;

		void pressPanel(Sc88Pro& board, const uint32_t buttons)
		{
			board.setButtons(buttons);
			for(unsigned sample{}; sample < g_panelPressSamples; ++sample) board.renderSample();
			board.setButtons(uint32_t{});
			for(unsigned sample{}; sample < g_panelReleaseSamples; ++sample) board.renderSample();
		}

		constexpr uint32_t panelButton(const Sc88ProButton button)
		{
			return uint32_t{1} << static_cast<uint8_t>(button);
		}

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
		const auto selectedPart = readWord(_image, g_selectedPart);
		if(selectedPart >= g_partCount) return Result::InvalidImage;
		const auto savedPage = _image[g_panelPage];
		const auto savedMenuCursor = readWord(_image, g_menuCursor);
		const auto hasMenuCursor = savedMenuCursor <= g_lastMenuCursor &&
		                           savedMenuCursor % g_menuCursorStep == uint16_t{};
		if(savedPage == g_partEditPage && !hasMenuCursor) return Result::InvalidImage;
		const auto savedSystemCursor = readWord(_image, g_systemCursor);
		const auto hasSystemCursor = savedSystemCursor <= g_lastSystemCursor &&
		                              savedSystemCursor % g_systemCursorStep == uint16_t{};
		if(savedPage == g_systemPage && !hasSystemCursor) return Result::InvalidImage;
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
		std::fill_n(_board.m_sram.begin() + g_levelMeterCountdownBase, g_partCount, uint8_t{});
		for(unsigned sample{}; sample < g_bootSamples; ++sample) _board.renderSample();
		// Reconstruct selection through normal firmware actions. This regenerates
		// descriptor targets and screen contents on the fresh timeline. Do it before
		// restoring receive inputs so navigation cannot reset a captured controller.
		if(selectedPart >= g_partsPerGroup)
			pressPanel(_board, panelButton(Sc88ProButton::InstAll) | panelButton(Sc88ProButton::PartL));
		for(size_t part{}; part < selectedPart % g_partsPerGroup; ++part)
			pressPanel(_board, panelButton(Sc88ProButton::PartR));
		// The firmware keeps 4C60 while other pages are visible. A matching
		// inactive cursor needs no extra panel actions on the fresh timeline.
		const auto cursorNeedsRecall = hasMenuCursor &&
		                               readWord(_board.m_sram, g_menuCursor) != savedMenuCursor;
		if(savedPage == g_partEditPage ||
		   ((savedPage == g_normalPage || savedPage == g_userInstrumentPage) && cursorNeedsRecall))
		{
			pressPanel(_board, panelButton(Sc88ProButton::PartL) | panelButton(Sc88ProButton::PartR));
			if(hasMenuCursor)
			{
				// Advance through the firmware's own descriptor selection, including
				// its special jumps. Neither the cursor nor descriptor table is copied.
				for(size_t move{}; move < g_maxMenuMoves &&
				    readWord(_board.m_sram, g_menuCursor) != savedMenuCursor; ++move)
					pressPanel(_board, panelButton(Sc88ProButton::Sc88Map));
				if(readWord(_board.m_sram, g_menuCursor) != savedMenuCursor)
					return Result::InvalidImage;
			}
			if(savedPage != g_partEditPage)
				pressPanel(_board, panelButton(Sc88ProButton::PartL) | panelButton(Sc88ProButton::PartR));
		}
		// The inactive System cursor normally survives boot. Only enter its menu
		// when the active page needs redrawing or a valid remembered cursor differs.
		const auto systemCursorNeedsRecall = hasSystemCursor &&
			readWord(_board.m_sram, g_systemCursor) != savedSystemCursor;
		if(savedPage == g_systemPage ||
		   ((savedPage == g_normalPage || savedPage == g_userInstrumentPage) &&
		    systemCursorNeedsRecall))
		{
			const auto systemChord = panelButton(Sc88ProButton::Sc55Map) |
			                         panelButton(Sc88ProButton::Sc88Map);
			if(_board.m_sram[g_panelPage] == g_systemPage) pressPanel(_board, systemChord);
			pressPanel(_board, systemChord);
			if(_board.m_sram[g_panelPage] != g_systemPage) return Result::InvalidImage;
			if(hasSystemCursor)
			{
				for(size_t move{}; move < g_maxSystemMoves &&
				    readWord(_board.m_sram, g_systemCursor) != savedSystemCursor; ++move)
				{
					const auto current = readWord(_board.m_sram, g_systemCursor);
					pressPanel(_board, panelButton(current < savedSystemCursor ?
						Sc88ProButton::Sc88Map : Sc88ProButton::Sc55Map));
				}
				if(readWord(_board.m_sram, g_systemCursor) != savedSystemCursor)
					return Result::InvalidImage;
			}
			if(savedPage != g_systemPage) pressPanel(_board, systemChord);
		}
		if(savedPage == g_userInstrumentPage)
			pressPanel(_board, panelButton(Sc88ProButton::UserInst));
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
