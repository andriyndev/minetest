#include "node_modifier.h"
#include "util/serialize.h"
#include <algorithm>
#include <cassert>
#include <unordered_set>
#if CHECK_CLIENT_BUILD()
#include "client/node_visuals.h" // ~NodeVisuals
#endif

void NodeModifier::serialize(std::ostream &os, u16 protocol_version) const {
	writeU64(os, m_modified_props_mask);
	writeS8(os, m_is_dummy);
	if (m_modified_props_mask & MP_LightSource) {
		writeU8(os, m_light_source);
	}
}

void NodeModifier::deSerialize(std::istream &is, u16 protocol_version) {
	m_modified_props_mask = readU64(is);
	m_is_dummy = readS8(is);
	/*if (m_modified_props_mask & MP_TileDef) {
		for (TileDef &td : tiledef)
			td.deSerialize(is, drawtype, protocol_version);
	}*/
	if (m_modified_props_mask & MP_LightSource) {
		m_light_source = readU8(is);
	}
}

u16 NodeModifierManager::add(NodeModifier &&nodeModifier) {
	u16 existing_id;
	if (m_name_id_mapping.getId(nodeModifier.m_name, existing_id)) {
		// Replace if a modifier with the same name already exists
		m_modifiers[existing_id] = std::move(nodeModifier);

		return existing_id;
	}

	if (m_modifiers.size() >= MODIFIER_IGNORE)
		return MODIFIER_IGNORE;

	m_name_id_mapping.set(m_modifiers.size(), nodeModifier.m_name);
	m_modifiers.push_back(std::move(nodeModifier));

	return m_modifiers.size() - 1;
}

u16 NodeModifierManager::getId(const std::string &name) const {
	u16 id;
	bool is_present = m_name_id_mapping.getId(name, id);
	if (is_present)
		return id;
	else
		return MODIFIER_IGNORE;
}

const NodeModifier& NodeModifierManager::get(u16 id) const {
	if (id >= m_modifiers.size())
		return dummy;

	return m_modifiers[id];
}

void NodeModifierManager::serialize(std::ostream &os, u16 protocol_version) const {
	u16 size = m_modifiers.size();
	writeU16(os, size);
	for (u16 i = 0; i < size; i++) {
		const NodeModifier& cur = m_modifiers[i];
		os << serializeString16(cur.m_name);
		cur.serialize(os, protocol_version);
	}
}
void NodeModifierManager::deSerialize(std::istream &is, u16 protocol_version) {
	u16 modifiers_num = readU16(is);

	m_modifiers.clear();
	m_name_id_mapping.clear();
	m_modifiers.reserve(modifiers_num);

	for (u16 i = 0; i < modifiers_num; i++) {
		std::string name = deSerializeString16(is);
		if (name.empty()) {
			throw SerializationError("NodeModifierManager::deSerialize(): "
				"The name of the node modifier is empty");
		}
		u16 existing_id;
		if (m_name_id_mapping.getId(name, existing_id)) {
			throw SerializationError("NodeModifierManager::deSerialize(): "
				"Node modifier with the name " + name + " was already deserialized");
		}
		m_name_id_mapping.set(m_modifiers.size(), name);
		m_modifiers.push_back(NodeModifier(name));
		m_modifiers[i].deSerialize(is, protocol_version);
	}
}

/*void NodeModifierManager::applyFunction(const std::function<void(ContentFeatures&)> &function) {
	for (auto &f : m_modifiers)
		function(f.m_content_features);
}*/

NodeModifierManager *createNodeModifierManager() {
	return new NodeModifierManager();
}

ContentLightingFlags NodePropertyOverrides::apply(ContentLightingFlags base) const
{
	if (mask & MP_LightSource)
		base.light_source = light_source;
	return base;
}

NodePropertyOverrides AppliedNodeModifiersList::resolveInRange(
		AppliedNodeModifiersList::ConstRange range, const NodeModifierManager &manager)
{
	NodePropertyOverrides result;
	for (auto it = range.first; it != range.second; ++it) {
		const NodeModifier &modifier = manager.get(it->id);
		// Unknown IDs and named placeholders are inert.
		if (modifier.m_is_dummy)
			continue;
		if (modifier.m_modified_props_mask & MP_LightSource) {
			result.mask |= MP_LightSource;
			result.light_source = modifier.m_light_source;
		}
	}
	return result;
}

NodePropertyOverrides AppliedNodeModifiersList::resolve(v3s16 pos,
		const NodeModifierManager &manager) const
{
	return resolveInRange(findRange(pos), manager);
}

NodePropertyOverrides AppliedNodeModifiersList::Cursor::resolve(v3s16 pos)
{
	assert(!m_has_position || !(pos < m_position));
	if (m_has_position && pos == m_position)
		return m_value;
	if (m_allow_skipping) {
		while (m_next != m_end && m_next->pos < pos)
			++m_next;
	} else {
		assert(m_next == m_end || m_next->pos >= pos);
	}
	ConstIterator first = m_next;
	while (m_next != m_end && m_next->pos == pos)
		++m_next;
	m_value = resolveInRange({first, m_next}, m_manager);
	m_position = pos;
	m_has_position = true;
	return m_value;
}

bool AppliedNodeModifiersList::add(v3s16 pos, u16 id) {
	auto [first, last] = findMutableRange(pos);

	for (auto it = first; it != last; ++it) {
		if (it->id == id) {
			// Already applied. Move it to the end of this node's modifiers.
			std::rotate(it, it + 1, last);
			return true;
		}
	}

	if (m_list.size() >= ((u16)-1))
		return false;

	AppliedNodeModifier entry;
	entry.pos = pos;
	entry.id = id;
	m_list.insert(last, entry);

	return true;
}

bool AppliedNodeModifiersList::set(v3s16 pos, const std::vector<u16> &ids)
{
	std::vector<u16> unique;
	std::unordered_set<u16> seen;
	for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
		if (*it == MODIFIER_IGNORE)
			return false;
		if (seen.insert(*it).second)
			unique.push_back(*it);
	}

	auto [first, last] = findMutableRange(pos);
	const size_t offset = first - m_list.begin();
	const size_t old_count = last - first;
	if (m_list.size() - old_count + unique.size() > MODIFIER_IGNORE)
		return false;

	// Resize the range once, then overwrite it without moving the tail again.
	if (unique.size() > old_count)
		m_list.insert(last, unique.size() - old_count, AppliedNodeModifier{pos, 0});
	else if (unique.size() < old_count)
		m_list.erase(first + unique.size(), last);
	for (size_t i = 0; i < unique.size(); ++i)
		m_list[offset + i] = {pos, unique[unique.size() - 1 - i]};
	return true;
}

bool AppliedNodeModifiersList::remove(v3s16 pos, u16 id) {
	auto [first, last] = findMutableRange(pos);

	for (auto it = first; it != last; ++it) {
		if (it->id == id) {
			m_list.erase(it);
			return true;
		}
	}
	return false;
}

bool AppliedNodeModifiersList::remove(v3s16 pos)
{
	auto [first, last] = findMutableRange(pos);
	if (first == last)
		return false;
	m_list.erase(first, last);
	return true;
}

AppliedNodeModifiersList::Range AppliedNodeModifiersList::findMutableRange(v3s16 pos) {
	auto cmp_func = [](const AppliedNodeModifier &entry, const v3s16 &pos) {
		return entry.pos < pos;
	};
	// To do: maybe optimize
	const auto first = std::lower_bound(m_list.begin(), m_list.end(), pos, cmp_func);

	auto last = first;
	//It is expected that there will be few modifiers per node
	while (last != m_list.end() && last->pos == pos) {
		last++;
	}

	return {first, last};
}

void AppliedNodeModifiersList::serialize(std::ostream &os, u16 protocol_version,
		bool disk, const NameIdMapping &nimap) const {
	u16 size = m_list.size();
	assert(size == m_list.size());
	writeU16(os, size);
	if (size == 0)
		return;

	if (disk)
		nimap.serialize(os);

	for (const auto &it : m_list) {
		writeV3S16(os, it.pos);
		writeU16(os, it.id);
	}
}

void AppliedNodeModifiersList::deSerialize(std::istream &is, u16 protocol_version,
		bool disk, NameIdMapping &nimap) {
	u16 size = readU16(is);
	m_list.clear();
	nimap.clear();
	if (size == 0)
		return;

	if (disk)
		nimap.deSerialize(is);

	std::vector<AppliedNodeModifier> temp_list;
	temp_list.reserve(size);

	AppliedNodeModifier modifier;
	v3s16 prev_pos;

	for (u16 i = 0; i < size; i++) {
		modifier.pos = readV3S16(is);
		modifier.id = readU16(is);
		if (i != 0 && prev_pos > modifier.pos)
			throw SerializationError("AppliedNodeModifiersList::deSerialize(): "
				"Serialized array is not sorted");
		temp_list.push_back(modifier);
		prev_pos = modifier.pos;
	}

	m_list = std::move(temp_list);
}

void AppliedNodeModifiersList::remapIds(const std::function<u16(u16)> &map_id)
{
	for (auto &entry : m_list)
		entry.id = map_id(entry.id);
}

AppliedNodeModifiersList::ConstRange AppliedNodeModifiersList::findRange(v3s16 pos) const
{
	auto cmp_func = [](const AppliedNodeModifier &entry, v3s16 pos) {
		return entry.pos < pos;
	};
	auto first = std::lower_bound(m_list.begin(), m_list.end(), pos, cmp_func);
	auto last = first;
	while (last != m_list.end() && last->pos == pos)
		++last;
	return {first, last};
}

// Used for merging several block-specific lists into one mesh-specific list
void AppliedNodeModifiersList::append(const AppliedNodeModifiersList &other, v3s16 offset)
{
	auto cmp_func = [](const AppliedNodeModifier &a, const AppliedNodeModifier &b) {
		return a.pos < b.pos;
	};
	auto entries = other.m_list;
	for (auto &entry : entries)
		entry.pos += offset;
	const auto split = m_list.size();
	m_list.insert(m_list.end(), entries.begin(), entries.end());
	std::inplace_merge(m_list.begin(), m_list.begin() + split, m_list.end(), cmp_func);
}
