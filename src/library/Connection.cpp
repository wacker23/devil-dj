#include "Connection.h"

#include "util/RealtimeGuard.h"

#include <sqlite3.h>

namespace devildj::library
{

namespace
{
constexpr int kBusyTimeoutMs = 5000;

std::string describe(sqlite3* db, int rc)
{
    std::string text = db != nullptr ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    return text + " (SQLite " + std::to_string(rc) + ")";
}

// The audio thread must never touch the database (CLAUDE.md golden rules). Debug builds know which thread that is
// (RealtimeGuard) and refuse before SQLite runs; no error text is set, since that would allocate there.

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Statement

Statement::Statement(sqlite3_stmt* s, sqlite3* d) noexcept
    : stmt(s)
    , db(d)
{
}

Statement::~Statement()
{
    sqlite3_finalize(stmt);
}

void Statement::noteBindError(int rc)
{
    if (rc != SQLITE_OK && lastError.empty())
        lastError = describe(db, rc);
}

Statement& Statement::bind(int index, std::int64_t value)
{
    noteBindError(sqlite3_bind_int64(stmt, index, value));
    return *this;
}

Statement& Statement::bind(int index, double value)
{
    noteBindError(sqlite3_bind_double(stmt, index, value));
    return *this;
}

Statement& Statement::bind(int index, std::string_view utf8)
{
    noteBindError(sqlite3_bind_text64(stmt, index, utf8.data(), utf8.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    return *this;
}

Statement& Statement::bindNull(int index)
{
    noteBindError(sqlite3_bind_null(stmt, index));
    return *this;
}

bool Statement::step()
{
    if (!lastError.empty())
        return false;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc != SQLITE_DONE)
        lastError = describe(db, rc);
    return false;
}

bool Statement::run()
{
    while (step())
    {
    }
    const bool good = ok();
    if (good)
        reset();
    else
    {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }
    return good;
}

void Statement::reset()
{
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    lastError.clear();
}

bool Statement::isNull(int column) const
{
    return sqlite3_column_type(stmt, column) == SQLITE_NULL;
}

std::int64_t Statement::getInt(int column) const
{
    return sqlite3_column_int64(stmt, column);
}

double Statement::getDouble(int column) const
{
    return sqlite3_column_double(stmt, column);
}

std::string Statement::getText(int column) const
{
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    if (text == nullptr)
        return {};
    return std::string(text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, column)));
}

std::optional<std::int64_t> Statement::getOptionalInt(int column) const
{
    if (isNull(column))
        return std::nullopt;
    return getInt(column);
}

std::optional<double> Statement::getOptionalDouble(int column) const
{
    if (isNull(column))
        return std::nullopt;
    return getDouble(column);
}

// ---------------------------------------------------------------------------------------------------------------
// Connection

Connection::Connection(sqlite3* d) noexcept
    : db(d)
{
}

Connection::~Connection()
{
    cache.clear(); // finalize statements before closing
    sqlite3_close_v2(db);
}

util::Result<std::unique_ptr<Connection>> Connection::open(const std::filesystem::path& file, Mode mode)
{
    using R = util::Result<std::unique_ptr<Connection>>;
    const auto u8 = file.u8string(); // SQLite wants UTF-8 file names (non-ASCII user folders)
    const std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
    const int flags = (mode == Mode::ReadOnly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                      SQLITE_OPEN_NOMUTEX; // each connection stays on one thread at a time
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(name.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        auto message = "Can't open " + name + ": " + describe(raw, rc);
        sqlite3_close_v2(raw);
        return R::fail(std::move(message));
    }
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, kBusyTimeoutMs);
    return R::ok(std::unique_ptr<Connection>(new Connection(raw)));
}

bool Connection::exec(std::string_view sql)
{
    if (util::isInRealtimeContext())
        return false;
    const std::string text(sql);
    char* message = nullptr;
    const int rc = sqlite3_exec(db, text.c_str(), nullptr, nullptr, &message);
    if (rc == SQLITE_OK)
    {
        lastErrorText.clear();
        return true;
    }
    lastErrorText =
        message != nullptr ? std::string(message) + " (SQLite " + std::to_string(rc) + ")" : describe(db, rc);
    sqlite3_free(message);
    return false;
}

Statement* Connection::prepare(const std::string& sql)
{
    if (util::isInRealtimeContext())
        return nullptr;
    if (const auto found = cache.find(sql); found != cache.end())
    {
        found->second->reset();
        return found->second.get();
    }
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v3(db, sql.c_str(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &raw,
                                      nullptr);
    if (rc != SQLITE_OK || raw == nullptr)
    {
        lastErrorText = describe(db, rc);
        sqlite3_finalize(raw);
        return nullptr;
    }
    auto& slot = cache[sql];
    slot = std::make_unique<Statement>(raw, db);
    return slot.get();
}

std::optional<std::int64_t> Connection::queryInt(const std::string& sql)
{
    auto* s = prepare(sql);
    if (s == nullptr || !s->step() || s->isNull(0))
        return std::nullopt;
    const auto v = s->getInt(0);
    s->reset();
    return v;
}

std::optional<std::string> Connection::queryText(const std::string& sql)
{
    auto* s = prepare(sql);
    if (s == nullptr || !s->step() || s->isNull(0))
        return std::nullopt;
    auto v = s->getText(0);
    s->reset();
    return v;
}

std::int64_t Connection::lastInsertId() const noexcept
{
    return sqlite3_last_insert_rowid(db);
}

int Connection::changes() const noexcept
{
    return sqlite3_changes(db);
}

} // namespace devildj::library
