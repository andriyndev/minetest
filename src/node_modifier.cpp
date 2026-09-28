#include "node_modifier.h"
#include "util/serialize.h"
#if CHECK_CLIENT_BUILD()
#include "client/node_visuals.h" // ~NodeVisuals
#endif

NodeModifier::NodeModifier(ContentFeatures &&def) {
	m_content_features = std::move(def);

	/* To do: add other functionality as with example of:
	 * content_t NodeDefManager::set(const std::string &name, const ContentFeatures &def) */
}

void NodeModifier::serialize(std::ostream &os, u16 protocol_version) const {
	m_content_features.serialize(os, protocol_version);
}

void NodeModifier::deSerialize(std::istream &is, u16 protocol_version) {
	m_content_features.deSerialize(is, protocol_version);
}

bool NodeModifierManager::set(const std::string &name, NodeModifier &&nodeModifier) {
	auto res = modifiers.emplace(name, std::move(nodeModifier));
	return res.second;
}

const NodeModifier *NodeModifierManager::get(const std::string &name) const {
	auto it = modifiers.find(name);
	if (it != modifiers.end())
		return &(*it).second;
	return nullptr;
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

	for (u32 i = 0; i < modifiers_num; i++) {
		std::string name = deSerializeString16(is);
		modifiers[name].deSerialize(is, protocol_version);
	}
}

void NodeModifierManager::applyFunction(const std::function<void(ContentFeatures&)> &function) {
	for (auto &f : modifiers)
		function(f.second.m_content_features);
}

NodeModifierManager *createNodeModifierManager() {
	return new NodeModifierManager();
}
