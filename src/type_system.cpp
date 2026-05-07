#include "type_system.h"

#include "object.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

std::string normalizeTypeAnnotation(const std::string& type);
bool areTypesCompatible(const std::string& expected, const std::string& actual);
std::string inferArrayLiteralType(const std::vector<std::string>& elementTypes);
std::string inferMapLiteralType(const std::vector<std::string>& valueTypes);

namespace {

struct TypeExpr {
    std::string name;
    std::vector<TypeExpr> args;
};

bool isIdentifierCharacter(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.';
}

std::string trimSpaces(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            result.push_back(ch);
        }
    }
    return result;
}

bool parseTypeExpr(const std::string& text, std::size_t* cursor, TypeExpr* out) {
    if (cursor == nullptr || out == nullptr || *cursor >= text.size()) {
        return false;
    }

    std::size_t start = *cursor;
    while (*cursor < text.size() && isIdentifierCharacter(text[*cursor])) {
        (*cursor)++;
    }

    if (*cursor == start) {
        return false;
    }

    out->name = text.substr(start, *cursor - start);
    out->args.clear();

    if (*cursor < text.size() && text[*cursor] == '<') {
        (*cursor)++;
        do {
            TypeExpr child;
            if (!parseTypeExpr(text, cursor, &child)) {
                return false;
            }
            out->args.push_back(std::move(child));
            if (*cursor < text.size() && text[*cursor] == ',') {
                (*cursor)++;
                continue;
            }
            break;
        } while (*cursor < text.size());

        if (*cursor >= text.size() || text[*cursor] != '>') {
            return false;
        }
        (*cursor)++;
    }

    return true;
}

bool tryParseTypeExpr(const std::string& text, TypeExpr* out) {
    if (out == nullptr) {
        return false;
    }

    std::string normalized = trimSpaces(text);
    if (normalized.empty()) {
        return false;
    }

    std::size_t cursor = 0;
    if (!parseTypeExpr(normalized, &cursor, out)) {
        return false;
    }

    return cursor == normalized.size();
}

bool isGenericParameter(const std::vector<std::string>& generics, const std::string& name) {
    return std::find(generics.begin(), generics.end(), name) != generics.end();
}

std::string stringifyTypeExpr(const TypeExpr& expr) {
    std::string result = expr.name;
    if (!expr.args.empty()) {
        result.push_back('<');
        for (std::size_t index = 0; index < expr.args.size(); ++index) {
            if (index > 0) {
                result.push_back(',');
            }
            result += stringifyTypeExpr(expr.args[index]);
        }
        result.push_back('>');
    }
    return result;
}

std::string mergeInferredTypes(const std::vector<std::string>& types) {
    std::string merged;
    bool sawConcrete = false;

    for (const std::string& type : types) {
        std::string normalized = normalizeTypeAnnotation(type);
        if (normalized == "Any") {
            return "Any";
        }

        if (!sawConcrete) {
            merged = normalized;
            sawConcrete = true;
            continue;
        }

        if (!areTypesCompatible(merged, normalized) ||
            !areTypesCompatible(normalized, merged)) {
            return "Any";
        }
    }

    if (!sawConcrete) {
        return "Any";
    }

    return merged;
}

bool inferBindingsFromExpr(const TypeExpr& expected,
                           const TypeExpr& actual,
                           const std::vector<std::string>& genericParameters,
                           std::unordered_map<std::string, std::string>* bindings) {
    if (bindings == nullptr) {
        return false;
    }

    if (expected.name == "Any") {
        return true;
    }

    if (isGenericParameter(genericParameters, expected.name) && expected.args.empty()) {
        auto existing = bindings->find(expected.name);
        std::string actualText = stringifyTypeExpr(actual);
        if (existing == bindings->end()) {
            (*bindings)[expected.name] = actualText;
            return true;
        }
        return existing->second == actualText;
    }

    if (expected.name != actual.name || expected.args.size() != actual.args.size()) {
        return false;
    }

    for (std::size_t index = 0; index < expected.args.size(); ++index) {
        if (!inferBindingsFromExpr(expected.args[index], actual.args[index],
                                   genericParameters, bindings)) {
            return false;
        }
    }

    return true;
}

TypeExpr applyBindingsToExpr(const TypeExpr& expr,
                             const std::unordered_map<std::string, std::string>& bindings) {
    auto binding = bindings.find(expr.name);
    if (binding != bindings.end() && expr.args.empty()) {
        TypeExpr rebound;
        if (tryParseTypeExpr(binding->second, &rebound)) {
            return rebound;
        }
    }

    TypeExpr rebound = expr;
    for (TypeExpr& child : rebound.args) {
        child = applyBindingsToExpr(child, bindings);
    }
    return rebound;
}

bool matchesTypeExpr(const Value& value, const TypeExpr& expected);

bool matchesArrayType(const ArrayPtr& array, const TypeExpr& expected) {
    if (expected.args.empty()) {
        return true;
    }

    if (array == nullptr) {
        return false;
    }

    for (const Value& element : array->elements) {
        if (!matchesTypeExpr(element, expected.args[0])) {
            return false;
        }
    }

    return true;
}

bool matchesMapType(const MapPtr& map, const TypeExpr& expected) {
    if (expected.args.size() < 2) {
        return true;
    }

    if (map == nullptr) {
        return false;
    }

    TypeExpr keyType = expected.args[0];
    TypeExpr valueType = expected.args[1];
    for (const auto& entry : map->entries) {
        if (keyType.name != "Any" && keyType.name != "String") {
            return false;
        }
        if (!matchesTypeExpr(entry.second, valueType)) {
            return false;
        }
    }

    return true;
}

bool matchesTypeExpr(const Value& value, const TypeExpr& expected) {
    if (expected.name.empty() || expected.name == "Any") {
        return true;
    }

    if (expected.name == "Nil") {
        return value.isNil();
    }
    if (expected.name == "Bool") {
        return value.isBool();
    }
    if (expected.name == "Number") {
        return value.isNumber();
    }
    if (expected.name == "String") {
        return value.isString();
    }
    if (expected.name == "Function") {
        return value.isFunction() || value.isClosure() || value.isNativeFunction() ||
               value.isBoundMethod();
    }
    if (expected.name == "Task") {
        return value.isTask();
    }
    if (expected.name == "Array") {
        return value.isArray() && matchesArrayType(value.asArray(), expected);
    }
    if (expected.name == "Map") {
        return value.isMap() && matchesMapType(value.asMap(), expected);
    }
    if (expected.name == "Class") {
        return value.isClass();
    }
    if (expected.name == "Instance") {
        return value.isInstance();
    }

    if (value.isClass() && value.asClass() != nullptr) {
        return value.asClass()->name == expected.name;
    }
    if (value.isInstance() && value.asInstance() != nullptr &&
        value.asInstance()->klass != nullptr) {
        return value.asInstance()->klass->name == expected.name;
    }

    return false;
}

std::string inferArrayRuntimeType(const ArrayPtr& array,
                                  int depth,
                                  std::unordered_set<const HeapObject*>* seen);
std::string inferMapRuntimeType(const MapPtr& map,
                                int depth,
                                std::unordered_set<const HeapObject*>* seen);

std::string runtimeTypeNameInternal(const Value& value,
                                    int depth,
                                    std::unordered_set<const HeapObject*>* seen) {
    if (depth > 6 || seen == nullptr) {
        return "Any";
    }

    if (value.isNil()) {
        return "Nil";
    }
    if (value.isBool()) {
        return "Bool";
    }
    if (value.isNumber()) {
        return "Number";
    }
    if (value.isString()) {
        return "String";
    }
    if (value.isArray()) {
        return inferArrayRuntimeType(value.asArray(), depth + 1, seen);
    }
    if (value.isMap()) {
        return inferMapRuntimeType(value.asMap(), depth + 1, seen);
    }
    if (value.isTask()) {
        return "Task";
    }
    if (value.isClass()) {
        if (value.asClass() != nullptr && !value.asClass()->name.empty()) {
            return value.asClass()->name;
        }
        return "Class";
    }
    if (value.isInstance()) {
        if (value.asInstance() != nullptr &&
            value.asInstance()->klass != nullptr &&
            !value.asInstance()->klass->name.empty()) {
            return value.asInstance()->klass->name;
        }
        return "Instance";
    }
    if (value.isFunction() || value.isClosure() || value.isNativeFunction() ||
        value.isBoundMethod()) {
        return "Function";
    }

    return "Any";
}

std::string inferArrayRuntimeType(const ArrayPtr& array,
                                  int depth,
                                  std::unordered_set<const HeapObject*>* seen) {
    if (array == nullptr) {
        return "Array<Any>";
    }

    const HeapObject* marker = static_cast<const HeapObject*>(array);
    if (seen->find(marker) != seen->end()) {
        return "Array<Any>";
    }

    seen->insert(marker);
    std::vector<std::string> elementTypes;
    elementTypes.reserve(array->elements.size());
    for (const Value& element : array->elements) {
        elementTypes.push_back(runtimeTypeNameInternal(element, depth, seen));
    }
    seen->erase(marker);

    return inferArrayLiteralType(elementTypes);
}

std::string inferMapRuntimeType(const MapPtr& map,
                                int depth,
                                std::unordered_set<const HeapObject*>* seen) {
    if (map == nullptr) {
        return "Map<String,Any>";
    }

    const HeapObject* marker = static_cast<const HeapObject*>(map);
    if (seen->find(marker) != seen->end()) {
        return "Map<String,Any>";
    }

    seen->insert(marker);
    std::vector<std::string> valueTypes;
    valueTypes.reserve(map->entries.size());
    for (const auto& entry : map->entries) {
        valueTypes.push_back(runtimeTypeNameInternal(entry.second, depth, seen));
    }
    seen->erase(marker);

    return inferMapLiteralType(valueTypes);
}

} // namespace

std::string normalizeTypeAnnotation(const std::string& type) {
    std::string normalized = trimSpaces(type);
    if (normalized.empty()) {
        return "Any";
    }
    return normalized;
}

bool isAnyTypeAnnotation(const std::string& type) {
    return normalizeTypeAnnotation(type) == "Any";
}

bool isConcreteTypeAnnotation(const std::string& type) {
    std::string normalized = normalizeTypeAnnotation(type);
    return !normalized.empty() && normalized != "Any";
}

bool areTypesCompatible(const std::string& expected, const std::string& actual) {
    std::string normalizedExpected = normalizeTypeAnnotation(expected);
    std::string normalizedActual = normalizeTypeAnnotation(actual);

    if (normalizedExpected == "Any" || normalizedActual == "Any") {
        return true;
    }

    if (normalizedExpected == normalizedActual) {
        return true;
    }

    TypeExpr expectedExpr;
    TypeExpr actualExpr;
    if (tryParseTypeExpr(normalizedExpected, &expectedExpr) &&
        tryParseTypeExpr(normalizedActual, &actualExpr)) {
        return stringifyTypeExpr(expectedExpr) == stringifyTypeExpr(actualExpr);
    }

    return false;
}

std::string applyTypeBindings(
    const std::string& type,
    const std::unordered_map<std::string, std::string>& bindings) {
    TypeExpr expr;
    if (!tryParseTypeExpr(type, &expr)) {
        return normalizeTypeAnnotation(type);
    }

    return stringifyTypeExpr(applyBindingsToExpr(expr, bindings));
}

bool inferTypeBindings(const std::vector<std::string>& genericParameters,
                       const std::vector<std::string>& parameterTypes,
                       const std::vector<std::string>& argumentTypes,
                       std::unordered_map<std::string, std::string>* bindings) {
    if (bindings == nullptr) {
        return false;
    }

    bindings->clear();
    std::size_t count = std::min(parameterTypes.size(), argumentTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        std::string parameterType = normalizeTypeAnnotation(parameterTypes[index]);
        std::string argumentType = normalizeTypeAnnotation(argumentTypes[index]);
        if (parameterType == "Any" || argumentType == "Any") {
            continue;
        }

        TypeExpr expected;
        TypeExpr actual;
        if (!tryParseTypeExpr(parameterType, &expected) ||
            !tryParseTypeExpr(argumentType, &actual)) {
            if (isGenericParameter(genericParameters, parameterType)) {
                auto existing = bindings->find(parameterType);
                if (existing == bindings->end()) {
                    (*bindings)[parameterType] = argumentType;
                } else if (existing->second != argumentType) {
                    return false;
                }
            }
            continue;
        }

        if (!inferBindingsFromExpr(expected, actual, genericParameters, bindings)) {
            return false;
        }
    }

    return true;
}

std::string inferArrayLiteralType(const std::vector<std::string>& elementTypes) {
    return "Array<" + mergeInferredTypes(elementTypes) + ">";
}

std::string inferMapLiteralType(const std::vector<std::string>& valueTypes) {
    return "Map<String," + mergeInferredTypes(valueTypes) + ">";
}

std::string indexedAccessResultType(const std::string& receiverType) {
    std::string normalized = normalizeTypeAnnotation(receiverType);
    if (normalized == "String") {
        return "String";
    }

    TypeExpr expr;
    if (!tryParseTypeExpr(normalized, &expr)) {
        return "Any";
    }

    if (expr.name == "Array" && !expr.args.empty()) {
        return stringifyTypeExpr(expr.args[0]);
    }
    if (expr.name == "Map" && expr.args.size() >= 2) {
        return stringifyTypeExpr(expr.args[1]);
    }

    return "Any";
}

std::string propertyAccessResultType(const std::string& receiverType,
                                     const std::string& property) {
    std::string normalized = normalizeTypeAnnotation(receiverType);
    if ((normalized == "Array" || normalized == "String") && property == "length") {
        return "Number";
    }

    TypeExpr expr;
    if (!tryParseTypeExpr(normalized, &expr)) {
        return "Any";
    }

    if ((expr.name == "Array" || expr.name == "String") && property == "length") {
        return "Number";
    }

    if (expr.name == "Map" && expr.args.size() >= 2) {
        return stringifyTypeExpr(expr.args[1]);
    }

    return "Any";
}

std::string runtimeTypeName(const Value& value) {
    std::unordered_set<const HeapObject*> seen;
    return runtimeTypeNameInternal(value, 0, &seen);
}

bool valueMatchesTypeAnnotation(const Value& value, const std::string& expected) {
    TypeExpr expr;
    if (!tryParseTypeExpr(expected, &expr)) {
        return true;
    }

    return matchesTypeExpr(value, expr);
}
