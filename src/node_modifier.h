#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "irrlichttypes.h"
#include "nodedef.h"

struct NodeModifier {
	NodeModifier() {};
	NodeModifier(ContentFeatures &&def);
	ContentFeatures m_content_features;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);
};

class NodeModifierManager {
public:
	NodeModifierManager() {};

	bool set(const std::string &name, NodeModifier &&nodeModifier);
	const NodeModifier *get(const std::string &name) const;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);

	inline u32 size() const {
		return modifiers.size();
	}

	/*!
	 * Applies a function to all Content Features.
	 * Clients need this to make use of the visuals field.
	 * @param function to apply
	 */
	void applyFunction(const std::function<void(ContentFeatures&)> &function);

private:
	std::unordered_map<std::string, NodeModifier> modifiers;
};

NodeModifierManager *createNodeModifierManager();