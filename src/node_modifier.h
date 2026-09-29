#pragma once

#include <functional>
#include <bitset>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "irrlichttypes.h"
#include "nameidmapping.h"
#include "nodedef.h"

#define MODIFIER_IGNORE ((u16)-1)


enum ModifiedProperty : u64 {
	//MP_TileDef = u64{1} << 0,
	MP_LightSource = u64{1} << 0,
};

// Holds properties which might be drawtype-dependent
/*struct NodeModifierDrawtypeDependent {
	struct TileDef tiledef[6];
	TileDef tiledef_overlay[6];
	TileDef tiledef_special[CF_SPECIAL_COUNT]; // eg. flowing liquid
	AlphaMode alpha;
	// The color of the node.
	video::SColor color;
	std::string palette_name;

#if CHECK_CLIENT_BUILD()
	// The Client class fills this for its NodeDefManager using fillNodeVisuals,
	// thus for ContentFeatures of a Client it is not a nullptr.
	std::unique_ptr<NodeVisuals> visuals;
#endif
};*/

struct NodeModifier {
	NodeModifier(const std::string &name, bool is_dummy=false)
			: m_name(name), m_is_dummy(is_dummy) {};

	std::string m_name;
	bool m_is_dummy;

	u8 m_light_source = 0;

	//std::array<NodeModifierDrawtypeDependent *, NodeDrawType_END> visuals = {nullptr};

	u64 m_modified_props_mask = 0;

	//ContentFeatures m_content_features;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);
};

class NodeModifierManager {
public:
	NodeModifierManager() {};

	u16 add(NodeModifier &&nodeModifier);
	u16 getId(const std::string &name) const;
	const NodeModifier& get(u16 id) const;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);

	inline u16 size() const {
		return m_modifiers.size();
	}

	/*!
	 * Applies a function to all Content Features.
	 * Clients need this to make use of the visuals field.
	 * @param function to apply
	 */
	//void applyFunction(const std::function<void(ContentFeatures&)> &function);

private:
	NameIdMapping m_name_id_mapping;
	std::vector<NodeModifier> m_modifiers;
	const inline static NodeModifier dummy{"<unknown>", true};
	//std::unordered_map<std::string, NodeModifier> modifiers;
};

NodeModifierManager *createNodeModifierManager();

// Resolved overrides only; properties absent from mask retain the base definition.
struct NodePropertyOverrides {
	u64 mask = 0;
	u8 light_source = 0;

	ContentLightingFlags apply(ContentLightingFlags base) const;
};

struct AppliedNodeModifier {
	v3s16 pos;
	u16 id;
};

class AppliedNodeModifiersList {
public:
	AppliedNodeModifiersList() {};
	using ConstIterator = std::vector<AppliedNodeModifier>::const_iterator;
	using ConstRange = std::pair<ConstIterator, ConstIterator>;
	ConstRange findRange(v3s16 pos) const;

	// Random access: O(log M + K), where K modifiers apply at pos.
	NodePropertyOverrides resolve(v3s16 pos, const NodeModifierManager &manager) const;

	class Cursor {
	public:
		// Positions must be nondecreasing in v3s16's X/Y/Z ordering.
		// Repeated positions reuse the resolved value. A scan costs O(N + M).
		NodePropertyOverrides resolve(v3s16 pos);

	private:
		friend class AppliedNodeModifiersList;
		Cursor(ConstIterator first, ConstIterator last, const NodeModifierManager &manager,
				bool allow_skipping) :
				m_next(first), m_end(last), m_manager(manager),
				m_allow_skipping(allow_skipping) {}

		ConstIterator m_next;
		ConstIterator m_end;
		const NodeModifierManager &m_manager;
		bool m_allow_skipping;
		bool m_has_position = false;
		v3s16 m_position;
		NodePropertyOverrides m_value;
	};

	// The list and manager must outlive the cursor and remain unchanged.
	// Any list or modifier-definition mutation invalidates the cursor.
	// If skipping is disabled, every modified position must be visited in order.
	// If enabled, entries preceding a requested position are skipped permanently.
	Cursor cursor(const NodeModifierManager &manager, bool allow_skipping=false) const {
		return Cursor(m_list.begin(), m_list.end(), manager, allow_skipping);
	}

	// Merge translated entries, preserving order within each position.
	void append(const AppliedNodeModifiersList &other, v3s16 offset);

	// To do: check usages outside the function
	const std::vector<AppliedNodeModifier> &entries() const { return m_list; }

	bool add(v3s16 pos, u16 id);
	// Replace one node's modifiers, keeping the last occurrence of duplicate IDs.
	// Invalid IDs or insufficient capacity leave the list unchanged.
	bool set(v3s16 pos, const std::vector<u16> &ids);
	bool remove(v3s16 pos, u16 id);
	bool remove(v3s16 pos);

	// Disk entries must already use the local IDs described by nimap.
	void serialize(std::ostream &os, u16 protocol_version, bool disk, const NameIdMapping &nimap) const;
	// Disk entries retain local IDs; nimap receives their names.
	void deSerialize(std::istream &is, u16 protocol_version, bool disk, NameIdMapping &nimap);
	// Change IDs without changing positions or modifier application order.
	void remapIds(const std::function<u16(u16)> &map_id);
private:
	// To do: consider optimizing for possible large number of modifiers
	std::vector<AppliedNodeModifier> m_list;
	using Iterator = std::vector<AppliedNodeModifier>::iterator;
	using Range = std::pair<Iterator, Iterator>;

	// Find the range [first, last) matching pos; m_list must be sorted by pos.
	// If absent, both iterators give the insertion position.
	Range findMutableRange(v3s16 pos);

	static NodePropertyOverrides resolveInRange(AppliedNodeModifiersList::ConstRange range,
			const NodeModifierManager &manager);
};
