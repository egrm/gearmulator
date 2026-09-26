#include "88lib/settingsChunk.h"
#include "common/test_util.hpp"
#include <cstring>
#include <limits>

using namespace emu88Lib;

int main()
{
	SettingsChunk original;
	original.model = DeviceModel::Sc88Pro;
	original.layout = 1;
	original.firmware = baseLib::MD5("9d4c2f123b4451d8ee75c3b982760f28").getWords();
	original.memory = {0, 1, 127, 128, 255};
	const auto encoded = original.encode();
	// Version 2 fixes uint32 fields to little endian, including MD5 words. This
	// fixture was assembled independently and hashed with .NET MD5.HashData.
	const std::vector<uint8_t> golden{
		0x38,0x38,0x48,0x57, 2,0,0,0, 49,0,0,0, 2,0,0,0, 1,0,0,0,
		0x9d,0x4c,0x2f,0x12,0x3b,0x44,0x51,0xd8,0xee,0x75,0xc3,0xb9,0x82,0x76,0x0f,0x28,
		5,0,0,0, 0,1,127,128,255,
		0x8b,0xc2,0xb9,0x8b,0x22,0xd3,0x3b,0x16,0xb2,0x68,0xda,0xde,0x95,0xd7,0x80,0xdd};
	CHECK(encoded == golden);
	CHECK(SettingsChunk::decode(golden.data(), golden.size()).has_value());
	auto legacy = golden;
	legacy[4] = 1;
	const auto migrated = SettingsChunk::decode(legacy.data(), legacy.size());
	CHECK(migrated.has_value());
	if(migrated) CHECK(migrated->encode() == golden);
	CHECK(!encoded.empty());
	const auto restored = SettingsChunk::decode(encoded.data(), encoded.size());
	CHECK(restored.has_value());
	if(restored)
	{
		CHECK(restored->model == original.model);
		CHECK(restored->layout == original.layout);
		CHECK(restored->firmware == original.firmware);
		CHECK(restored->memory == original.memory);
	}
	bool truncatedRejected = true;
	for(size_t length = 0; length < encoded.size(); ++length)
		truncatedRejected &= !SettingsChunk::decode(encoded.data(), length).has_value();
	CHECK(truncatedRejected);
	bool mutationRejected = true;
	for(size_t offset = 0; offset < encoded.size(); ++offset)
	{
		auto mutated = encoded;
		mutated[offset] ^= 1;
		mutationRejected &= !SettingsChunk::decode(mutated.data(), mutated.size()).has_value();
	}
	CHECK(mutationRejected);
	CHECK(!SettingsChunk::decode(nullptr, encoded.size()));
	// Reject oversized extents before reading or hashing. A header consistent with
	// the claimed extent must not reach MD5's uint32 bit-length overflow path.
	auto oversizedHash = golden;
	constexpr size_t firstUnsupportedBody = size_t{1} << 29;
	constexpr size_t oversizedExtent = firstUnsupportedBody + 12 + 16;
	const uint32_t oversizedPayload = static_cast<uint32_t>(oversizedExtent - 12);
	const uint32_t oversizedData = static_cast<uint32_t>(oversizedExtent - 56);
	std::memcpy(oversizedHash.data() + 8, &oversizedPayload, sizeof(oversizedPayload));
	std::memcpy(oversizedHash.data() + 36, &oversizedData, sizeof(oversizedData));
	CHECK(!SettingsChunk::decode(oversizedHash.data(), oversizedExtent));
	auto damaged = encoded;
	damaged.back() ^= 1;
	CHECK(!SettingsChunk::decode(damaged.data(), damaged.size()));
	auto trailing = encoded; trailing.push_back(0);
	CHECK(!SettingsChunk::decode(trailing.data(), trailing.size()));
	// Shared ChunkWriter header: FourCC, uint32 version, uint32 payload length.
	auto oversized = encoded;
	const uint32_t maximumLength = std::numeric_limits<uint32_t>::max();
	std::memcpy(oversized.data() + 8, &maximumLength, sizeof(maximumLength));
	CHECK(!SettingsChunk::decode(oversized.data(), oversized.size()));
	auto oversizedMemory = encoded;
	std::memcpy(oversizedMemory.data() + 36, &maximumLength, sizeof(maximumLength));
	CHECK(!SettingsChunk::decode(oversizedMemory.data(), oversizedMemory.size()));
	auto wrongVersion = encoded; wrongVersion[4] = 3;
	CHECK(!SettingsChunk::decode(wrongVersion.data(), wrongVersion.size()));
	auto wrongModel = encoded;
	std::memcpy(wrongModel.data() + 12, &maximumLength, sizeof(maximumLength));
	CHECK(!SettingsChunk::decode(wrongModel.data(), wrongModel.size()));
	original.model = static_cast<DeviceModel>(255);
	CHECK(original.encode().empty());
	original.model = DeviceModel::Sc88Pro;
	original.layout = 0;
	CHECK(original.encode().empty());
	original.layout = 1;
	original.firmware = {};
	CHECK(original.encode().empty());
	original.firmware = baseLib::MD5("9d4c2f123b4451d8ee75c3b982760f28").getWords();
	original.memory.clear();
	CHECK(original.encode().empty());
	return test::finish("settingsChunk");
}
