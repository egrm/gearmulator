#include "88lib/boards/sc155Settings.h"
#include "88lib/boards/sc55Board.h"
#include "baseLib/md5.h"

#include <algorithm>
#include <limits>

namespace emu88Lib
{
	namespace
	{
		// Registered paired SC-155 Rev1 control ROMs, romRegistry.h.
		constexpr baseLib::MD5 g_internalRom("6b74988a79c2a48239809f07a54175b3");
		constexpr baseLib::MD5 g_programRom("5c7c6ab34ef6da079b05a36fd23c9c91");
		// Observed SC-155 Rev1 control-firmware edits in sc155-panel-probe.log.
		constexpr size_t g_programPartOne = 0x129;
		constexpr size_t g_volumePartOne = 0x0c0;
		// 00:1CBE indexes A318+R1 in this paired MCU ROM. A32F changed after
		// the native note gesture; the first-generation GP has 24 voices.
		constexpr size_t g_voiceLinks = 0x2318;
		constexpr size_t g_voiceLinkCount = 24;
		// 00:04CF/04D0 is the firmware scheduler's SLEEP loop.
		constexpr uint32_t g_sleepPc = 0x04cf;
		constexpr uint32_t g_loopPc = 0x04d0;
		// 00:0609 and 00:1F0B/1F31 produce and consume the ABF6/ABF8 ring;
		// 00:1F4C/1F72 consumes the CB7C/CB7E alternate receiver ring.
		constexpr size_t g_directRead = 0x2bf6;
		constexpr size_t g_directWrite = 0x2bf8;
		constexpr size_t g_alternateRead = 0x4b7c;
		constexpr size_t g_alternateWrite = 0x4b7e;
		// 04:31F5..3253 scans three physical columns, comparing its samples
		// at CCAC..CCAE and CCB0..CCB2. 04:325E/3400 uses CCD8/CCDA
		// as the panel edge queue's read/write pointers. A released LevelR
		// leaves the third samples at DF until the firmware has scanned it.
		constexpr size_t g_previousPanel = 0x4cac;
		constexpr size_t g_currentPanel = 0x4cb0;
		constexpr size_t g_panelColumns = 3;
		constexpr size_t g_panelQueueRead = 0x4cd8;
		constexpr size_t g_panelQueueWrite = 0x4cda;
		constexpr uint8_t g_releasedRows = 0xff;
		// SC-55 player already gives its paired board ten seconds to boot.
		constexpr uint32_t g_bootSeconds = 10;
		uint16_t readWord(const std::vector<uint8_t>& bytes, const size_t offset)
		{
			return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
		}
	}

	bool Sc155Settings::firmwareMatches(const Sc55Board& board)
	{
		return board.m_valid && board.m_roms.model == DeviceModel::Sc155 &&
		       board.m_internalRomHash == g_internalRom &&
		       board.m_programRomHash == g_programRom;
	}

	bool Sc155Settings::isPanelInputBoundary(Sc55Board& board)
	{
		if(!supported(board) || board.m_machine.cpu().in_slice()) return false;
		const auto pc = board.m_machine.cpu().code_addr(board.m_machine.cpu().regs().pc);
		if(pc != g_sleepPc && pc != g_loopPc) return false;
		if(board.m_lcdIrqAt != 0 || board.m_gaIntTrigger != 0) return false;
		const auto& ram = board.m_sram;
		if(readWord(ram, g_panelQueueRead) != readWord(ram, g_panelQueueWrite)) return false;
		for(size_t column{}; column < g_panelColumns; ++column)
		{
			const auto pressed = uint8_t(board.m_buttons >>
				(column * std::numeric_limits<uint8_t>::digits));
			const auto expected = uint8_t(g_releasedRows & ~pressed);
			if(ram[g_previousPanel + column] != expected ||
			   ram[g_currentPanel + column] != expected) return false;
		}
		return true;
	}

	bool Sc155Settings::isCaptureBoundary(Sc55Board& board)
	{
		if(board.m_buttons != 0 || !isPanelInputBoundary(board)) return false;
		if(board.m_machine.sci(0).rx_pending() != 0 ||
		   (board.m_machine.sci(0).ssr(board.m_machine.now()) & h8500::Sci::kRdrf))
			return false;
		const auto& ram = board.m_sram;
		return readWord(ram, g_directRead) == readWord(ram, g_directWrite) &&
		       readWord(ram, g_alternateRead) == readWord(ram, g_alternateWrite);
	}

	Sc155Settings::Result Sc155Settings::capture(Sc55Board& board, std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(!isCaptureBoundary(board)) return Result::PendingInput;
		image = board.m_sram;
		return Result::Success;
	}

	Sc155Settings::Result Sc155Settings::restore(Sc55Board& board,
	                                            const std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(image.size() != Sc55Board::SramSize) return Result::InvalidImage;
		if(board.m_samplesRendered != 0) return Result::RequiresFreshBoard;
		board.m_sram = image;
		std::fill_n(board.m_sram.begin() + g_voiceLinks, g_voiceLinkCount, uint8_t{});
		board.powerCycle();
		for(uint32_t sample{}; sample < g_bootSeconds * board.sampleRate(); ++sample)
			board.renderSample();
		if(board.m_sram[g_programPartOne] != image[g_programPartOne] ||
		   board.m_sram[g_volumePartOne] != image[g_volumePartOne])
			return Result::InvalidImage;
		return Result::Success;
	}

	bool Sc155Settings::snapshotForProbe(Sc55Board& board, DiagnosticSnapshot& result)
	{
		if(!firmwareMatches(board) || board.m_machine.cpu().in_slice()) return false;
		result.sram = board.m_sram;
		result.pc = board.m_machine.cpu().code_addr(board.m_machine.cpu().regs().pc);
		result.sciPending = board.m_machine.sci(0).rx_pending();
		result.sciStatus = board.m_machine.sci(0).ssr(board.m_machine.now());
		result.buttons = board.m_buttons;
		return true;
	}
}
