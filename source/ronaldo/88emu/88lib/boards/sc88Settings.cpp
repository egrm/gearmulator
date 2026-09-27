#include "88lib/boards/sc88Settings.h"
#include "88lib/boards/sc88.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace emu88Lib
{
	namespace
	{
		// CPU-order control ROM fingerprints from romRegistry.h and the
		// independently audited SC-88/VL real-ROM boot diagnostic.
		constexpr baseLib::MD5 g_sc88Firmware("0ac771782ea58a53af590ebdf140d517");
		constexpr baseLib::MD5 g_sc88vlFirmware("25e016e93c8a44ba3c35584462b56d72");
		// SC-88 01:0B0C and VL 01:0C45 copy C072 to the C092 boot gate.
		// Their native C072 writers accept only 0 or 1. Preserve the saved value
		// after the private candidate has booted through the preservation branch.
		constexpr size_t g_batteryPreference = 0xc072;
		constexpr size_t g_bootGate = 0xc092;
		constexpr uint8_t g_preserveSettings = 1;
		// SC-88 01:BBCC..BC33 and VL 01:AF94..AFFB write the 32
		// panel-activity countdowns; 01:A6EE..A706 and 01:9A00..9A18
		// decrement them. They are not saved sound parameters.
		constexpr size_t g_meterCount = 32;
		constexpr size_t g_sc88MeterStart = 0x5444;
		constexpr size_t g_sc88vlMeterStart = 0x5448;
		// SC-88 00:2903/290B and VL 00:2ABB/2AC3 read/update this
		// synthesis overflow accumulator, then write a per-voice value.
		constexpr size_t g_sc88Accumulator = 0xf5ea;
		constexpr size_t g_sc88vlAccumulator = 0xf5fe;
		constexpr size_t g_accumulatorBytes = sizeof(uint16_t);
		// Voice-start handlers SC 00:220C/2210 and VL 00:23BC/23C0 write
		// owner and note to adjacent bytes. Their 01:913B/837F meter loops
		// scan 64 even owner offsets; neither byte is a saved part setting.
		constexpr size_t g_sc88VoiceOwnerStart = 0xeade;
		constexpr size_t g_sc88vlVoiceOwnerStart = 0xeaf2;
		constexpr size_t g_voiceOwnerCount = 64;
		constexpr size_t g_voiceOwnerStride = sizeof(uint16_t);
		// Existing SC-88/VL real-ROM diagnostic boots for this interval. This
		// foundation needs a model-specific readiness predicate before host use.
		constexpr unsigned g_bootSamples = g_sampleRate * 10;
		constexpr size_t g_parts = 32; // Both ROM boot loops address two groups of 16.
		// SC 00:30C4/3585 and VL 00:3195/3663 write/reset 128 per-key
		// pressure bytes per part. These receive values are separate from voices.
		constexpr size_t g_polyPressureBase = 0xc660;
		constexpr size_t g_polyPressureKeys = 128;
		// Native event consumers SC 1EB0/1EF0/1F58 and VL 2060/20A0/2108
		// retain portamento, hold and sostenuto in bits 5, 7 and 6. The
		// separate active-key membership arrays are not restored.
		constexpr size_t g_sc88SwitchBase = 0xe59e;
		constexpr size_t g_sc88vlSwitchBase = 0xe5b2;
		constexpr uint8_t g_switchMask = 0xe0;
		// PartR writers SC 01:C81F and VL 01:BF6F use a 0..31 UI index;
		// native initialization resets it (SC 01:D38A, VL 07:C605).
		constexpr size_t g_sc88SelectedIndex = 0x56f2;
		constexpr size_t g_sc88vlSelectedIndex = 0x56fc;
		// UserInst handlers SC 01:C784 / VL 01:BED3 request page3;
		// display workers 01:A750/01:9A9D publish it into these current-page bytes.
		constexpr size_t g_sc88PanelPage = 0x54d2;
		constexpr size_t g_sc88vlPanelPage = 0x54da;
		constexpr uint8_t g_userInstrumentPage = 3;
		// UserInst entry/exit writes requested pages 3/0 at SC 01:C784/C792
		// and VL 01:BED3/BEE1; workers 01:A750/01:9A9D publish them.
		constexpr uint8_t g_normalPage = 0;
		constexpr size_t g_sc88RequestedPage = 0x54d3;
		constexpr size_t g_sc88vlRequestedPage = 0x54db;
		// The source-verified dispatcher idle loops are SC 00:06B7/06BB/06BE
		// and VL 00:0733/0737/073A (family idle disassemblies).
		constexpr std::array<uint32_t,3> g_sc88IdlePc{0x06b7,0x06bb,0x06be};
		constexpr std::array<uint32_t,3> g_sc88vlIdlePc{0x0733,0x0737,0x073a};
		// Both loops compare the ready-queue pointer with 75EE. Their kernel
		// nesting word is accessed at 75AE (same archived disassemblies).
		constexpr size_t g_readyQueue = 0x75f8;
		constexpr uint16_t g_emptyReadyQueue = 0x75ee;
		constexpr size_t g_kernelNesting = 0x75ae;
		// SC 01:A37C/A3BF and VL 01:9696/07:BCA4 scan and queue keys here.
		// The scanner/UI waits are SC 01:A211/C065 and VL 01:95CE/B45C.
		constexpr size_t g_keyRead = 0x5000, g_keyWrite = 0x5002;
		constexpr size_t g_previousMatrix = 0x5004, g_scannedMatrix = 0x5008;
		constexpr size_t g_deferredKeys = 0x5030;
		constexpr size_t g_scannerTask = 0x75c0, g_uiTask = 0x75d0;
		constexpr uint8_t g_scannerSleeping = 0x81, g_uiSleeping = 0x89;
		constexpr uint8_t g_scannerWorkMask = 1, g_uiWorkMask = 9;
		constexpr uint8_t g_deferredMask = 1;
		// Native producer/consumer disassembly: SC 00:0C4E/0B5D and
		// VL 00:0CE8/0BF6 (88emu-sc88-vl-midi-boundary-ring.md).
		constexpr size_t g_sc88MidiRead = 0x05ea, g_sc88MidiWrite = 0x05ec;
		constexpr size_t g_sc88vlMidiRead = 0x0666, g_sc88vlMidiWrite = 0x0668;
		// The first consumer forwards to a second ring, not to the parameter
		// handler: SC 00:08C2/092C; VL 00:0942/09AD. Each record is two words.
		constexpr size_t g_sc88WorkWrite = 0x0226, g_sc88WorkRead = 0x0228;
		constexpr size_t g_sc88WorkBegin = 0x022a, g_sc88WorkFree = 0x03ba;
		constexpr size_t g_sc88vlWorkWrite = 0x02a0, g_sc88vlWorkRead = 0x02a2;
		constexpr size_t g_sc88vlWorkBegin = 0x02a4, g_sc88vlWorkFree = 0x0434;
		constexpr size_t g_midiRecordBytes = sizeof(uint16_t) + sizeof(uint16_t);
		// Parameter-message feeder producer/consumer SC 00:0A12/09ED and
		// VL 00:0A94/0A6F forward into the work ring (boundary-ring report).
		constexpr size_t g_sc88FeederRead = 0x03be, g_sc88FeederWrite = 0x03c0;
		constexpr size_t g_sc88vlFeederRead = 0x043a, g_sc88vlFeederWrite = 0x043c;
		// UI event queue initializers SC 01:9749 and VL 01:89F9; consumers
		// SC 01:96F0 / VL 01:89A0 are called separately from the key ring.
		constexpr size_t g_sc88UiRead = 0x41ea, g_sc88UiWrite = 0x41ec;
		constexpr size_t g_sc88vlUiRead = 0x4266, g_sc88vlUiWrite = 0x4268;
		// ROM-selected UserInst titles from SC 07:B2BC/B2EC/B30C and
		// VL 07:D1C0/D1F0/D210. The visible LCD can lag the sleeping UI task.
		constexpr std::array<std::string_view,3> g_userTitles{"Vib.","Fil.","Env."};
		constexpr size_t g_titleColumn = 3, g_titleWidth = 4;
		constexpr size_t g_filterDashColumn = 8, g_filterDashWidth = 3;
		constexpr std::string_view g_filterDashes = "---";
		// Native Select handlers SC 01:CF0F..CF1F / VL 01:C6AC..C6BC
		// cycle Vibrato, Filter and Envelope through subgroup values 1..3.
		constexpr size_t g_sc88UserGroup = 0x54d7;
		constexpr size_t g_sc88vlUserGroup = 0x54df;
		constexpr uint8_t g_firstUserGroup = 1;
		constexpr uint8_t g_filterUserGroup = 2; // ROM subgroup 2 is Filter.
		constexpr uint8_t g_lastUserGroup = 3;
		// Match the native panel gesture timing used by the Pro adapter and
		// Sc88::runFactoryReset, allowing the scanner to consume each edge.
		constexpr unsigned g_panelPressSamples = g_sampleRate / 10;
		constexpr unsigned g_panelReleaseSamples = g_sampleRate / 4;
		constexpr size_t g_partStride = sizeof(uint16_t); // R3 is doubled part index.
		// Native part-record bases/stride from SC 01:0A64 and VL 01:0B30.
		constexpr std::array<size_t,2> g_partRecordBases{0x8088,0x9588};
		constexpr size_t g_partsPerGroup = g_parts / g_partRecordBases.size();
		constexpr size_t g_partRecordStride = 0x70;
		// Secondary record pointers: SC 01:0A64..0A8E, VL 01:0B30..0B5A.
		// User-tone loaders SC 00:2E02 and VL 01:2B68 replace eight edit
		// bytes at +6 even after the preserved-settings boot path is chosen.
		constexpr std::array<size_t,2> g_secondaryBases{0x87c8,0x9cc8};
		constexpr size_t g_secondaryStride = 0x20;
		constexpr size_t g_userToneOffset = 6;
		constexpr size_t g_userToneBytes = 8;
		// RPN fine-tuning MSB/LSB mirrors written by SC 3493/326F and
		// VL 3571/3340. Boot resets them independently of retained settings.
		constexpr std::array<size_t,2> g_fineTuneMirrors{0x2b,0x37};
		constexpr auto g_byteBits = std::numeric_limits<uint8_t>::digits;
		// SC-88 handlers 30EC..326E and VL 31BD..333F. Controller values are
		// per-part receiver state; part-record volume/pan/send settings are already
		// retained by the firmware's C092 branch and are deliberately excluded.
		constexpr std::array g_receiverBytes{
			0xd6a0, 0xd660, 0xd661, 0xd6a1, 0xd6e0, 0xd6e1,
			0xd720, 0xd721, 0xd760, 0xd761, 0xd820, 0xd860,
			0xd861, 0xd8a0, 0xd8e0, 0xd8e1, 0xd920, 0xd921,
			// RPN fine word and coarse byte: SC 3493/34B3, VL 3571/3591.
			0xd7a0, 0xd7a1, 0xd7e0};
		// The six MIDI writers OR their recalculation source masks into these
		// dirty targets. Both ROMs independently use the same pairs:
		// SC-88 30C4/30EC/3108/318B/313B/3150 and
		// VL 3195/31BD/31D9/325C/320C/3221.
		constexpr std::array g_dirtyMasks{
			std::pair{0xc420, 0xc5a0}, std::pair{0xc3e0, 0xc560},
			std::pair{0xc3a0, 0xc520}, std::pair{0xc360, 0xc4e0},
			std::pair{0xc460, 0xc5e0}, std::pair{0xc4a0, 0xc620}};

		uint16_t readWord(const std::vector<uint8_t>& bytes, size_t address)
		{
			return static_cast<uint16_t>((uint16_t{bytes[address]} << g_byteBits) |
			                             bytes[address + sizeof(uint8_t)]);
		}
		void pressPanel(Sc88& board, uint32_t buttons)
		{
			// Reuse the native scanner so descriptor and display ownership stays local.
			board.setButtons(buttons);
			for(unsigned sample{};sample<g_panelPressSamples;++sample) board.renderSample();
			board.setButtons(uint32_t{});
			for(unsigned sample{};sample<g_panelReleaseSamples;++sample) board.renderSample();
		}

	}
	bool Sc88Settings::supported(const Sc88& board)
	{
		return board.isValid() &&
		       ((board.m_model == Model::Sc88 && board.m_firmwareHash == g_sc88Firmware) ||
		        (board.m_model == Model::Sc88VL && board.m_firmwareHash == g_sc88vlFirmware));
	}
	bool Sc88Settings::isPanelInputBoundary(const Sc88& board)
	{
		if(!supported(board) || board.m_machine.cpu().in_slice()) return false;
		const auto& ram = board.m_sram;
		const auto sc = board.m_model == Model::Sc88;
		const auto& idlePc = sc ? g_sc88IdlePc : g_sc88vlIdlePc;
		const auto pc = board.m_machine.cpu().code_addr(board.m_machine.cpu().regs().pc);
		if(std::find(idlePc.begin(),idlePc.end(),pc) == idlePc.end() ||
		   readWord(ram,g_readyQueue) != g_emptyReadyQueue ||
		   readWord(ram,g_kernelNesting) != uint16_t{} ||
		   readWord(ram,g_keyRead) != readWord(ram,g_keyWrite)) return false;
		if(ram[g_scannerTask] != g_scannerSleeping ||
		   (ram[g_scannerTask+sizeof(uint8_t)] & g_scannerWorkMask) ||
		   ram[g_uiTask] != g_uiSleeping ||
		   (ram[g_uiTask+sizeof(uint8_t)] & g_uiWorkMask)) return false;
		for(size_t column{};column<sizeof(board.m_buttons);++column)
		{
			const auto observed = static_cast<uint8_t>(
				~(board.m_buttons >> (column * g_byteBits)));
			if(ram[g_previousMatrix+column] != observed ||
			   ram[g_scannedMatrix+column] != observed) return false;
		}
		return true;
	}
	bool Sc88Settings::isCaptureBoundary(const Sc88& board, const bool requireReleasedPanel)
	{
		// Only the released-panel restore path has bounded native tests.
		if(!requireReleasedPanel || board.m_buttons != uint32_t{} ||
		   !isPanelInputBoundary(board) || !board.m_midiInQueue.empty() ||
		   board.m_midiMailboxFull || board.m_midiWireDelay != uint32_t{} ||
		   board.m_gaLcdEvent != decltype(board.m_gaLcdEvent){}) return false;
		const auto& ram = board.m_sram;
		const auto sc = board.m_model == Model::Sc88;
		const auto page = ram[sc ? g_sc88PanelPage : g_sc88vlPanelPage];
		if(readWord(ram,sc ? g_sc88MidiRead : g_sc88vlMidiRead) !=
		   readWord(ram,sc ? g_sc88MidiWrite : g_sc88vlMidiWrite) ||
		   readWord(ram,sc ? g_sc88WorkRead : g_sc88vlWorkRead) !=
		   readWord(ram,sc ? g_sc88WorkWrite : g_sc88vlWorkWrite) ||
		   readWord(ram,sc ? g_sc88FeederRead : g_sc88vlFeederRead) !=
		   readWord(ram,sc ? g_sc88FeederWrite : g_sc88vlFeederWrite) ||
		   readWord(ram,sc ? g_sc88UiRead : g_sc88vlUiRead) !=
		   readWord(ram,sc ? g_sc88UiWrite : g_sc88vlUiWrite)) return false;
		const auto workFree = sc ? ram[g_sc88WorkFree] : readWord(ram,g_sc88vlWorkFree);
		const auto capacity = sc ? (g_sc88WorkFree-g_sc88WorkBegin)/g_midiRecordBytes
		                         : (g_sc88vlWorkFree-g_sc88vlWorkBegin)/g_midiRecordBytes;
		if(workFree != capacity) return false;
		if(page != g_normalPage && page != g_userInstrumentPage) return false;
		if(page != ram[sc ? g_sc88RequestedPage : g_sc88vlRequestedPage]) return false;
		for(size_t key{};key<std::numeric_limits<decltype(board.m_buttons)>::digits;++key)
			if(ram[g_deferredKeys+key] & g_deferredMask) return false;
		if(page == g_userInstrumentPage)
		{
			const auto group = ram[sc ? g_sc88UserGroup : g_sc88vlUserGroup];
			if(group < g_firstUserGroup || group > g_lastUserGroup) return false;
			const auto& lcd = board.m_lcd;
			std::string visible(lcd.getVisibleColumns()*lcd.getVisibleLines(),' ');
			lcd.copyVisibleDdRam(visible.data());
			if(visible.substr(g_titleColumn,g_titleWidth) != g_userTitles[group-g_firstUserGroup])
				return false;
			if(group == g_filterUserGroup &&
			   visible.substr(g_filterDashColumn,g_filterDashWidth) != g_filterDashes)
				return false;
		}
		return true;
	}

	Sc88Settings::Result Sc88Settings::capture(const Sc88& board, std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		image = board.m_sram;
		return Result::Success;
	}

	Sc88Settings::Result Sc88Settings::restore(Sc88& board, const std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(image.size() != Sc88::SramSize || image[g_batteryPreference] > g_preserveSettings)
			return Result::InvalidImage;
		if(board.m_samplesRendered != 0) return Result::RequiresFreshBoard;
		const auto meterStart = board.m_model == Model::Sc88 ? g_sc88MeterStart : g_sc88vlMeterStart;
		const auto accumulator = board.m_model == Model::Sc88 ? g_sc88Accumulator : g_sc88vlAccumulator;
		const auto voiceOwner = board.m_model == Model::Sc88 ? g_sc88VoiceOwnerStart : g_sc88vlVoiceOwnerStart;
		const auto switches = board.m_model == Model::Sc88 ? g_sc88SwitchBase : g_sc88vlSwitchBase;
		const auto selectedIndex = board.m_model == Model::Sc88 ? g_sc88SelectedIndex : g_sc88vlSelectedIndex;
		const auto panelPage = board.m_model == Model::Sc88 ? g_sc88PanelPage : g_sc88vlPanelPage;
		const auto requestedPage = board.m_model == Model::Sc88 ? g_sc88RequestedPage : g_sc88vlRequestedPage;
		const auto userGroup = board.m_model == Model::Sc88 ? g_sc88UserGroup : g_sc88vlUserGroup;
		const auto savedSelection = readWord(image,selectedIndex);
		if(savedSelection >= g_parts) return Result::InvalidImage;
		// Only normal and UserInst pages have source-backed fresh-board panel
		// reconstruction. A pending page transition is not a settled image.
		if((image[panelPage] != g_normalPage && image[panelPage] != g_userInstrumentPage) ||
		   image[requestedPage] != image[panelPage]) return Result::InvalidImage;
		if(image[panelPage]==g_userInstrumentPage &&
		   (image[userGroup]<g_firstUserGroup || image[userGroup]>g_lastUserGroup))
			return Result::InvalidImage;
		std::array<uint8_t,g_meterCount> freshMeters{};
		std::array<uint8_t,g_accumulatorBytes> freshAccumulator{};
		std::array<uint8_t,g_voiceOwnerCount> freshVoiceOwners{};
		std::copy_n(board.m_sram.begin()+meterStart,g_meterCount,freshMeters.begin());
		std::copy_n(board.m_sram.begin()+accumulator,g_accumulatorBytes,freshAccumulator.begin());
		for(size_t voice{};voice<g_voiceOwnerCount;++voice)
			freshVoiceOwners[voice] = board.m_sram[voiceOwner+voice*g_voiceOwnerStride];
		board.m_sram = image;
		std::copy(freshMeters.begin(),freshMeters.end(),board.m_sram.begin()+meterStart);
		std::copy(freshAccumulator.begin(),freshAccumulator.end(),board.m_sram.begin()+accumulator);
		for(size_t voice{};voice<g_voiceOwnerCount;++voice)
			board.m_sram[voiceOwner+voice*g_voiceOwnerStride] = freshVoiceOwners[voice];
		board.m_sram[g_batteryPreference] = g_preserveSettings;
		for(unsigned sample{}; sample < g_bootSamples; ++sample) board.renderSample();
		if(board.m_sram[g_bootGate] != g_preserveSettings) return Result::InvalidImage;
		board.m_sram[g_batteryPreference] = image[g_batteryPreference];
		// Preserve unsaved UserInst edits before firmware reconstructs the UI
		// descriptors; boot may reload the stored user tone over its edit buffer.
		for(size_t part{};part<g_parts;++part)
		{
			const auto address=g_secondaryBases[part/g_partsPerGroup]+
			                   (part%g_partsPerGroup)*g_secondaryStride+g_userToneOffset;
			std::copy_n(image.begin()+address,g_userToneBytes,board.m_sram.begin()+address);
		}
		// Let firmware rebuild the selected part's descriptors and LCD rather
		// than transplanting UI pointers into a fresh machine.
		for(size_t move{};move<g_parts && readWord(board.m_sram,selectedIndex)!=savedSelection;++move)
			pressPanel(board,buttonBit(Button::PartR));
		if(readWord(board.m_sram,selectedIndex)!=savedSelection) return Result::InvalidImage;
		if(image[panelPage]==g_userInstrumentPage && board.m_sram[panelPage]!=g_userInstrumentPage)
		{
			pressPanel(board,buttonBit(Button::UserInst));
			if(board.m_sram[panelPage]!=g_userInstrumentPage) return Result::InvalidImage;
		}
		if(image[panelPage]==g_userInstrumentPage)
		{
			for(unsigned move{};move<g_lastUserGroup && board.m_sram[userGroup]!=image[userGroup];++move)
				pressPanel(board,buttonBit(Button::Select));
			if(board.m_sram[userGroup]!=image[userGroup]) return Result::InvalidImage;
		}
		std::copy_n(image.begin()+g_polyPressureBase,g_parts*g_polyPressureKeys,
		            board.m_sram.begin()+g_polyPressureBase);
		// The boot routines still reset receive controllers with C092 set.
		// Restore only source-verified per-part receiver bytes, then reproduce
		// the native handlers' dirty-mask ORs so dependent DSP state recomputes.
		// Voice-event queues and voice/key ownership history remain fresh;
		// the source-verified poly-pressure receive array is restored above.
		for(size_t part{}; part < g_parts; ++part)
		{
			const auto stride = part * g_partStride;
			board.m_sram[switches+stride] = static_cast<uint8_t>(
				(board.m_sram[switches+stride] & ~g_switchMask) |
				(image[switches+stride] & g_switchMask));
			for(const auto address : g_receiverBytes)
				board.m_sram[address + stride] = image[address + stride];
			const auto partBase = g_partRecordBases[part/g_partsPerGroup] +
			                      (part%g_partsPerGroup)*g_partRecordStride;
			for(const auto offset : g_fineTuneMirrors)
				board.m_sram[partBase+offset] = image[partBase+offset];
			for(const auto& [source, target] : g_dirtyMasks)
			{
				const auto value = readWord(board.m_sram, source + stride) |
				                   readWord(board.m_sram, target + stride);
				board.m_sram[target + stride] = static_cast<uint8_t>(value >> g_byteBits);
				board.m_sram[target + stride + sizeof(uint8_t)] = static_cast<uint8_t>(value);
			}
		}
		return Result::Success;
	}
}
