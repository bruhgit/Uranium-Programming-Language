#include "value.h"
#include "object.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace {

std::string valueToStringInternal(const Value& value,
                                  std::unordered_set<const ArrayObject*>* seenArrays,
                                  std::unordered_set<const MapObject*>* seenMaps);

std::string arrayToString(const ArrayPtr& array,
                          std::unordered_set<const ArrayObject*>* seenArrays,
                          std::unordered_set<const MapObject*>* seenMaps) {
    if (array == nullptr) {
        return "[]";
    }

    if (!seenArrays->insert(array).second) {
        return "[...]";
    }

    std::ostringstream stream;
    stream << "[";
    for (std::size_t index = 0; index < array->elements.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }

        stream << valueToStringInternal(array->elements[index], seenArrays, seenMaps);
    }
    stream << "]";

    seenArrays->erase(array);
    return stream.str();
}

std::string mapToString(const MapPtr& map,
                        std::unordered_set<const ArrayObject*>* seenArrays,
                        std::unordered_set<const MapObject*>* seenMaps) {
    if (map == nullptr) {
        return "{}";
    }

    if (!seenMaps->insert(map).second) {
        return "{...}";
    }

    std::vector<std::string> keys;
    keys.reserve(map->entries.size());
    for (const auto& pair : map->entries) {
        keys.push_back(pair.first);
    }
    std::sort(keys.begin(), keys.end());

    std::ostringstream stream;
    stream << "{";
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index > 0) {
            stream << ", ";
        }

        const std::string& key = keys[index];
        stream << key << ": "
               << valueToStringInternal(map->entries.at(key), seenArrays, seenMaps);
    }
    stream << "}";

    seenMaps->erase(map);
    return stream.str();
}

std::string classToString(const ClassPtr& klass) {
    if (klass == nullptr || klass->name.empty()) {
        return "<class>";
    }

    return "<class " + klass->name + ">";
}

std::string instanceToString(const InstancePtr& instance) {
    if (instance == nullptr || instance->klass == nullptr || instance->klass->name.empty()) {
        return "<instance>";
    }

    return "<" + instance->klass->name + " instance>";
}

std::string boundMethodToString(const BoundMethodPtr& boundMethod) {
    if (boundMethod == nullptr || boundMethod->method == nullptr) {
        return "<bound method>";
    }

    if (boundMethod->method->function == nullptr ||
        boundMethod->method->function->name.empty()) {
        return "<bound method>";
    }

    return "<bound method " + boundMethod->method->function->name + ">";
}

std::string closureToString(const ClosurePtr& closure) {
    if (closure == nullptr || closure->function == nullptr || closure->function->name.empty()) {
        return "<fn>";
    }

    return "<fn " + closure->function->name + ">";
}

std::string valueToStringInternal(const Value& value,
                                  std::unordered_set<const ArrayObject*>* seenArrays,
                                  std::unordered_set<const MapObject*>* seenMaps) {
    if (value.isNil()) {
        return "nil";
    }

    if (value.isUnset()) {
        return "<unset>";
    }

    if (value.isBool()) {
        return value.asBool() ? "true" : "false";
    }

    if (value.isNumber()) {
        std::ostringstream stream;
        stream << value.asNumber();
        return stream.str();
    }

    if (value.isInt()) {
        std::ostringstream stream;
        stream << value.asInt();
        return stream.str();
    }

    if (value.isFunction()) {
        FunctionPtr function = value.asFunction();
        if (function == nullptr || function->name.empty()) {
            return "<script>";
        }
        return "<fn " + function->name + ">";
    }

    if (value.isClosure()) {
        return closureToString(value.asClosure());
    }

    if (value.isNativeFunction()) {
        NativeFunctionPtr nativeFunction = value.asNativeFunction();
        if (nativeFunction == nullptr || nativeFunction->name.empty()) {
            return "<native fn>";
        }
        return "<native fn " + nativeFunction->name + ">";
    }

    if (value.isArray()) {
        return arrayToString(value.asArray(), seenArrays, seenMaps);
    }

    if (value.isMap()) {
        return mapToString(value.asMap(), seenArrays, seenMaps);
    }

    if (value.isClass()) {
        return classToString(value.asClass());
    }

    if (value.isInstance()) {
        return instanceToString(value.asInstance());
    }

    if (value.isBoundMethod()) {
        return boundMethodToString(value.asBoundMethod());
    }

    if (value.isTask()) {
        return "<task>";
    }

    return value.asString();
}

} // namespace

bool isFalsey(const Value& value) {
    return value.isNil() || (value.isBool() && !value.asBool());
}

bool valuesEqual(const Value& left, const Value& right) {
    if (left.type != right.type) {
        return false;
    }

    switch (left.type) {
        case VAL_UNSET:
            return true;
        case VAL_NIL:
            return true;
        case VAL_BOOL:
            return left.asBool() == right.asBool();
        case VAL_NUMBER:
            return left.asNumber() == right.asNumber();
        case VAL_INT:
            return left.asInt() == right.asInt();
        case VAL_STRING:
            return left.asString() == right.asString();
        case VAL_FUNCTION:
            return left.asFunction() == right.asFunction();
        case VAL_CLOSURE:
            return left.asClosure() == right.asClosure();
        case VAL_NATIVE_FUNCTION:
            return left.asNativeFunction() == right.asNativeFunction();
        case VAL_ARRAY:
            return left.asArray() == right.asArray();
        case VAL_MAP:
            return left.asMap() == right.asMap();
        case VAL_CLASS:
            return left.asClass() == right.asClass();
        case VAL_INSTANCE:
            return left.asInstance() == right.asInstance();
        case VAL_BOUND_METHOD:
            return left.asBoundMethod() == right.asBoundMethod();
        case VAL_TASK:
            return left.asTask() == right.asTask();
        default:
            return false;
    }
}

std::string valueToString(const Value& value) {
    std::unordered_set<const ArrayObject*> seenArrays;
    std::unordered_set<const MapObject*> seenMaps;
    return valueToStringInternal(value, &seenArrays, &seenMaps);
}

void printValue(const Value& value) {
    std::cout << valueToString(value);
}
