#pragma once
#include <Proto/Behavior.hpp>
#include <string_view>

namespace proto {
class Scene;
struct BehaviorSchema {
    std::string sdkBuildId;
    std::vector<sdk::BehaviorDescriptor> types;
    const sdk::BehaviorDescriptor* find(BehaviorTypeId id) const;
};
BehaviorSchema describeRegistry(const sdk::BehaviorRegistry& registry, std::string buildId);
std::string encodeBehaviorSchema(const BehaviorSchema& schema);
BehaviorSchema decodeBehaviorSchema(std::string_view json, std::string_view expectedBuildId);
sdk::PropertyMap normalizeBehaviorProperties(const sdk::BehaviorDescriptor& descriptor, const sdk::PropertyMap& values);
void validateBehaviorValue(const sdk::PropertyValue& value);
void validateBehaviorBinding(const sdk::BehaviorBinding& binding);
void validateSceneBehaviors(const Scene& scene, const BehaviorSchema& schema);
size_t behaviorBytes(const sdk::BehaviorBinding& binding);
} // namespace proto
