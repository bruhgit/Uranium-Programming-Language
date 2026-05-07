#include "db_native.h"
#include "vm.h"
#include "heap.h"
#include "sqlite3.h"
#include <vector>
#include <mutex>

namespace {
    std::vector<sqlite3*> g_databases;
    std::mutex g_dbMutex;
}

Value nativeDbOpen(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isString()) {
        *errorMessage = "dbOpen expects 1 string argument (file path).";
        return Value::nilValue();
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open(args[0].asString().c_str(), &db);
    if (rc != SQLITE_OK) {
        *errorMessage = "Failed to open database: " + std::string(sqlite3_errmsg(db));
        if (db) sqlite3_close(db);
        return Value::nilValue();
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    g_databases.push_back(db);
    return Value::numberValue(static_cast<double>(g_databases.size() - 1));
}

Value nativeDbExecute(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isNumber() || !args[1].isString()) {
        *errorMessage = "dbExecute expects 2 arguments (db id, sql string).";
        return Value::nilValue();
    }

    size_t id = static_cast<size_t>(args[0].asNumber());
    if (id >= g_databases.size() || g_databases[id] == nullptr) {
        *errorMessage = "Invalid database ID.";
        return Value::nilValue();
    }

    sqlite3* db = g_databases[id];
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, args[1].asString().c_str(), nullptr, 0, &errMsg);

    if (rc != SQLITE_OK) {
        *errorMessage = "DB Execute Error: " + std::string(errMsg);
        sqlite3_free(errMsg);
        return Value::boolValue(false);
    }

    return Value::boolValue(true);
}

Value nativeDbQuery(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 2 || !args[0].isNumber() || !args[1].isString()) {
        *errorMessage = "dbQuery expects 2 arguments (db id, sql string).";
        return Value::nilValue();
    }

    size_t id = static_cast<size_t>(args[0].asNumber());
    if (id >= g_databases.size() || g_databases[id] == nullptr) {
        *errorMessage = "Invalid database ID.";
        return Value::nilValue();
    }

    sqlite3* db = g_databases[id];
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, args[1].asString().c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        *errorMessage = "DB Query Prepare Error: " + std::string(sqlite3_errmsg(db));
        return Value::nilValue();
    }

    ArrayPtr resultArray = uraniumHeap().allocateArray();
    int cols = sqlite3_column_count(stmt);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MapPtr rowMap = uraniumHeap().allocateMap();

        for (int i = 0; i < cols; ++i) {
            std::string colName = sqlite3_column_name(stmt, i);
            int type = sqlite3_column_type(stmt, i);

            Value cellValue;
            if (type == SQLITE_INTEGER) {
                cellValue = Value::numberValue(static_cast<double>(sqlite3_column_int64(stmt, i)));
            } else if (type == SQLITE_FLOAT) {
                cellValue = Value::numberValue(sqlite3_column_double(stmt, i));
            } else if (type == SQLITE_TEXT) {
                const unsigned char* text = sqlite3_column_text(stmt, i);
                cellValue = Value::stringValue(std::string(reinterpret_cast<const char*>(text)));
            } else if (type == SQLITE_NULL) {
                cellValue = Value::nilValue();
            } else {
                cellValue = Value::stringValue("<blob>");
            }

            rowMap->entries[colName] = cellValue;
        }

        resultArray->elements.push_back(Value::mapValue(rowMap));
    }

    sqlite3_finalize(stmt);
    return Value::arrayValue(resultArray);
}

Value nativeDbClose(int argCount, const Value* args, std::string* errorMessage) {
    if (argCount != 1 || !args[0].isNumber()) {
        *errorMessage = "dbClose expects 1 argument (db id).";
        return Value::nilValue();
    }

    size_t id = static_cast<size_t>(args[0].asNumber());
    if (id >= g_databases.size() || g_databases[id] == nullptr) {
        return Value::boolValue(false);
    }

    sqlite3_close(g_databases[id]);
    g_databases[id] = nullptr;
    return Value::boolValue(true);
}
