#include <string>
#include <unordered_map>
#include <vector>

#include "irrlichttypes.h"
#include "nodedef.h"

struct NodeModifier {
	NodeModifier(const ContentFeatures &def);
	ContentFeatures m_content_features;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);
};

class NodeModifierManager {
public:
	NodeModifierManager() {};

	bool set(const std::string &name, const NodeModifier &nodeModifier);
	NodeModifier *get(const std::string &name) const;

	void serialize(std::ostream &os, u16 protocol_version) const;
	void deSerialize(std::istream &is, u16 protocol_version);

private:
	std::unordered_map<std::string, NodeModifier> modifiers;
};