#ifndef uranium_type_system_h
#define uranium_type_system_h

#include "value.h"
#include <string>
#include <unordered_map>
#include <vector>

std::string normalizeTypeAnnotation(const std::string& type);
bool isAnyTypeAnnotation(const std::string& type);
bool isConcreteTypeAnnotation(const std::string& type);
bool areTypesCompatible(const std::string& expected, const std::string& actual);
std::string applyTypeBindings(
    const std::string& type,
    const std::unordered_map<std::string, std::string>& bindings);
bool inferTypeBindings(const std::vector<std::string>& genericParameters,
                       const std::vector<std::string>& parameterTypes,
                       const std::vector<std::string>& argumentTypes,
                       std::unordered_map<std::string, std::string>* bindings);
std::string inferArrayLiteralType(const std::vector<std::string>& elementTypes);
std::string inferMapLiteralType(const std::vector<std::string>& valueTypes);
std::string indexedAccessResultType(const std::string& receiverType);
std::string propertyAccessResultType(const std::string& receiverType,
                                     const std::string& property);
std::string runtimeTypeName(const Value& value);
bool valueMatchesTypeAnnotation(const Value& value, const std::string& expected);

#endif
