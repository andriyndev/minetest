// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "catch.h"
#include "constants.h"
#include "node_modifier.h"
#include <array>

TEST_CASE("benchmark_node_modifier_lookup")
{
	NodeModifierManager manager;
	NodeModifier modifier("benchmark:light");
	modifier.m_modified_props_mask = MP_LightSource;
	modifier.m_light_source = 14;
	u16 id = manager.add(std::move(modifier));

	// One block, with the X-fastest traversal used by mesh generation.
	std::vector<v3s16> positions;
	for (s16 z = 0; z < MAP_BLOCKSIZE; ++z)
	for (s16 y = 0; y < MAP_BLOCKSIZE; ++y)
	for (s16 x = 0; x < MAP_BLOCKSIZE; ++x)
		positions.emplace_back(x, y, z);

	for (size_t count : {16, 256, 4096}) {
		AppliedNodeModifiersList list;
		for (size_t i = 0; i < count; ++i)
			REQUIRE(list.add(positions[i * positions.size() / count], id));

		BENCHMARK("mesh-order lookup, modifiers=" + std::to_string(count)) {
			u32 sum = 0;
			for (v3s16 pos : positions)
				sum += list.resolve(pos, manager).light_source;
			return sum;
		};
		BENCHMARK("ordered cursor, modifiers=" + std::to_string(count)) {
			u32 sum = 0;
			auto cursor = list.cursor(manager);
			for (s16 x = 0; x < MAP_BLOCKSIZE; ++x)
			for (s16 y = 0; y < MAP_BLOCKSIZE; ++y)
			for (s16 z = 0; z < MAP_BLOCKSIZE; ++z)
				sum += cursor.resolve({x, y, z}).light_source;
			return sum;
		};

		// A repeatable approximation of eight local samples per node. These
		// measure lookup only, not complete mesh generation or light propagation.
		BENCHMARK("eight lookups per node, modifiers=" + std::to_string(count)) {
			u32 sum = 0;
			for (v3s16 pos : positions) {
				for (s16 offset = 0; offset < 8; ++offset) {
					v3s16 sample((pos.X + (offset & 1)) % MAP_BLOCKSIZE,
						(pos.Y + ((offset >> 1) & 1)) % MAP_BLOCKSIZE,
						(pos.Z + ((offset >> 2) & 1)) % MAP_BLOCKSIZE);
					sum += list.resolve(sample, manager).light_source;
				}
			}
			return sum;
		};
		BENCHMARK("build dense snapshot + eight reads, modifiers=" + std::to_string(count)) {
			std::array<u8, MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE> sources;
			auto cursor = list.cursor(manager);
			for (s16 x = 0; x < MAP_BLOCKSIZE; ++x)
			for (s16 y = 0; y < MAP_BLOCKSIZE; ++y)
			for (s16 z = 0; z < MAP_BLOCKSIZE; ++z)
				sources[x + MAP_BLOCKSIZE * (y + MAP_BLOCKSIZE * z)] =
						cursor.resolve({x, y, z}).light_source;
			u32 sum = 0;
			for (v3s16 pos : positions) {
				for (s16 offset = 0; offset < 8; ++offset) {
					s16 x = (pos.X + (offset & 1)) % MAP_BLOCKSIZE;
					s16 y = (pos.Y + ((offset >> 1) & 1)) % MAP_BLOCKSIZE;
					s16 z = (pos.Z + ((offset >> 2) & 1)) % MAP_BLOCKSIZE;
					sum += sources[x + MAP_BLOCKSIZE * (y + MAP_BLOCKSIZE * z)];
				}
			}
			return sum;
		};
	}
}
