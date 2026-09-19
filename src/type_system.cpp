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
    bool isNullable = false;
    std::vector<TypeExpr> unionVariants;
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

bool parseTypeExpr(const std::string& text, std::size_t* cursor, TypeExpr* out);

bool parseSingleTypeExpr(const std::string& text, std::size_t* cursor, TypeExpr* out) {
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
    out->unionVariants.clear();
    out->isNullable = false;

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

    if (*cursor < text.size() && text[*cursor] == '?') {
        out->isNullable = true;
        (*cursor)++;
    }

    return true;
}

bool parseTypeExpr(const std::string& text, std::size_t* cursor, TypeExpr* out) {
    if (cursor == nullptr || out == nullptr || *cursor >= text.size()) {
        return false;
    }

    TypeExpr first;
    if (!parseSingleTypeExpr(text, cursor, &first)) {
        return false;
    }

    if (*cursor < text.size() && text[*cursor] == '|') {
        out->name.clear();
        out->args.clear();
        out->isNullable = false;
        out->unionVariants.clear();
        out->unionVariants.push_back(std::move(first));

        while (*cursor < text.size() && text[*cursor] == '|') {
            (*cursor)++;
            TypeExpr next;
            if (!parseSingleTypeExpr(text, cursor, &next)) {
                return false;
            }
            out->unionVariants.push_back(std::move(next));
        }
        return true;
    }

    *out = std::move(first);
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
    if (!expr.unionVariants.empty()) {
        std::string result;
        for (std::size_t i = 0; i < expr.unionVariants.size(); ++i) {
            if (i > 0) result += "|";
            result += stringifyTypeExpr(expr.unionVariants[i]);
        }
        if (expr.isNullable) result += "?";
        return result;
    }

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
    if (expr.isNullable) {
        result.push_back('?');
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

static std::unordered_map<std::string, std::vector<std::string>> g_subtypeHierarchy;

static bool isSubtypeOf(const std::string& subType, const std::string& superType) {
    if (subType == superType) return true;
    std::unordered_set<std::string> visited;
    std::vector<std::string> queue;
    queue.push_back(subType);
    visited.insert(subType);
    while (!queue.empty()) {
        std::string curr = queue.back();
        queue.pop_back();
        auto it = g_subtypeHierarchy.find(curr);
        if (it != g_subtypeHierarchy.end()) {
            for (const auto& parent : it->second) {
                if (parent == superType) return true;
                if (visited.insert(parent).second) {
                    queue.push_back(parent);
                }
            }
        }
    }
    return false;
}

bool matchesTypeExpr(const Value& value, const TypeExpr& expected) {
    if (expected.name.empty() && expected.unionVariants.empty()) {
        return true;
    }
    if (expected.name == "Any") {
        return true;
    }

    if (value.isNil()) {
        if (expected.name == "Nil" || expected.name == "nil" || expected.isNullable) {
            return true;
        }
        if (!expected.unionVariants.empty()) {
            for (const auto& variant : expected.unionVariants) {
                if (variant.name == "Nil" || variant.name == "nil" || variant.isNullable) {
                    return true;
                }
            }
        }
        return false;
    }

    if (!expected.unionVariants.empty()) {
        for (const auto& variant : expected.unionVariants) {
            if (matchesTypeExpr(value, variant)) {
                return true;
            }
        }
        return false;
    }

    if (expected.name == "Bool" || expected.name == "bool" || expected.name == "Boolean") {
        return value.isBool();
    }
    if (expected.name == "Number" || expected.name == "number") {
        return value.isNumber() || value.isInt();
    }
    if (expected.name == "int" || expected.name == "Int") {
        return value.isInt();
    }
    if (expected.name == "float" || expected.name == "Float") {
        return value.isNumber();
    }
    if (expected.name == "String" || expected.name == "string") {
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
        for (ClassPtr curr = value.asClass(); curr != nullptr; curr = curr->superclass) {
            if (isSubtypeOf(curr->name, expected.name)) {
                return true;
            }
        }
        return false;
    }
    if (value.isInstance() && value.asInstance() != nullptr &&
        value.asInstance()->klass != nullptr) {
        for (ClassPtr curr = value.asInstance()->klass; curr != nullptr; curr = curr->superclass) {
            if (isSubtypeOf(curr->name, expected.name)) {
                return true;
            }
        }
        return false;
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
        return "float";
    }
    if (value.isInt()) {
        return "int";
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

static bool isPrimitiveTypeMatch(const std::string& expected, const std::string& actual) {
    if (expected == actual) return true;
    if ((expected == "Int" || expected == "int") && (actual == "Int" || actual == "int")) return true;
    if ((expected == "Float" || expected == "float") && (actual == "Float" || actual == "float")) return true;
    if ((expected == "Bool" || expected == "Boolean" || expected == "bool") && 
        (actual == "Bool" || actual == "Boolean" || actual == "bool")) return true;
    if ((expected == "String" || expected == "string") && (actual == "String" || actual == "string")) return true;
    if ((expected == "Nil" || expected == "nil") && (actual == "Nil" || actual == "nil")) return true;
    if ((expected == "Number" || expected == "number") && 
        (actual == "int" || actual == "Int" || actual == "float" || actual == "Float" || actual == "number" || actual == "Number")) return true;
    if ((actual == "Number" || actual == "number") && 
        (expected == "int" || expected == "Int" || expected == "float" || expected == "Float" || expected == "number" || expected == "Number")) return true;
    return false;
}

static bool isTypeExprCompatible(const TypeExpr& expected, const TypeExpr& actual) {
    if (expected.name == "Any" || actual.name == "Any") {
        return true;
    }

    // If expected is a union:
    if (!expected.unionVariants.empty()) {
        if (!actual.unionVariants.empty()) {
            for (const auto& actualVariant : actual.unionVariants) {
                if (!isTypeExprCompatible(expected, actualVariant)) {
                    return false;
                }
            }
            return true;
        }
        for (const auto& expectedVariant : expected.unionVariants) {
            if (isTypeExprCompatible(expectedVariant, actual)) {
                return true;
            }
        }
        return false;
    }

    // If actual is a union: all variants of actual must be compatible with expected
    if (!actual.unionVariants.empty()) {
        for (const auto& actualVariant : actual.unionVariants) {
            if (!isTypeExprCompatible(expected, actualVariant)) {
                return false;
            }
        }
        return true;
    }

    // Nullability check:
    if (expected.isNullable) {
        if (actual.name == "Nil" || actual.name == "nil") {
            return true;
        }
        TypeExpr nonNullExpected = expected;
        nonNullExpected.isNullable = false;
        TypeExpr nonNullActual = actual;
        nonNullActual.isNullable = false;
        return isTypeExprCompatible(nonNullExpected, nonNullActual);
    }

    // Expected is NOT nullable, but actual is Nil -> INCOMPATIBLE (Null-Safety!)
    if ((actual.name == "Nil" || actual.name == "nil") && expected.name != "Nil" && expected.name != "nil") {
        return false;
    }

    // Actual is nullable, but expected is NOT nullable -> INCOMPATIBLE
    if (actual.isNullable && !expected.isNullable) {
        return false;
    }

    if (isPrimitiveTypeMatch(expected.name, actual.name)) {
        return true;
    }

    if (expected.name == actual.name) {
        if (expected.args.size() != actual.args.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.args.size(); ++i) {
            if (!isTypeExprCompatible(expected.args[i], actual.args[i])) {
                return false;
            }
        }
        return true;
    }

    if (expected.name == "Instance") {
        if (actual.name == "Instance") return true;
        if (actual.name != "Nil" && actual.name != "nil" &&
            actual.name != "int" && actual.name != "Int" &&
            actual.name != "float" && actual.name != "Float" &&
            actual.name != "number" && actual.name != "Number" &&
            actual.name != "Bool" && actual.name != "bool" && actual.name != "Boolean" &&
            actual.name != "String" && actual.name != "string" &&
            actual.name != "Array" && actual.name != "Map" &&
            actual.name != "Function" && actual.name != "Task") {
            return true;
        }
    }

    if (isSubtypeOf(actual.name, expected.name)) {
        return true;
    }

    return false;
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
        return isTypeExprCompatible(expectedExpr, actualExpr);
    }

    return normalizedExpected == normalizedActual;
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

void registerTypeSubtype(const std::string& subType, const std::string& superType) {
    g_subtypeHierarchy[subType].push_back(superType);
}

void clearTypeSubtypes() {
    g_subtypeHierarchy.clear();
}

