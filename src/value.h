#ifndef uranium_value_h
#define uranium_value_h

#include "common.h"
#include <string>
#include <utility>
#include <vector>

struct FunctionObject;
struct ClosureObject;
struct UpvalueObject;
struct NativeFunctionObject;
struct ArrayObject;
struct MapObject;
struct ClassObject;
struct InstanceObject;
struct BoundMethodObject;
struct TaskHandle;
using FunctionPtr = FunctionObject*;
using ClosurePtr = ClosureObject*;
using UpvaluePtr = UpvalueObject*;
using NativeFunctionPtr = NativeFunctionObject*;
using ArrayPtr = ArrayObject*;
using MapPtr = MapObject*;
using ClassPtr = ClassObject*;
using InstancePtr = InstanceObject*;
using BoundMethodPtr = BoundMethodObject*;
using TaskPtr = TaskHandle*;

enum ValueType {
    VAL_UNSET,
    VAL_NIL,
    VAL_BOOL,
    VAL_NUMBER,
    VAL_STRING,
    VAL_FUNCTION,
    VAL_CLOSURE,
    VAL_NATIVE_FUNCTION,
    VAL_ARRAY,
    VAL_MAP,
    VAL_CLASS,
    VAL_INSTANCE,
    VAL_BOUND_METHOD,
    VAL_TASK,
};

struct Value {
    ValueType type = VAL_NIL;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    FunctionPtr function = nullptr;
    ClosurePtr closure = nullptr;
    NativeFunctionPtr nativeFunction = nullptr;
    ArrayPtr array = nullptr;
    MapPtr map = nullptr;
    ClassPtr klass = nullptr;
    InstancePtr instance = nullptr;
    BoundMethodPtr boundMethod = nullptr;
    TaskPtr task = nullptr;

    Value() = default;
    
    explicit Value(bool boolValue) : type(VAL_BOOL), boolean(boolValue) {
    }

    explicit Value(double numberValue) : type(VAL_NUMBER), number(numberValue) {
    }

    explicit Value(std::string stringValue)
        : type(VAL_STRING), string(std::move(stringValue)) {
    }

    explicit Value(FunctionPtr functionValue)
        : type(VAL_FUNCTION), function(functionValue) {
    }

    explicit Value(ClosurePtr closureValue)
        : type(VAL_CLOSURE), closure(closureValue) {
    }

    explicit Value(NativeFunctionPtr nativeFunctionValue)
        : type(VAL_NATIVE_FUNCTION), nativeFunction(nativeFunctionValue) {
    }

    explicit Value(ArrayPtr arrayValue)
        : type(VAL_ARRAY), array(arrayValue) {
    }

    explicit Value(MapPtr mapValue)
        : type(VAL_MAP), map(mapValue) {
    }

    explicit Value(ClassPtr classValue)
        : type(VAL_CLASS), klass(classValue) {
    }

    explicit Value(InstancePtr instanceValue)
        : type(VAL_INSTANCE), instance(instanceValue) {
    }

    explicit Value(BoundMethodPtr boundMethodValue)
        : type(VAL_BOUND_METHOD), boundMethod(boundMethodValue) {
    }

    explicit Value(TaskPtr taskValue)
        : type(VAL_TASK), task(taskValue) {
    }

    static Value nilValue() {
        return Value();
    }

    static Value unsetValue() {
        Value value;
        value.type = VAL_UNSET;
        return value;
    }

    static Value numberValue(double number) {
        return Value(number);
    }

    static Value boolValue(bool boolean) {
        return Value(boolean);
    }

    static Value stringValue(std::string string) {
        return Value(std::move(string));
    }

    static Value functionValue(FunctionPtr function) {
        return Value(function);
    }

    static Value nativeFunctionValue(NativeFunctionPtr nativeFunction) {
        return Value(nativeFunction);
    }

    static Value closureValue(ClosurePtr closure) {
        return Value(closure);
    }

    static Value arrayValue(ArrayPtr array) {
        return Value(array);
    }

    static Value mapValue(MapPtr map) {
        return Value(map);
    }

    static Value classValue(ClassPtr klass) {
        return Value(klass);
    }

    static Value instanceValue(InstancePtr instance) {
        return Value(instance);
    }

    static Value boundMethodValue(BoundMethodPtr boundMethod) {
        return Value(boundMethod);
    }

    static Value taskValue(TaskPtr task) {
        return Value(task);
    }

    bool isNil() const {
        return type == VAL_NIL;
    }

    bool isUnset() const {
        return type == VAL_UNSET;
    }

    bool isNumber() const {
        return type == VAL_NUMBER;
    }

    bool isBool() const {
        return type == VAL_BOOL;
    }

    bool isString() const {
        return type == VAL_STRING;
    }

    bool isFunction() const {
        return type == VAL_FUNCTION;
    }

    bool isClosure() const {
        return type == VAL_CLOSURE;
    }

    bool isNativeFunction() const {
        return type == VAL_NATIVE_FUNCTION;
    }

    bool isArray() const {
        return type == VAL_ARRAY;
    }

    bool isMap() const {
        return type == VAL_MAP;
    }

    bool isClass() const {
        return type == VAL_CLASS;
    }

    bool isInstance() const {
        return type == VAL_INSTANCE;
    }

    bool isBoundMethod() const {
        return type == VAL_BOUND_METHOD;
    }

    bool isTask() const {
        return type == VAL_TASK;
    }

    double asNumber() const {
        return number;
    }

    bool asBool() const {
        return boolean;
    }

    const std::string& asString() const {
        return string;
    }

    FunctionPtr asFunction() const {
        return function;
    }

    ClosurePtr asClosure() const {
        return closure;
    }

    NativeFunctionPtr asNativeFunction() const {
        return nativeFunction;
    }

    ArrayPtr asArray() const {
        return array;
    }

    MapPtr asMap() const {
        return map;
    }

    ClassPtr asClass() const {
        return klass;
    }

    InstancePtr asInstance() const {
        return instance;
    }

    BoundMethodPtr asBoundMethod() const {
        return boundMethod;
    }

    TaskPtr asTask() const {
        return task;
    }
};

class ValueArray {
public:
    std::vector<Value> values;

    ValueArray() = default;
    ~ValueArray() = default;

    void write(const Value& value) {
        values.push_back(value);
    }
};

bool isFalsey(const Value& value);
bool valuesEqual(const Value& left, const Value& right);
void printValue(const Value& value);
std::string valueToString(const Value& value);

#endif
