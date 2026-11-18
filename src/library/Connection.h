#pragma once

#include "util/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

namespace devildj::library
{

// One prepared SQLite statement. Owned by its Connection's statement cache, so it is prepared once and reused.
// Parameter and column indices follow SQLite: bind() is 1-based, get*() is 0-based.
class Statement
{
public:
    Statement(sqlite3_stmt* stmt, sqlite3* db) noexcept;
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value) { return bind(index, static_cast<std::int64_t>(value)); }
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view utf8);
    Statement& bind(int index, const char* utf8) { return bind(index, std::string_view(utf8)); }
    Statement& bindNull(int index);
    template <typename T>
    Statement& bind(int index, const std::optional<T>& value)
    {
        return value ? bind(index, *value) : bindNull(index);
    }

    // Steps once. True when a row is available; false when done or on an error (see ok()).
    bool step();
    // Runs to completion (for INSERT / UPDATE / DELETE), then resets. False on an error; the error stays
    // (error()) until reset() or the next Connection::prepare() of the same SQL.
    bool run();
    // Rewinds and clears the bindings, ready for the next use.
    void reset();

    bool ok() const noexcept { return lastError.empty(); }
    const std::string& error() const noexcept { return lastError; }

    bool isNull(int column) const;
    std::int64_t getInt(int column) const;
    double getDouble(int column) const;
    std::string getText(int column) const;
    std::optional<std::int64_t> getOptionalInt(int column) const;
    std::optional<double> getOptionalDouble(int column) const;

private:
    void noteBindError(int rc);

    sqlite3_stmt* stmt;
    sqlite3* db;
    std::string lastError;
};

// A SQLite connection used by one thread at a time. Writers come from library::Database (its writer thread);
// readers from Database::openReader(), one per reading thread.
class Connection
{
public:
    enum class Mode
    {
        ReadWrite, // creates the file if missing
        ReadOnly,
    };

    static util::Result<std::unique_ptr<Connection>> open(const std::filesystem::path& file, Mode mode);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Runs one or more statements without results. False on an error (see lastError()).
    bool exec(std::string_view sql);

    // A cached prepared statement, reset and ready to bind. nullptr on an SQL error (see lastError()).
    // Step it to the end or reset() it when done: a half-read SELECT keeps its read snapshot open.
    Statement* prepare(const std::string& sql);
    std::size_t cachedStatementCount() const noexcept { return cache.size(); }

    // Runs body inside BEGIN IMMEDIATE ... COMMIT; rolls back when body returns false or a statement fails.
    template <typename Body>
    bool transaction(Body&& body)
    {
        if (!exec("BEGIN IMMEDIATE"))
            return false;
        if (!body())
        {
            const auto why = lastErrorText;
            exec("ROLLBACK");
            lastErrorText = why.empty() ? std::string("transaction rolled back") : why;
            return false;
        }
        return exec("COMMIT");
    }

    // Single-value helpers (empty optional when there is no row or on an error).
    std::optional<std::int64_t> queryInt(const std::string& sql);
    std::optional<std::string> queryText(const std::string& sql);

    std::int64_t lastInsertId() const noexcept;
    int changes() const noexcept;
    const std::string& lastError() const noexcept { return lastErrorText; }
    void setError(std::string message) { lastErrorText = std::move(message); }
    sqlite3* handle() const noexcept { return db; }

private:
    explicit Connection(sqlite3* db) noexcept;

    sqlite3* db;
    std::unordered_map<std::string, std::unique_ptr<Statement>> cache;
    std::string lastErrorText;
};

} // namespace devildj::library
