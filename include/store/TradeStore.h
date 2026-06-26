#pragma once

#include "infra/FutexSemaphore.h"
#include "matching/MatchingEngine.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace chronobook {

#ifndef CHRONOBOOK_USE_DYNAMIC_SQLITE

class SQLiteError : public std::runtime_error {
public:
    explicit SQLiteError(const std::string& msg) : std::runtime_error(msg) {}
};

class SQLiteConnection {
public:
    explicit SQLiteConnection(std::string path) : m_path(std::move(path)) {}
    const std::string& path() const noexcept { return m_path; }
    void exec(const std::string&) {}

private:
    std::string m_path;
};

class PreparedStatement {
public:
    PreparedStatement(SQLiteConnection&, std::string sql) : m_sql(std::move(sql)) {}
    void reset() {}
    const std::string& sql() const noexcept { return m_sql; }

private:
    std::string m_sql;
};

class StatementCache {
public:
    explicit StatementCache(SQLiteConnection& conn, size_t capacity = 16)
        : m_conn(conn), m_capacity(capacity) {}

    PreparedStatement& get(const std::string& sql) {
        auto hit = m_index.find(sql);
        if (hit != m_index.end()) {
            m_lru.splice(m_lru.begin(), m_lru, hit->second);
            return *hit->second->stmt;
        }
        if (m_lru.size() == m_capacity) {
            m_index.erase(m_lru.back().sql);
            m_lru.pop_back();
        }
        m_lru.push_front(Entry{sql, std::make_unique<PreparedStatement>(m_conn, sql)});
        m_index[sql] = m_lru.begin();
        return *m_lru.begin()->stmt;
    }

    size_t size() const noexcept { return m_lru.size(); }

private:
    struct Entry {
        std::string sql;
        std::unique_ptr<PreparedStatement> stmt;
    };

    SQLiteConnection& m_conn;
    size_t m_capacity{16};
    std::list<Entry> m_lru;
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index;
};

class Transaction {
public:
    explicit Transaction(SQLiteConnection&) {}
    void commit() { m_active = false; }
    bool active() const noexcept { return m_active; }

private:
    bool m_active{true};
};

class TradeStore {
public:
    explicit TradeStore(std::string path) : m_path(std::move(path)) {
        load();
    }

    static void init(SQLiteConnection&) {}

    void insertBatch(const std::vector<Fill>& fills, bool failBeforeCommit = false) {
        std::vector<Fill> staged = m_trades;
        staged.insert(staged.end(), fills.begin(), fills.end());
        if (failBeforeCommit) throw SQLiteError("injected failure before commit");

        std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
        if (!out) throw SQLiteError("open trade store failed: " + m_path);
        const uint64_t count = static_cast<uint64_t>(staged.size());
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(staged.data()),
                  static_cast<std::streamsize>(staged.size() * sizeof(Fill)));
        if (!out) throw SQLiteError("write trade store failed: " + m_path);
        m_trades.swap(staged);
    }

    uint64_t tradeCount() const noexcept {
        return static_cast<uint64_t>(m_trades.size());
    }

    double vwap(uint64_t beginTs, uint64_t endTs) const noexcept {
        long double notional = 0.0;
        uint64_t qty = 0;
        for (const auto& f : m_trades) {
            if (f.timestamp < beginTs || f.timestamp > endTs) continue;
            notional += static_cast<long double>(f.price) * f.qty;
            qty += f.qty;
        }
        return qty == 0 ? 0.0 : static_cast<double>(notional / qty);
    }

    std::string explainPlan() const {
        return "SEARCH trades USING INDEX idx_trades_ts (portable fallback)\n";
    }

    const std::string& path() const noexcept { return m_path; }

private:
    void load() {
        std::ifstream in(m_path, std::ios::binary);
        if (!in) return;
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in) return;
        m_trades.resize(static_cast<size_t>(count));
        in.read(reinterpret_cast<char*>(m_trades.data()),
                static_cast<std::streamsize>(m_trades.size() * sizeof(Fill)));
        if (!in) throw SQLiteError("truncated trade store: " + m_path);
    }

    std::string m_path;
    std::vector<Fill> m_trades;
};

class ConnectionPool;

class ConnectionLease {
public:
    ConnectionLease() = default;
    ConnectionLease(ConnectionPool* pool, SQLiteConnection* conn)
        : m_pool(pool), m_conn(conn) {}
    ~ConnectionLease();

    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;

    ConnectionLease(ConnectionLease&& other) noexcept
        : m_pool(other.m_pool), m_conn(other.m_conn) {
        other.m_pool = nullptr;
        other.m_conn = nullptr;
    }

    ConnectionLease& operator=(ConnectionLease&& other) noexcept {
        if (this != &other) {
            reset();
            m_pool = other.m_pool;
            m_conn = other.m_conn;
            other.m_pool = nullptr;
            other.m_conn = nullptr;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return m_conn != nullptr; }
    SQLiteConnection& connection() { return *m_conn; }
    void reset();

private:
    ConnectionPool* m_pool{nullptr};
    SQLiteConnection* m_conn{nullptr};
};

class ConnectionPool {
public:
    ConnectionPool(const std::string& path, size_t size) : m_sem(size) {
        if (size == 0) throw std::invalid_argument("connection pool size must be > 0");
        for (size_t i = 0; i < size; ++i) {
            auto conn = std::make_unique<SQLiteConnection>(path);
            m_available.push(conn.get());
            m_all.push_back(std::move(conn));
        }
    }

    ConnectionLease acquire() {
        m_sem.acquire();
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* conn = m_available.front();
        m_available.pop();
        return ConnectionLease(this, conn);
    }

    template <typename Rep, typename Period>
    ConnectionLease tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
        if (!m_sem.tryAcquireFor(timeout)) return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* conn = m_available.front();
        m_available.pop();
        return ConnectionLease(this, conn);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_available.size();
    }

private:
    friend class ConnectionLease;

    void release(SQLiteConnection* conn) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_available.push(conn);
        }
        m_sem.release();
    }

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<SQLiteConnection>> m_all;
    std::queue<SQLiteConnection*> m_available;
    FutexSemaphore m_sem;
};

inline ConnectionLease::~ConnectionLease() { reset(); }

inline void ConnectionLease::reset() {
    if (m_pool && m_conn) {
        m_pool->release(m_conn);
        m_pool = nullptr;
        m_conn = nullptr;
    }
}

#else

struct sqlite3;
struct sqlite3_stmt;

class SQLiteError : public std::runtime_error {
public:
    explicit SQLiteError(const std::string& msg) : std::runtime_error(msg) {}
};

class SQLiteApi {
public:
    using OpenV2 = int (*)(const char*, sqlite3**, int, const char*);
    using Close = int (*)(sqlite3*);
    using Errmsg = const char* (*)(sqlite3*);
    using Exec = int (*)(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
    using Free = void (*)(void*);
    using PrepareV2 = int (*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    using Finalize = int (*)(sqlite3_stmt*);
    using Step = int (*)(sqlite3_stmt*);
    using Reset = int (*)(sqlite3_stmt*);
    using ClearBindings = int (*)(sqlite3_stmt*);
    using BindInt = int (*)(sqlite3_stmt*, int, int);
    using BindInt64 = int (*)(sqlite3_stmt*, int, long long);
    using BindText = int (*)(sqlite3_stmt*, int, const char*, int, void (*)(void*));
    using ColumnInt64 = long long (*)(sqlite3_stmt*, int);
    using ColumnDouble = double (*)(sqlite3_stmt*, int);
    using ColumnText = const unsigned char* (*)(sqlite3_stmt*, int);

    static SQLiteApi& instance() {
        static SQLiteApi api;
        return api;
    }

    OpenV2 open_v2{};
    Close close{};
    Errmsg errmsg{};
    Exec exec{};
    Free free_fn{};
    PrepareV2 prepare_v2{};
    Finalize finalize{};
    Step step{};
    Reset reset{};
    ClearBindings clear_bindings{};
    BindInt bind_int{};
    BindInt64 bind_int64{};
    BindText bind_text{};
    ColumnInt64 column_int64{};
    ColumnDouble column_double{};
    ColumnText column_text{};

private:
    SQLiteApi() { load(); }
    SQLiteApi(const SQLiteApi&) = delete;
    SQLiteApi& operator=(const SQLiteApi&) = delete;

    void* symbol(const char* name) {
#ifdef _WIN32
        auto* p = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
        auto* p = dlsym(m_handle, name);
#endif
        if (!p) throw SQLiteError(std::string("missing SQLite symbol: ") + name);
        return p;
    }

    void load() {
#ifdef _WIN32
        m_handle = LoadLibraryA("sqlite3.dll");
        if (!m_handle) throw SQLiteError("sqlite3.dll not found");
#else
        m_handle = dlopen("libsqlite3.so", RTLD_NOW);
        if (!m_handle) m_handle = dlopen("libsqlite3.dylib", RTLD_NOW);
        if (!m_handle) throw SQLiteError("libsqlite3 not found");
#endif
        open_v2 = reinterpret_cast<OpenV2>(symbol("sqlite3_open_v2"));
        close = reinterpret_cast<Close>(symbol("sqlite3_close"));
        errmsg = reinterpret_cast<Errmsg>(symbol("sqlite3_errmsg"));
        exec = reinterpret_cast<Exec>(symbol("sqlite3_exec"));
        free_fn = reinterpret_cast<Free>(symbol("sqlite3_free"));
        prepare_v2 = reinterpret_cast<PrepareV2>(symbol("sqlite3_prepare_v2"));
        finalize = reinterpret_cast<Finalize>(symbol("sqlite3_finalize"));
        step = reinterpret_cast<Step>(symbol("sqlite3_step"));
        reset = reinterpret_cast<Reset>(symbol("sqlite3_reset"));
        clear_bindings = reinterpret_cast<ClearBindings>(symbol("sqlite3_clear_bindings"));
        bind_int = reinterpret_cast<BindInt>(symbol("sqlite3_bind_int"));
        bind_int64 = reinterpret_cast<BindInt64>(symbol("sqlite3_bind_int64"));
        bind_text = reinterpret_cast<BindText>(symbol("sqlite3_bind_text"));
        column_int64 = reinterpret_cast<ColumnInt64>(symbol("sqlite3_column_int64"));
        column_double = reinterpret_cast<ColumnDouble>(symbol("sqlite3_column_double"));
        column_text = reinterpret_cast<ColumnText>(symbol("sqlite3_column_text"));
    }

    void* m_handle{nullptr};
};

inline constexpr int kSqliteOk = 0;
inline constexpr int kSqliteRow = 100;
inline constexpr int kSqliteDone = 101;
inline constexpr int kSqliteOpenReadWrite = 0x00000002;
inline constexpr int kSqliteOpenCreate = 0x00000004;

class SQLiteConnection {
public:
    explicit SQLiteConnection(const std::string& path) {
        auto& api = SQLiteApi::instance();
        const int rc = api.open_v2(path.c_str(), &m_db,
                                   kSqliteOpenReadWrite | kSqliteOpenCreate,
                                   nullptr);
        if (rc != kSqliteOk || !m_db) throw SQLiteError("sqlite3_open_v2 failed");
    }

    ~SQLiteConnection() noexcept {
        if (m_db) SQLiteApi::instance().close(m_db);
    }

    SQLiteConnection(const SQLiteConnection&) = delete;
    SQLiteConnection& operator=(const SQLiteConnection&) = delete;

    SQLiteConnection(SQLiteConnection&& other) noexcept : m_db(other.m_db) {
        other.m_db = nullptr;
    }

    SQLiteConnection& operator=(SQLiteConnection&& other) noexcept {
        if (this != &other) {
            if (m_db) SQLiteApi::instance().close(m_db);
            m_db = other.m_db;
            other.m_db = nullptr;
        }
        return *this;
    }

    sqlite3* raw() const noexcept { return m_db; }

    void exec(const std::string& sql) {
        char* err = nullptr;
        auto& api = SQLiteApi::instance();
        const int rc = api.exec(m_db, sql.c_str(), nullptr, nullptr, &err);
        if (rc != kSqliteOk) {
            std::string msg = err ? err : api.errmsg(m_db);
            if (err) api.free_fn(err);
            throw SQLiteError(msg);
        }
    }

    std::string error() const {
        return SQLiteApi::instance().errmsg(m_db);
    }

private:
    sqlite3* m_db{nullptr};
};

class PreparedStatement {
public:
    PreparedStatement(SQLiteConnection& conn, const std::string& sql)
        : m_conn(&conn), m_sql(sql) {
        auto& api = SQLiteApi::instance();
        const int rc = api.prepare_v2(conn.raw(), sql.c_str(), -1, &m_stmt, nullptr);
        if (rc != kSqliteOk) throw SQLiteError("prepare failed: " + conn.error());
    }

    ~PreparedStatement() noexcept {
        if (m_stmt) SQLiteApi::instance().finalize(m_stmt);
    }

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;

    PreparedStatement(PreparedStatement&& other) noexcept
        : m_conn(other.m_conn), m_stmt(other.m_stmt), m_sql(std::move(other.m_sql)) {
        other.m_conn = nullptr;
        other.m_stmt = nullptr;
    }

    PreparedStatement& operator=(PreparedStatement&&) = delete;

    void reset() {
        auto& api = SQLiteApi::instance();
        api.reset(m_stmt);
        api.clear_bindings(m_stmt);
    }

    void bindInt(int index, int value) {
        check(SQLiteApi::instance().bind_int(m_stmt, index, value));
    }

    void bindInt64(int index, uint64_t value) {
        check(SQLiteApi::instance().bind_int64(m_stmt, index, static_cast<long long>(value)));
    }

    void bindText(int index, const std::string& value) {
        check(SQLiteApi::instance().bind_text(m_stmt, index, value.c_str(),
                                              static_cast<int>(value.size()), nullptr));
    }

    bool stepRow() {
        const int rc = SQLiteApi::instance().step(m_stmt);
        if (rc == kSqliteRow) return true;
        if (rc == kSqliteDone) return false;
        throw SQLiteError("step failed: " + m_conn->error());
    }

    void stepDone() {
        const int rc = SQLiteApi::instance().step(m_stmt);
        if (rc != kSqliteDone) throw SQLiteError("stepDone failed: " + m_conn->error());
    }

    uint64_t columnUInt64(int index) const {
        return static_cast<uint64_t>(SQLiteApi::instance().column_int64(m_stmt, index));
    }

    double columnDouble(int index) const {
        return SQLiteApi::instance().column_double(m_stmt, index);
    }

    std::string columnText(int index) const {
        const unsigned char* text = SQLiteApi::instance().column_text(m_stmt, index);
        return text ? reinterpret_cast<const char*>(text) : "";
    }

private:
    void check(int rc) {
        if (rc != kSqliteOk) throw SQLiteError("bind failed: " + m_conn->error());
    }

    SQLiteConnection* m_conn{nullptr};
    sqlite3_stmt* m_stmt{nullptr};
    std::string m_sql;
};

class StatementCache {
public:
    explicit StatementCache(SQLiteConnection& conn, size_t capacity = 16)
        : m_conn(conn), m_capacity(capacity) {}

    PreparedStatement& get(const std::string& sql) {
        auto hit = m_index.find(sql);
        if (hit != m_index.end()) {
            m_lru.splice(m_lru.begin(), m_lru, hit->second);
            hit->second->stmt->reset();
            return *hit->second->stmt;
        }
        if (m_lru.size() == m_capacity) {
            m_index.erase(m_lru.back().sql);
            m_lru.pop_back();
        }
        m_lru.push_front(Entry{sql, std::make_unique<PreparedStatement>(m_conn, sql)});
        m_index[sql] = m_lru.begin();
        return *m_lru.begin()->stmt;
    }

    size_t size() const noexcept { return m_lru.size(); }

private:
    struct Entry {
        std::string sql;
        std::unique_ptr<PreparedStatement> stmt;
    };

    SQLiteConnection& m_conn;
    size_t m_capacity{16};
    std::list<Entry> m_lru;
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index;
};

class Transaction {
public:
    explicit Transaction(SQLiteConnection& conn) : m_conn(conn) {
        m_conn.exec("BEGIN IMMEDIATE;");
    }

    ~Transaction() noexcept {
        if (m_active) {
            try { m_conn.exec("ROLLBACK;"); } catch (...) {}
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit() {
        m_conn.exec("COMMIT;");
        m_active = false;
    }

private:
    SQLiteConnection& m_conn;
    bool m_active{true};
};

class TradeStore {
public:
    explicit TradeStore(const std::string& path)
        : m_path(path), m_writer(path), m_cache(m_writer, 16) {
        init(m_writer);
    }

    static void init(SQLiteConnection& conn) {
        conn.exec("PRAGMA journal_mode=WAL;");
        conn.exec("PRAGMA synchronous=NORMAL;");
        conn.exec("CREATE TABLE IF NOT EXISTS trades ("
                  "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                  "buy_order_id INTEGER NOT NULL,"
                  "sell_order_id INTEGER NOT NULL,"
                  "price INTEGER NOT NULL,"
                  "qty INTEGER NOT NULL,"
                  "ts INTEGER NOT NULL);");
        conn.exec("CREATE INDEX IF NOT EXISTS idx_trades_ts ON trades(ts);");
    }

    void insertBatch(const std::vector<Fill>& fills, bool failBeforeCommit = false) {
        Transaction txn(m_writer);
        auto& stmt = m_cache.get(
            "INSERT INTO trades(buy_order_id,sell_order_id,price,qty,ts) VALUES(?,?,?,?,?);");
        for (const auto& f : fills) {
            stmt.reset();
            stmt.bindInt64(1, f.buyOrderId);
            stmt.bindInt64(2, f.sellOrderId);
            stmt.bindInt(3, static_cast<int>(f.price));
            stmt.bindInt(4, static_cast<int>(f.qty));
            stmt.bindInt64(5, f.timestamp);
            stmt.stepDone();
        }
        if (failBeforeCommit) throw SQLiteError("injected failure before commit");
        txn.commit();
    }

    uint64_t tradeCount() {
        auto& stmt = m_cache.get("SELECT COUNT(*) FROM trades;");
        return stmt.stepRow() ? stmt.columnUInt64(0) : 0;
    }

    double vwap(uint64_t beginTs, uint64_t endTs) {
        auto& stmt = m_cache.get(
            "SELECT COALESCE(SUM(price * qty) * 1.0 / NULLIF(SUM(qty),0),0) "
            "FROM trades WHERE ts BETWEEN ? AND ?;");
        stmt.bindInt64(1, beginTs);
        stmt.bindInt64(2, endTs);
        return stmt.stepRow() ? stmt.columnDouble(0) : 0.0;
    }

    std::string explainPlan() {
        auto& stmt = m_cache.get(
            "EXPLAIN QUERY PLAN SELECT * FROM trades WHERE ts BETWEEN ? AND ?;");
        stmt.bindInt64(1, 0);
        stmt.bindInt64(2, UINT64_MAX);
        std::string out;
        while (stmt.stepRow()) {
            out += stmt.columnText(3);
            out += '\n';
        }
        return out;
    }

    const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    SQLiteConnection m_writer;
    StatementCache m_cache;
};

class ConnectionPool;

class ConnectionLease {
public:
    ConnectionLease() = default;
    ConnectionLease(ConnectionPool* pool, SQLiteConnection* conn)
        : m_pool(pool), m_conn(conn) {}
    ~ConnectionLease();

    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;

    ConnectionLease(ConnectionLease&& other) noexcept
        : m_pool(other.m_pool), m_conn(other.m_conn) {
        other.m_pool = nullptr;
        other.m_conn = nullptr;
    }

    ConnectionLease& operator=(ConnectionLease&& other) noexcept {
        if (this != &other) {
            reset();
            m_pool = other.m_pool;
            m_conn = other.m_conn;
            other.m_pool = nullptr;
            other.m_conn = nullptr;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return m_conn != nullptr; }
    SQLiteConnection& connection() { return *m_conn; }
    void reset();

private:
    ConnectionPool* m_pool{nullptr};
    SQLiteConnection* m_conn{nullptr};
};

class ConnectionPool {
public:
    ConnectionPool(const std::string& path, size_t size) : m_sem(size) {
        if (size == 0) throw std::invalid_argument("connection pool size must be > 0");
        for (size_t i = 0; i < size; ++i) {
            auto conn = std::make_unique<SQLiteConnection>(path);
            TradeStore::init(*conn);
            m_available.push(conn.get());
            m_all.push_back(std::move(conn));
        }
    }

    ConnectionLease acquire() {
        m_sem.acquire();
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* conn = m_available.front();
        m_available.pop();
        return ConnectionLease(this, conn);
    }

    template <typename Rep, typename Period>
    ConnectionLease tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
        if (!m_sem.tryAcquireFor(timeout)) return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* conn = m_available.front();
        m_available.pop();
        return ConnectionLease(this, conn);
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_available.size();
    }

private:
    friend class ConnectionLease;

    void release(SQLiteConnection* conn) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_available.push(conn);
        }
        m_sem.release();
    }

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<SQLiteConnection>> m_all;
    std::queue<SQLiteConnection*> m_available;
    FutexSemaphore m_sem;
};

inline ConnectionLease::~ConnectionLease() { reset(); }

inline void ConnectionLease::reset() {
    if (m_pool && m_conn) {
        m_pool->release(m_conn);
        m_pool = nullptr;
        m_conn = nullptr;
    }
}

#endif

} // namespace chronobook
