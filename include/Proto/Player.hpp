#pragma once
#include <Proto/Behavior.hpp>
namespace proto::sdk {
using Registration = void (*)(BehaviorRegistry&);
int RunPlayer(int argc, wchar_t** argv, Registration registerBehaviors);
} // namespace proto::sdk
