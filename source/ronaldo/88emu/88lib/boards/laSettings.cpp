#include "88lib/boards/laSettings.h"
#include "88lib/boards/laBoard.h"

#include "baseLib/md5.h"

#include <algorithm>
#include <array>

namespace emu88Lib
{
	namespace
	{
		// Roland MT-32 Owner's Manual, MIDI Implementation §5. Each address
		// digit is seven bits; patch temporary is 03 00 00, timbre temporary
		// is 04 00 00, and system is 10 00 00. Dummy patch bytes are excluded.
		// https://dosdays.co.uk/media/midi/rolandmt-32ownersmanual.pdf
		constexpr uint32_t g_addressDigitBits = 7;
		constexpr uint32_t g_addressBlockShift = g_addressDigitBits * 2;
		constexpr uint32_t g_systemAddress = 0x10u << g_addressBlockShift;
		constexpr uint32_t g_patchAddress = 0x03u << g_addressBlockShift;
		constexpr uint32_t g_patchOutputOffset = 8;
		constexpr uint32_t g_timbreAddress = 0x04u << g_addressBlockShift;
		constexpr uint8_t g_midiDataMaximum = 0x7f;
		constexpr uint32_t g_byteBits = 8;

		struct Block { uint32_t address; size_t offset; size_t size; };
		constexpr std::array<Block, 4> g_blocks{{
			{g_systemAddress, LaSettings::SystemOffset, LaSettings::SystemBytes},
			{g_patchAddress, LaSettings::PatchHeadOffset, LaSettings::PatchHeadBytes},
			{g_patchAddress + g_patchOutputOffset, LaSettings::PatchTailOffset,
			 LaSettings::PatchTailBytes},
			{g_timbreAddress, LaSettings::TimbreOffset, LaSettings::TimbreBytes}
		}};

		std::vector<uint8_t> slice(const std::vector<uint8_t>& image, const Block& block)
		{
			return {image.begin() + block.offset, image.begin() + block.offset + block.size};
		}
	}

	bool LaSettings::supported(const LaBoard& board)
	{
		if(!board.m_valid || !board.m_romAssets) return false;
		const baseLib::MD5 control(board.m_romAssets->control);
		// Only these model/revision pairs passed real-ROM RQ1/DT1 tests. The
		// digests are of the selected complete control images, not filenames.
		return (board.m_model == LaModel::Mt32Old &&
		        control == baseLib::MD5("5626206284b22c2734f3e9efefcd2675")) ||
		       (board.m_model == LaModel::Mt32New &&
		        control == baseLib::MD5("8c4b2c7708be8e88a3ac14777575629c")) ||
		       (board.m_model == LaModel::Cm32l &&
		        control == baseLib::MD5("bfff32b6144c1d706109accb6e6b1113"));
	}

	bool LaSettings::isCaptureBoundary(const LaBoard& board)
	{
		return supported(board) && board.isCaptureInputBoundary();
	}

	LaSettings::Result LaSettings::capture(const LaBoard& board,
	                                      const size_t maxSamplesPerBlock,
	                                      std::vector<uint8_t>& image)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(!board.isCaptureInputBoundary() || maxSamplesPerBlock == 0)
			return Result::InputPending;
		auto privateBoard = board.cloneExecution();
		if(!privateBoard) return Result::QueryFailed;
		std::vector<uint8_t> candidate(ImageBytes);
		for(const auto& block : g_blocks)
		{
			std::vector<uint8_t> data;
			if(!privateBoard->queryParameterBlock(block.address, block.size,
			                                      maxSamplesPerBlock, data) || data.size() != block.size)
				return Result::QueryFailed;
			std::copy(data.begin(), data.end(), candidate.begin() + block.offset);
		}
		candidate[KnobOffset] = static_cast<uint8_t>(board.m_knob & 0xff);
		candidate[KnobOffset + 1] = static_cast<uint8_t>(board.m_knob >> g_byteBits);
		image = std::move(candidate);
		return Result::Success;
	}

	LaSettings::Result LaSettings::restore(LaBoard& board,
	                                      const std::vector<uint8_t>& image,
	                                      const size_t bootSamples,
	                                      const size_t maxSamplesPerBlock)
	{
		if(!supported(board)) return Result::UnsupportedFirmware;
		if(image.size() != ImageBytes || bootSamples == 0 || maxSamplesPerBlock == 0)
			return Result::InvalidImage;
		if(board.m_samplesRendered != 0) return Result::RequiresFreshBoard;
		for(const auto& block : g_blocks)
		{
			if(std::any_of(image.begin() + block.offset, image.begin() + block.offset + block.size,
			               [](const uint8_t value) { return value > g_midiDataMaximum; }))
				return Result::InvalidImage;
		}
		const auto knob = static_cast<uint16_t>(image[KnobOffset] |
			(static_cast<uint16_t>(image[KnobOffset + 1]) << g_byteBits));
		if(knob > LaBoard::KnobMaximum) return Result::InvalidImage;
		board.setKnob(knob);
		for(size_t sample = 0; sample < bootSamples; ++sample) board.renderSample();
		for(const auto& block : g_blocks)
		{
			if(!board.writeAndVerifyParameterBlock(block.address, slice(image, block),
			                                      maxSamplesPerBlock)) return Result::RestoreFailed;
		}
		// The last write may change a previous temporary region. Check the
		// complete image after all DT1 handlers have run.
		for(const auto& block : g_blocks)
		{
			std::vector<uint8_t> data;
			if(!board.queryParameterBlock(block.address, block.size,
			                              maxSamplesPerBlock, data) || data != slice(image, block))
				return Result::RestoreFailed;
		}
		return Result::Success;
	}
}
