/*
 * Copyright (C) 2021 Emeric Poupon
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

#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include "database/Artist.hpp"
#include "database/Cluster.hpp"
#include "database/Db.hpp"
#include "database/Listen.hpp"
#include "database/MediaLibrary.hpp"
#include "database/Release.hpp"
#include "database/ScanSettings.hpp"
#include "database/Session.hpp"
#include "database/Track.hpp"
#include "database/TrackArtistLink.hpp"
#include "database/TrackBookmark.hpp"
#include "database/TrackFeatures.hpp"
#include "database/TrackList.hpp"
#include "database/Types.hpp"
#include "database/User.hpp"

template <typename T>
class [[nodiscard]] ScopedEntity
{
public:
    using IdType = typename T::IdType;

    template <typename... Args>
    ScopedEntity(Database::Session& session, Args&& ...args)
        : _session{ session }
    {
        auto transaction{ _session.createWriteTransaction() };

        auto entity{ _session.create<T>(std::forward<Args>(args)...) };
        EXPECT_TRUE(entity);
        _id = entity->getId();
    }

    ~ScopedEntity()
    {
        auto transaction{ _session.createWriteTransaction() };

        auto entity{ T::find(_session, _id) };
        // could not be here due to "on delete cascade" constraints...
        if (entity)
            entity.remove();
    }

    ScopedEntity(const ScopedEntity&) = delete;
    ScopedEntity(ScopedEntity&&) = delete;
    ScopedEntity& operator=(const ScopedEntity&) = delete;
    ScopedEntity& operator=(ScopedEntity&&) = delete;

    typename T::pointer lockAndGet()
    {
        auto transaction{ _session.createReadTransaction() };
        return get();
    }

    typename T::pointer get()
    {
        _session.checkReadTransaction();

        auto entity{ T::find(_session, _id) };
        EXPECT_TRUE(entity);
        return entity;
    }

    typename T::pointer operator->()
    {
        return get();
    }

    IdType getId() const { return _id; }

private:
    Database::Session& _session;
    IdType _id{};
};

using ScopedArtist = ScopedEntity<Database::Artist>;
using ScopedCluster = ScopedEntity<Database::Cluster>;
using ScopedClusterType = ScopedEntity<Database::ClusterType>;
using ScopedMediaLibrary = ScopedEntity<Database::MediaLibrary>;
using ScopedRelease = ScopedEntity<Database::Release>;
using ScopedTrack = ScopedEntity<Database::Track>;
using ScopedTrackList = ScopedEntity<Database::TrackList>;
using ScopedUser = ScopedEntity<Database::User>;

class ScopedFileDeleter final
{
public:
    ScopedFileDeleter(const std::filesystem::path& path) : _path{ path } {}
    ~ScopedFileDeleter() { std::filesystem::remove(_path); }

private:
    ScopedFileDeleter(const ScopedFileDeleter&) = delete;
    ScopedFileDeleter(ScopedFileDeleter&&) = delete;
    ScopedFileDeleter operator=(const ScopedFileDeleter&) = delete;
    ScopedFileDeleter operator=(ScopedFileDeleter&&) = delete;

    const std::filesystem::path _path;
};

class TmpDatabase final
{
public:
    TmpDatabase();

    Database::Db& getDb();

private:
    const std::filesystem::path _tmpFile;
    ScopedFileDeleter _fileDeleter;
    Database::Db _db;
};

class DatabaseFixture : public ::testing::Test
{
public:
    ~DatabaseFixture();

public:
    static void SetUpTestCase();
    static void TearDownTestCase();

private:
    void testDatabaseEmpty();

    static inline std::unique_ptr<TmpDatabase> _tmpDb{};

public:
    Database::Session session{ _tmpDb->getDb() };
};

