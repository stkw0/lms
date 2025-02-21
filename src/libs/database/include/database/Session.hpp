/*
 * Copyright (C) 2013 Emeric Poupon
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

#pragma once

#include <Wt/Dbo/Dbo.h>
#include <Wt/Dbo/SqlConnectionPool.h>

#include "utils/RecursiveSharedMutex.hpp"
#include "database/Object.hpp"
#include "database/TransactionChecker.hpp"

namespace Database
{
    class WriteTransaction
    {
    public:
        ~WriteTransaction();
    private:
        friend class Session;
        WriteTransaction(RecursiveSharedMutex& mutex, Wt::Dbo::Session& session);

        WriteTransaction(const WriteTransaction&) = delete;
        WriteTransaction& operator=(const WriteTransaction&) = delete;

        std::unique_lock<RecursiveSharedMutex> _lock;
        Wt::Dbo::Transaction _transaction;
    };

    class ReadTransaction
    {
    public:
        ~ReadTransaction();
    private:
        friend class Session;
        ReadTransaction(Wt::Dbo::Session& session);


        ReadTransaction(const ReadTransaction&) = delete;
        ReadTransaction& operator=(const ReadTransaction&) = delete;

        Wt::Dbo::Transaction _transaction;
    };

    class Db;
    class Session
    {
    public:
        Session(Db& database);

        [[nodiscard]] WriteTransaction createWriteTransaction();
        [[nodiscard]] ReadTransaction createReadTransaction();

        void checkWriteTransaction() { TransactionChecker::checkWriteTransaction(_session); }
        void checkReadTransaction() { TransactionChecker::checkReadTransaction(_session); }

        void analyze();
        void optimize();

        void prepareTables(); // need to run only once at startup

        Wt::Dbo::Session& getDboSession() { return _session; }
        Db& getDb() { return _db; }

        template <typename Object, typename... Args>
        typename Object::pointer create(Args&&... args)
        {
            TransactionChecker::checkWriteTransaction(_session);

            typename Object::pointer res{ Object::create(*this, std::forward<Args>(args)...) };
            getDboSession().flush();

            if (res->hasOnPostCreated())
                res.modify()->onPostCreated();

            return res;
        }

    private:
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        Db& _db;
        Wt::Dbo::Session	_session;
    };
} // namespace Database
