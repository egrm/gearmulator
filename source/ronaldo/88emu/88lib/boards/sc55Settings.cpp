#include "88lib/boards/sc55Settings.h"
#include "88lib/boards/sc55Board.h"
#include "baseLib/md5.h"

#include <algorithm>
#include <limits>

namespace emu88Lib
{
	namespace
	{
		// Paired SC-55 1.21 H8 and program images in romRegistry.h:625-630.
		constexpr baseLib::MD5 g_internal121("462cb3a2ce9e54f4e54a52f643931ffd");
		constexpr baseLib::MD5 g_program121("6b61186953b50d900e430ae6a996bda7");
		// romRegistry.h stock GS-28 2.00 pair, shared by SC-55mkII and SC-155mkII.
		// The CTF variants have different program images and remain excluded.
		constexpr baseLib::MD5 g_mk2Internal("4ca058f7db05f51e97bb30a162e9610a");
		constexpr baseLib::MD5 g_mk2Program("63b24c7193ce34afefce9cec32ac39f0");
		// Match the existing player fast-boot interval in hardwareDevice.cpp:34.
		// The native 1.21 battery experiment also read back the edited fields
		// after one second; this longer established boot path is conservative.
		constexpr uint32_t g_bootSeconds = 10;
		// Observed native MIDI edits in sc55-battery-matched-fields.log: program
		// C0 40 reached SRAM[185], and CC7=20 reached SRAM[192]. The freshly
		// booted battery candidate retained both values.
		constexpr size_t g_programPartOne = 185;
		constexpr size_t g_volumePartOne = 192;
		// SC-55 1.21 note/voice-list writer 00:1C9C stores a voice index
		// into A318+R1. The original GP has 24 voices (sc55Board.h), and
		// the exact active-note VST3 SRAM image wrote A32F (R1=0x17) from
		// that path. Retaining this allocation link changed the next note
		// after a silent fresh boot; replacing only that byte from the quiet
		// image made 47,784 differing frames identical. These 24 links are
		// runtime voice membership, not the part program or panel volume.
		constexpr size_t g_voiceLinks = 0x2318;
		constexpr size_t g_voiceLinkCount = 24;
		// SC-55 1.21 ROM: TRAPA #0 scheduler sleeps at 00:04CF and
		// branches back at 00:04D0 only when its ready-task scan is empty.
		constexpr uint32_t g_schedulerSleepPc = 0x04cf;
		constexpr uint32_t g_schedulerLoopPc = 0x04d0;
		// SC-55 1.21 ROM: SCI ISR 00:05E8..061D writes the A3F8 ring;
		// 00:1EE9..1F1F consumes it via ABF6/ABF8. The parser at
		// 00:1F67..1FC7 also consumes the CB7E ring via CB7A/CB7C.
		// Offsets are from the board's SRAM base at 00:8000.
		constexpr size_t g_directRead = 0x2bf6;
		constexpr size_t g_directWrite = 0x2bf8;
		constexpr size_t g_alternateRead = 0x4b7a;
		constexpr size_t g_alternateWrite = 0x4b7c;
		// SC-55 1.21 ROM 04:29C9..2A24 samples three panel matrix
		// columns into CCA4..CCA6 and compares the previous CCA1..CCA3.
		// With every host button released, both banks must have observed FF.
		constexpr size_t g_panelPrevious = 0x4ca1;
		constexpr size_t g_panelCurrent = 0x4ca4;
		constexpr size_t g_panelColumns = 3;
		// 04:29DD masks the F0FE/F0FD/F0FB scanner address table with CC98.
		constexpr size_t g_panelScanMask = 0x4c98;
		constexpr uint8_t g_releasedMatrix = 0xff;
		// The same scanner enqueues edges via 04:2A27..2A4B into
		// CCA7..CCC7, with CCCA read and CCCC write pointers. Its 04:2928
		// dispatcher holds an active edge code at CCC8 until dispatched.
		constexpr size_t g_panelQueueRead = 0x4cca;
		constexpr size_t g_panelQueueWrite = 0x4ccc;
		constexpr size_t g_panelActiveEdge = 0x4cc8;
		uint16_t readWord(const std::vector<uint8_t>& bytes, const size_t offset)
		{
			return uint16_t((uint16_t(bytes[offset]) << 8) | bytes[offset + 1]);
		}
	}

	bool Sc55Settings::isPanelInputBoundary(Sc55Board& board)
	{
		if(!supported(board) || board.m_machine.cpu().in_slice())
			return false;
		const auto pc = board.m_machine.cpu().code_addr(board.m_machine.cpu().regs().pc);
		if(pc != g_schedulerSleepPc && pc != g_schedulerLoopPc) return false;
		if(board.m_lcdIrqAt != 0 || board.m_gaIntTrigger != 0) return false;
		const auto& ram = board.m_sram;
		if(readWord(ram, g_panelQueueRead) != readWord(ram, g_panelQueueWrite) ||
		   ram[g_panelActiveEdge] != g_releasedMatrix) return false;
		for(size_t scan = 0; scan < g_panelColumns; ++scan)
		{
			const auto select = uint8_t(~(uint8_t{1} << scan)) & ram[g_panelScanMask];
			uint8_t expected = g_releasedMatrix;
			for(size_t column = 0; column < sizeof(board.m_buttons); ++column)
				if((select & (uint8_t{1} << column)) == 0)
					expected &= uint8_t(~(board.m_buttons >>
						(column * std::numeric_limits<uint8_t>::digits)));
			if(ram[g_panelPrevious + scan] != expected ||
			   ram[g_panelCurrent + scan] != expected) return false;
		}
		return true;
	}

	bool Sc55Settings::isCaptureBoundary(Sc55Board& board)
	{
		if(board.m_buttons != 0 || !isPanelInputBoundary(board)) return false;
		if(board.m_machine.sci(0).rx_pending() != 0 ||
		   (board.m_machine.sci(0).ssr(board.m_machine.now()) & h8500::Sci::kRdrf))
			return false;
		const auto& ram = board.m_sram;
		return readWord(ram, g_directRead) == readWord(ram, g_directWrite) &&
		       readWord(ram, g_alternateRead) == readWord(ram, g_alternateWrite);
	}

	bool Sc55Settings::supported(const Sc55Board& board)
	{
		return board.m_valid && board.m_roms.model == DeviceModel::Sc55Mk1 &&
		       board.m_internalRomHash == g_internal121 &&
		       board.m_programRomHash == g_program121;
	}

	Sc55Settings::Result Sc55Settings::capture(Sc55Board& board, std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(!isCaptureBoundary(board)) return Result::PendingInput;
		image = board.m_sram;
		return Result::Success;
	}

	Sc55Settings::Result Sc55Settings::restore(Sc55Board& board, const std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(image.size() != Sc55Board::SramSize) return Result::InvalidImage;
		if(board.m_samplesRendered != 0) return Result::RequiresFreshBoard;
		board.m_sram = image;
		std::fill_n(board.m_sram.begin() + g_voiceLinks, g_voiceLinkCount, uint8_t{});
		board.powerCycle();
		for(uint32_t sample = 0; sample < g_bootSeconds * board.sampleRate(); ++sample)
			board.renderSample();
		if(board.m_sram[g_programPartOne] != image[g_programPartOne] ||
		   board.m_sram[g_volumePartOne] != image[g_volumePartOne])
			return Result::InvalidImage;
		return Result::Success;
	}

	bool Sc55Settings::probeMk2Image(const Sc55Board& board, std::vector<uint8_t>& image)
	{
		if(!board.m_valid || board.m_roms.model != DeviceModel::Sc55Mk2 ||
		   board.m_internalRomHash != g_mk2Internal ||
		   board.m_programRomHash != g_mk2Program) return false;
		image = board.m_sram;
		return true;
	}

	bool Sc55Settings::reopenMk2Probe(Sc55Board& board, const std::vector<uint8_t>& image)
	{
		std::vector<uint8_t> check;
		if(!probeMk2Image(board, check) || image.size() != Sc55Board::SramSize ||
		   board.m_samplesRendered != 0) return false;
		board.m_sram = image;
		board.powerCycle();
		for(uint32_t sample = 0; sample < g_bootSeconds * board.sampleRate(); ++sample)
			board.renderSample();
		return true;
	}

	bool Sc55Settings::probeMk2InputDrained(const Sc55Board& board)
	{
		return board.m_valid && board.m_roms.model == DeviceModel::Sc55Mk2 &&
		       board.m_internalRomHash == g_mk2Internal &&
		       board.m_programRomHash == g_mk2Program &&
		       board.m_subMcu.hostInputDrained();
	}
}
