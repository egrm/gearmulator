#include "88lib/boards/laBoard.h"
#include "88lib/boards/laSettings.h"
#include "88lib/rom/romloader.h"
#include "baseLib/md5.h"
#include "common/test_util.hpp"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>

namespace
{
	using emu88Lib::LaModel;
	using emu88Lib::LaRomSet;

	// A control ROM the board will boot: the 8095 starts at 2080h, which lands in the bus ROM
	// window rather than the bank window, so a test program is written straight into the image.
	struct Program
	{
		std::vector<uint8_t> bytes = std::vector<uint8_t>(LaRomSet::controlSize(LaModel::Cm32l), 0xff);
		size_t cursor = 0x2080;
		void emit(std::initializer_list<uint8_t> code)
		{
			std::copy(code.begin(), code.end(), bytes.begin() + cursor);
			cursor += code.size();
		}
		void write(uint16_t address, uint8_t value)
		{
			// LDB 20h,#value ; STB 20h,[address]
			emit({0xb1, value, 0x20, 0xc7, 1, uint8_t(address), uint8_t(address >> 8), 0x20});
		}
		void readTo(uint8_t reg, uint16_t address)
		{
			// LDB reg,[address]
			emit({0xb3, 1, uint8_t(address), uint8_t(address >> 8), reg});
		}
		void halt() { emit({0x27, 0xfe}); }	// SJMP $
		void finish() { halt(); CHECK(cursor <= 0x3000); }
	};

	// The same program on any of the boards: the control image padded or cut to the board's
	// size (the test code sits below 3000h, inside every one of them).
	LaRomSet romSet(const Program& program, const LaModel model = LaModel::Cm32l)
	{
		LaRomSet roms;
		roms.model = model;
		roms.control = program.bytes;
		roms.control.resize(LaRomSet::controlSize(model), 0xff);
		roms.wave.assign(LaRomSet::waveSize(model), 0x80);
		// A reverb microcode image that writes nothing and accumulates nothing: every step
		// reads RAM (control bit 1) with the shifter held (bit 2), so both outputs stay zero.
		roms.reverb.assign(LaRomSet::ReverbSize, 0x06);
		return roms;
	}

	void run(emu88Lib::LaBoard& board, const unsigned samples = 200)
	{
		for(unsigned i = 0; i < samples; ++i)
			board.renderSample();
	}
}

int main(int argc, char** argv)
{
	using emu88Lib::LaBoard;

	// Nothing runs without a complete set, and an incomplete board is silent rather than unsafe.
	CHECK(!LaBoard(LaRomSet{}).isValid());
	CHECK(LaBoard(LaRomSet{}).renderSample() == LaBoard::SampleFrame{});
	{
		Program p;
		p.finish();
		auto roms = romSet(p);
		roms.reverb.pop_back();
		CHECK(!LaBoard(roms).isValid());
	}
	// Each board takes its own image sizes and no other's: the MT-32s have half the CM-32L's
	// PCM, and the new-type MT-32 board pages twice the control ROM.
	{
		Program p;
		p.finish();
		CHECK(LaBoard(romSet(p, LaModel::Mt32Old)).isValid());
		CHECK(LaBoard(romSet(p, LaModel::Mt32New)).isValid());
		CHECK(LaBoard(romSet(p, LaModel::Cm32ln)).isValid());
		auto roms = romSet(p, LaModel::Mt32Old);
		roms.wave = romSet(p, LaModel::Cm32l).wave;
		CHECK(!LaBoard(roms).isValid());
		roms = romSet(p, LaModel::Mt32New);
		roms.control = romSet(p, LaModel::Mt32Old).control;
		CHECK(!LaBoard(roms).isValid());
		// The 80C198 of the CM-32LN runs two clocks per state, the NMOS parts three.
		CHECK_EQ(LaBoard(romSet(p, LaModel::Cm32ln)).cpuStateRate(), LaBoard::CpuClock / 2);
		CHECK_EQ(LaBoard(romSet(p, LaModel::Mt32Old)).cpuStateRate(), LaBoard::CpuClock / 3);
		CHECK_EQ(LaBoard(romSet(p)).cpuStateRate(), LaBoard::CpuClock / 3);
	}

	// The 0200h latch: bit 0 is the MIDI MESSAGE lamp, and the whole byte is mirrored across
	// the 0200-027F window the switch matrix reads from.
	{
		Program p;
		p.write(0x0200, 0x01);
		p.finish();
		LaBoard board(romSet(p));
		CHECK(board.isValid());
		run(board);
		CHECK_EQ(board.leds(), 1);
		board.reset();
		CHECK_EQ(board.leds(), 0);
	}

	// The switch matrix. Group 0 answers at 021Ah and group 1 at 021Ch, both active low, so a
	// pressed button reads as a cleared bit. Buttons 0 and 9 land in different groups.
	{
		Program p;
		p.readTo(0x30, 0x021a);
		p.readTo(0x32, 0x021c);
		p.finish();
		LaBoard board(romSet(p));
		board.setButtons(emu88Lib::mt32ButtonBit(emu88Lib::Mt32Button::Part1) |
		                 emu88Lib::mt32ButtonBit(emu88Lib::Mt32Button::Part5));
		run(board);
		CHECK_EQ(board.registerByte(0x30), 0xfe);
		CHECK_EQ(board.registerByte(0x32), 0xfd);
		// Reset releases everything the panel was holding.
		board.reset();
		run(board);
		CHECK_EQ(board.registerByte(0x30), 0xff);
		CHECK_EQ(board.registerByte(0x32), 0xff);
	}

	// The bank window at 8000h: banks 0-3 page the control ROM linearly, 10h and 11h the two
	// halves of the 32 KiB of work RAM. Writing through the window must be readable back.
	{
		Program p;
		p.bytes[0x4000 + 0x0123] = 0x5a;	// bank 1, offset 0123h
		p.write(0x0100, 0x01);
		p.readTo(0x30, 0x8123);
		p.write(0x0100, 0x11);
		p.write(0x8005, 0xa5);
		p.readTo(0x32, 0x8005);
		p.write(0x0100, 0x10);
		p.readTo(0x34, 0x8005);	// bank 10h is the RAM at C000h, not bank 11h
		p.readTo(0x36, 0x8fff);	// an unwritten RAM cell
		p.write(0x0100, 0x40);
		p.readTo(0x38, 0x8000);	// no bank there at all
		p.write(0x0100, 0x05);
		p.readTo(0x3a, 0x8123);	// bank 5: past a 64 KiB image, inside a 128 KiB one
		p.finish();
		LaBoard board(romSet(p));
		run(board);
		CHECK_EQ(board.registerByte(0x30), 0x5a);
		CHECK_EQ(board.registerByte(0x32), 0xa5);
		CHECK_EQ(board.registerByte(0x34), 0x00);
		CHECK_EQ(board.registerByte(0x36), 0x00);
		CHECK_EQ(board.registerByte(0x38), 0xff);
		CHECK_EQ(board.registerByte(0x3a), 0xff);
		// The new-type MT-32 board's 128 KiB image reaches four banks further.
		auto roms = romSet(p, LaModel::Mt32New);
		roms.control[0x14000 + 0x0123] = 0x3c;
		LaBoard wide(roms);
		run(wide);
		CHECK_EQ(wide.registerByte(0x30), 0x5a);
		CHECK_EQ(wide.registerByte(0x3a), 0x3c);
		// The old-type board's wider LA32 window ends at 0FFFh: the bank window is RAM there
		// too, not the chip. (A decode that let the chip swallow the window booted the 1.x
		// firmware into a display and no sound.)
		LaBoard old(romSet(p, LaModel::Mt32Old));
		run(old);
		CHECK_EQ(old.registerByte(0x30), 0x5a);
		CHECK_EQ(old.registerByte(0x32), 0xa5);
		CHECK_EQ(old.registerByte(0x34), 0x00);
	}

	// The display latch buffers data bytes at 0300h and strobes the burst through on the
	// control write at 0380h. 80h|n positions the cursor; the characters follow it.
	{
		Program p;
		p.write(0x0300, 'C');
		p.write(0x0300, 'M');
		p.write(0x0380, 0x80);	// cursor home, then flush "CM"
		p.write(0x0380, 0x0d);	// display on
		p.finish();
		LaBoard board(romSet(p));
		run(board);
		CHECK_EQ(board.lcd().getVisibleColumns(), 20u);
		CHECK(board.lcd().isDisplayOn());
		CHECK_EQ(board.lcd().getVisibleCharacter(0), 'C');
		CHECK_EQ(board.lcd().getVisibleCharacter(1), 'M');
		board.reset();
		CHECK(!board.lcd().isDisplayOn());
		CHECK_EQ(board.lcd().getVisibleCharacter(0), ' ');
	}

	// MIDI arrives on the 8095's serial port. Echo it back through a register so the test can
	// see the byte the firmware would have read, and check the port filter drops other ports.
	{
		Program p;
		p.emit({0xb1, 0x5a, 0x30});					// a sentinel the arriving byte replaces
		p.emit({0xb0, 0x11, 0x20, 0x36, 0x20, 0xfa});	// wait for RI in SP_STAT
		p.emit({0xb0, 0x07, 0x30});					// read SBUF
		p.finish();
		LaBoard board(romSet(p));
		const synthLib::SMidiEvent note(synthLib::MidiEventSource::Host, 0x90, 60, 100);
		board.addMidiEvent(note, 1);
		run(board);
		CHECK_EQ(board.registerByte(0x30), 0x5a);
		board.addMidiEvent(note);
		run(board);
		CHECK_EQ(board.registerByte(0x30), 0x90);
	}

	// P0.4 carries the LA32's SH3 line, which changes twice per output frame; a board that
	// left it stuck would hang one of these two waits and never set the second marker.
	{
		Program p;
		p.emit({0x34, 0x0e, 0xfd});			// JBC P0,4,-3: spin until P0.4 is high
		p.emit({0xb1, 0x01, 0x30});
		p.emit({0x3c, 0x0e, 0xfd});			// JBS P0,4,-3: spin until it is low again
		p.emit({0xb1, 0x01, 0x32});
		p.finish();
		LaBoard board(romSet(p));
		run(board, 400);
		CHECK_EQ(board.registerByte(0x30), 1);
		CHECK_EQ(board.registerByte(0x32), 1);
	}

	// The LA32's address decode by board generation. A read of the chip anywhere but its
	// control block answers the interrupt status byte, so a register read comes back with a
	// value rather than the bus's FFh; the old-type board answers over 0C00-0FFF with the CPU's
	// A0 left off, the new-type boards over 0C00-0DFF a byte at a time.
	{
		Program p;
		p.readTo(0x30, 0x0c00);	// the chip on either board
		p.readTo(0x32, 0x0e00);	// only the old board's wider window
		p.readTo(0x34, 0x0f00);	// chip register 180h there, a status read; the control block is at 0F80h+
		p.finish();
		LaBoard newBoard(romSet(p, LaModel::Mt32New));
		run(newBoard);
		CHECK(newBoard.registerByte(0x30) != 0xff);
		CHECK_EQ(newBoard.registerByte(0x32), 0xff);
		CHECK_EQ(newBoard.registerByte(0x34), 0xff);
		LaBoard oldBoard(romSet(p, LaModel::Mt32Old));
		run(oldBoard);
		CHECK(oldBoard.registerByte(0x30) != 0xff);
		CHECK(oldBoard.registerByte(0x32) != 0xff);
		CHECK(oldBoard.registerByte(0x34) != 0xff);
	}

	// The VOLUME/VALUE knob is a potentiometer on analog channel 7: it stands where it was
	// left, through a reset, and the CM-32L boards have the pin tied high. The firmware reads
	// it through the A/D converter; here the position is checked at the board.
	{
		Program p;
		p.finish();
		LaBoard board(romSet(p, LaModel::Mt32New));
		CHECK_EQ(board.knob(), LaBoard::KnobMaximum);
		board.setKnob(300);
		CHECK_EQ(board.knob(), 300);
		board.turnKnob(-1000);
		CHECK_EQ(board.knob(), 0);
		board.turnKnob(5000);
		CHECK_EQ(board.knob(), LaBoard::KnobMaximum);
		board.setKnob(512);
		board.reset();
		CHECK_EQ(board.knob(), 512);
	}

	// The audio wiring of the two board generations. The new-type boards rotate the LA32's
	// word up one bit onto the bus, bit 14 into bit 0: a small value doubles, one past
	// +/-16383 folds back, and the sign stays. The old-type board keeps the bus straight and
	// wires the DAC one bit up with bit 14 dropped instead.
	{
		CHECK_EQ(LaBoard::rotateNewBoardBus(0), 0);
		CHECK_EQ(LaBoard::rotateNewBoardBus(1), 2);
		CHECK_EQ(LaBoard::rotateNewBoardBus(1000), 2000);
		CHECK_EQ(LaBoard::rotateNewBoardBus(16383), 32766);
		CHECK_EQ(LaBoard::rotateNewBoardBus(16384), 1);			// folds: 2 * (16384 - 16384) + 1
		CHECK_EQ(LaBoard::rotateNewBoardBus(20000), 7233);			// 2 * (20000 - 16384) + 1
		CHECK_EQ(LaBoard::rotateNewBoardBus(-1), -1);
		CHECK_EQ(LaBoard::rotateNewBoardBus(-2), -3);				// 2x + 1 while bit 14 is set
		CHECK_EQ(LaBoard::rotateNewBoardBus(-16384), -32767);
		CHECK_EQ(LaBoard::rotateNewBoardBus(-16385), -2);			// folds: 2x + 32768
		CHECK_EQ(LaBoard::rotateNewBoardBus(-32768), -32768);
		CHECK_EQ(LaBoard::shiftOldBoardDac(0), 0);
		CHECK_EQ(LaBoard::shiftOldBoardDac(1000), 2000);
		CHECK_EQ(LaBoard::shiftOldBoardDac(16383), 32766);
		CHECK_EQ(LaBoard::shiftOldBoardDac(16384), 0);
		CHECK_EQ(LaBoard::shiftOldBoardDac(-1), -2);
		CHECK_EQ(LaBoard::shiftOldBoardDac(-16384), -32768);
		CHECK_EQ(LaBoard::shiftOldBoardDac(-16385), -2);
	}

	// A silent chip gives a silent DAC on both boards, and the six DAC words are what the
	// board's dacBuses() reports.
	{
		Program p;
		p.finish();
		for(const auto model : {LaModel::Mt32Old, LaModel::Mt32New, LaModel::Cm32l})
		{
			LaBoard board(romSet(p, model));
			run(board);
			for(const auto word : board.dacBuses())
				CHECK_EQ(word, 0);
			const auto silent = board.analogSample() == std::pair<float, float>{};
			CHECK(silent);
		}
	}

	// A private capture board must keep pending MIDI wire timing and CPU state while
	// retaining its own UART, chip IRQ, and scheduler callback destinations.
	{
		Program p;
		p.emit({0xb1, 0x5a, 0x30});					// sentinel until a serial byte arrives
		p.emit({0xb0, 0x11, 0x20, 0x36, 0x20, 0xfa});	// wait for RI in SP_STAT
		p.emit({0xb0, 0x07, 0x30});					// read SBUF
		p.finish();
		for(const auto model : {LaModel::Mt32Old, LaModel::Mt32New, LaModel::Cm32l})
		{
			auto source = std::make_unique<LaBoard>(romSet(p, model));
			source->setKnob(300);
			source->addMidiEvent(synthLib::SMidiEvent(synthLib::MidiEventSource::Host, 0x90, 60, 100));
			auto prepare = source->prepareExecutionClone();
			CHECK(static_cast<bool>(prepare));
			if(!prepare) continue;
			auto shell = prepare();
			CHECK(shell && shell->acceptsExecutionFrom(*source));
			if(!shell) continue;
			CHECK(shell->copyExecutionFrom(*source));
			CHECK(!shell->acceptsExecutionFrom(*source));
			auto clone = source->cloneExecution();
			CHECK(clone && clone->isValid());
			if(!clone) continue;
			CHECK_EQ(clone->knob(), source->knob());
			for(unsigned sample = 0; sample < 200; ++sample)
			{
				const auto sourceFrame = source->renderSample();
				CHECK(clone->renderSample() == sourceFrame);
				CHECK(shell->renderSample() == sourceFrame);
				CHECK_EQ(clone->registerByte(0x30), source->registerByte(0x30));
				CHECK_EQ(shell->registerByte(0x30), source->registerByte(0x30));
				CHECK(clone->dacBuses() == source->dacBuses());
				CHECK(shell->dacBuses() == source->dacBuses());
			}
			CHECK_EQ(clone->registerByte(0x30), 0x90);
			auto second = source->cloneExecution();
			CHECK(second && second->isValid());
			if(!second) continue;
			source.reset();
			for(unsigned sample = 0; sample < 200; ++sample)
			{
				const auto secondFrame = second->renderSample();
				CHECK(clone->renderSample() == secondFrame);
				CHECK(shell->renderSample() == secondFrame);
			}
		}
	}

	// Optional real-ROM probe: laBoard_test <mt32-old|mt32-new|cm32l> <rom-directory>.
	// Roland's MT-32 MIDI Implementation
	// §§3 and 5 specify RQ1/DT1, patch temporary 03 00 00 and system 10 00 00.
	// The 4-byte system slice covers tuning and reverb mode/time/level, with no
	// ignored dummy field. The first two patch bytes choose group and timbre.
	if(argc == 3)
	{
		const std::string_view name(argv[1]);
		const auto model = name == "mt32-old" ? LaModel::Mt32Old :
		                   name == "mt32-new" ? LaModel::Mt32New : LaModel::Cm32l;
		CHECK(name == "mt32-old" || name == "mt32-new" || name == "cm32l");
		synthLib::RomLoader::setSearchPath(argv[2]);
		emu88Lib::RomLoader::rescan();
		const auto roms = emu88Lib::RomLoader::findLaRomSet(model);
		if(roms.isValid()) std::printf("LA control MD5 %s\n", baseLib::MD5(roms.control).toString().c_str());
		CHECK(roms.isValid());
		if(roms.isValid())
		{
			constexpr uint32_t systemAddress = 0x10u << 14;
			constexpr uint32_t patchOneAddress = 0x03u << 14;
			constexpr uint32_t timbreOneAddress = 0x04u << 14;
			constexpr size_t systemSoundBytes = 4;
			constexpr size_t patchSelectionBytes = 2;
			// Owner's Manual §5-1: common 0x0e plus four 0x3a partials.
			constexpr size_t timbreSoundBytes = 0x0e + 4 * 0x3a;
			constexpr size_t bootSamples = LaBoard::SampleRate * 10;
			constexpr size_t replyBudgetSamples = LaBoard::SampleRate * 5;
			LaBoard source(roms);
			run(source, bootSamples);
			// Program change for part 1, whose documented power-on channel is 2.
			source.addMidiEvent(synthLib::SMidiEvent(synthLib::MidiEventSource::Host, 0xc1, 17));
			run(source, LaBoard::SampleRate);
			std::vector<uint8_t> patch;
			CHECK(source.requestParameterBlock(patchOneAddress, patchSelectionBytes,
			                                   replyBudgetSamples, patch));
			CHECK(patch.size() == patchSelectionBytes);
			std::vector<uint8_t> system;
			CHECK(source.requestParameterBlock(systemAddress, systemSoundBytes,
			                                   replyBudgetSamples, system));
			CHECK(system.size() == systemSoundBytes);
			std::vector<uint8_t> timbre;
			CHECK(source.requestParameterBlock(timbreOneAddress, timbreSoundBytes,
			                                   replyBudgetSamples, timbre));
			CHECK(timbre.size() == timbreSoundBytes);
			if(patch.size() == patchSelectionBytes && system.size() == systemSoundBytes &&
			   timbre.size() == timbreSoundBytes)
			{
				// One audible reverb level edit through the firmware's own DT1 handler.
				system.back() = system.back() == 0 ? 1 : 0;
				CHECK(source.writeAndVerifyParameterBlock(systemAddress, system,
				                                         replyBudgetSamples));
				LaBoard restored(roms);
				run(restored, bootSamples);
				CHECK(restored.writeAndVerifyParameterBlock(systemAddress, system,
				                                           replyBudgetSamples));
				CHECK(restored.writeAndVerifyParameterBlock(patchOneAddress, patch,
				                                           replyBudgetSamples));
				CHECK(restored.writeAndVerifyParameterBlock(timbreOneAddress, timbre,
				                                           replyBudgetSamples));
				std::vector<uint8_t> savedSystem, savedPatch, savedTimbre;
				CHECK(restored.requestParameterBlock(systemAddress, systemSoundBytes,
				                                     replyBudgetSamples, savedSystem));
				CHECK(restored.requestParameterBlock(patchOneAddress, patchSelectionBytes,
				                                     replyBudgetSamples, savedPatch));
				CHECK(restored.requestParameterBlock(timbreOneAddress, timbreSoundBytes,
				                                     replyBudgetSamples, savedTimbre));
				CHECK(savedSystem == system);
				CHECK(savedPatch == patch);
				CHECK(savedTimbre == timbre);
				// Capture the firmware's current part-1 sound image from a
				// destination-safe clone, then restore it before the first note.
				run(source, LaBoard::SampleRate);
				std::vector<uint8_t> image;
				CHECK(emu88Lib::LaSettings::isCaptureBoundary(source));
				CHECK(emu88Lib::LaSettings::capture(source, replyBudgetSamples, image) ==
				      emu88Lib::LaSettings::Result::Success);
				CHECK_EQ(image.size(), emu88Lib::LaSettings::ImageBytes);
				if(image.size() == emu88Lib::LaSettings::ImageBytes)
				{
					CHECK(std::equal(system.begin(), system.end(),
					                 image.begin() + emu88Lib::LaSettings::SystemOffset));
					LaBoard candidate(roms);
					CHECK(emu88Lib::LaSettings::restore(candidate, image, bootSamples,
					                                      replyBudgetSamples) ==
					      emu88Lib::LaSettings::Result::Success);
					std::vector<uint8_t> roundtrip;
					CHECK(emu88Lib::LaSettings::capture(candidate, replyBudgetSamples,
					                                    roundtrip) ==
					      emu88Lib::LaSettings::Result::Success);
					CHECK(roundtrip == image);
				}
			}
		}
	}
	else CHECK(argc == 1);

	return test::finish("la board");
}
