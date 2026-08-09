#include "node_modifier.h"
#include "util/serialize.h"

NodeModifier::NodeModifier(const ContentFeatures &def) {
	m_content_features = def;

	/* To do: add other functionality as with example of:
	 * content_t NodeDefManager::set(const std::string &name, const ContentFeatures &def) */
}

void NodeModifier::serialize(std::ostream &os, u16 protocol_version) const {
	m_content_features.serialize(os, protocol_version);
}

void NodeModifier::deSerialize(std::istream &is, u16 protocol_version) {
	m_content_features.deSerialize(is, protocol_version);
}

bool NodeModifierManager::set(const std::string &name, const NodeModifier &nodeModifier) {
	auto res = modifiers.insert({name, nodeModifier});
	return res.second;
}

NodeModifier *NodeModifierManager::get(const std::string &name) const {
	auto it = modifiers.find(name);
	if (it != modifiers.end())
		return &(*it).second;
}

void NodeModifierManager::serialize(std::ostream &os, u16 protocol_version) const {
	writeU32(os, modifiers.size());
	// To do: add compressing (probably)
	for (auto it = modifiers.begin(); it != modifiers.end(); it++) {
		os << serializeString16(it->first);
		it->second.serialize(os, protocol_version);
	}
}
void NodeModifierManager::deSerialize(std::istream &is, u16 protocol_version) {
	u32 modifiers_num = readU32(is);

	modifiers.clear();
	modifiers.reserve(modifiers_num);

	for (int i = 0; i < modifiers_num; i++) {
		std::string name = deSerializeString16(is);
		modifiers[name].deSerialize(is, protocol_version);
	}
}