#include "lua_api/l_node_modifier.h"
#include "lua_api/l_internal.h"
#include "common/c_content.h"
#include "node_modifier.h"
#include "server.h"

int ModApiNodeModifier::l_register_node_modifier_raw(lua_State *L)
{
	NO_MAP_LOCK_REQUIRED;
	luaL_checktype(L, 1, LUA_TTABLE);
	int table = 1;

	auto *matdef = getServer(L)->getWritableNodeModifierManager();

	std::string name;
	lua_getfield(L, table, "name");
	name = luaL_checkstring(L, -1);

	ContentFeatures f;
	read_content_features(L, f, table);

	if (f.name.empty())
		throw LuaError("Cannot register node modifier with empty name");

	NodeModifier mat(std::move(f));
	matdef->set(name, std::move(mat));

	return 0;
}

void ModApiNodeModifier::Initialize(lua_State *L, int top)
{
	API_FCT(register_node_modifier_raw);
}