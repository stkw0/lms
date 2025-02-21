/*
 * Copyright (C) 2019 Emeric Poupon
 *
 * This file is part of LMS.
 *
 * LMS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LMS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LMS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "database/Db.hpp"

#include <Wt/Dbo/FixedSqlConnectionPool.h>
#include <Wt/Dbo/backend/Sqlite3.h>

#include "database/Session.hpp"
#include "database/User.hpp"
#include "utils/IConfig.hpp"
#include "utils/Service.hpp"
#include "utils/ILogger.hpp"

namespace Database
{
    namespace
    {
        class Connection : public Wt::Dbo::backend::Sqlite3
        {
        public:
            Connection(const std::filesystem::path& dbPath)
                : Wt::Dbo::backend::Sqlite3{ dbPath.string() }
                , _dbPath{ dbPath }
            {
                prepare();
            }

            Connection(const Connection& other)
                : Wt::Dbo::backend::Sqlite3{ other }
                , _dbPath{ other._dbPath }
            {
                prepare();
            }

            ~Connection()
            {
                // make use of per-connection usage stats to optimize
                optimize();
            }

        private:
            Connection& operator=(const Connection&) = delete;

            std::unique_ptr<SqlConnection> clone() const override
            {
                return std::make_unique<Connection>(*this);
            }

            void prepare()
            {
                LMS_LOG(DB, DEBUG, "Setting per-connection settings...");
                executeSql("pragma journal_mode=WAL");
                executeSql("pragma synchronous=normal");
                executeSql("pragma analysis_limit=2000"); // to help make analyze command faster, 1000 does not seem to be enough to speed up all queries
                LMS_LOG(DB, DEBUG, "Setting per-connection settings done!");
            }

            void optimize()
            {
                LMS_LOG(DB, DEBUG, "connection close: Running pragma optimize...");
                executeSql("pragma optimize");
                LMS_LOG(DB, DEBUG, "connection close: pragma optimize complete");
            }

            std::filesystem::path _dbPath;
        };
    }

    // Session living class handling the database and the login
    Db::Db(const std::filesystem::path& dbPath, std::size_t connectionCount)
    {
        LMS_LOG(DB, INFO, "Creating connection pool on file " << dbPath.string());

        auto connection{ std::make_unique<Connection>(dbPath.string()) };
        if (IConfig * config{ Service<IConfig>::get() })// may not be here on testU
            connection->setProperty("show-queries", config->getBool("db-show-queries", false) ? "true" : "false");

        auto connectionPool{ std::make_unique<Wt::Dbo::FixedSqlConnectionPool>(std::move(connection), connectionCount) };
        connectionPool->setTimeout(std::chrono::seconds{ 10 });

        _connectionPool = std::move(connectionPool);
    }

    void Db::executeSql(const std::string& sql)
    {
        ScopedConnection connection{ *_connectionPool };
        connection->executeSql(sql);
    }

    Session& Db::getTLSSession()
    {
        static thread_local Session* tlsSession{};

        if (!tlsSession)
        {
            auto newSession{ std::make_unique<Session>(*this) };
            tlsSession = newSession.get();

            {
                std::scoped_lock lock{ _tlsSessionsMutex };
                _tlsSessions.push_back(std::move(newSession));
            }
        }

        return *tlsSession;
    }

    Db::ScopedConnection::ScopedConnection(Wt::Dbo::SqlConnectionPool& pool)
        : _connectionPool{ pool }
        , _connection{ _connectionPool.getConnection() }
    {
    }

    Db::ScopedConnection::~ScopedConnection()
    {
        _connectionPool.returnConnection(std::move(_connection));
    }

    Wt::Dbo::SqlConnection* Db::ScopedConnection::operator->() const
    {
        return _connection.get();
    }

} // namespace Database
