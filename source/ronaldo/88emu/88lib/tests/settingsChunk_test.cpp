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
	auto wrongVersion = encoded; wrongVersion[4] = 2;
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
