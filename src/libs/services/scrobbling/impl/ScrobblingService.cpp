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

#include "ScrobblingService.hpp"

#include "database/Artist.hpp"
#include "database/Db.hpp"
#include "database/Listen.hpp"
#include "database/Release.hpp"
#include "database/Session.hpp"
#include "database/Track.hpp"
#include "database/User.hpp"
#include "utils/ILogger.hpp"

#include "internal/InternalBackend.hpp"
#include "listenbrainz/ListenBrainzBackend.hpp"

namespace Scrobbling
{
    using namespace Database;

    namespace
    {
        Database::Listen::StatsFindParameters convertToListenFindParameters(const ScrobblingService::FindParameters& params)
        {
            Database::Listen::StatsFindParameters listenFindParams;
            listenFindParams.setUser(params.user);
            listenFindParams.setClusters(params.clusters);
            listenFindParams.setRange(params.range);
            listenFindParams.setMediaLibrary(params.library);
            listenFindParams.setArtist(params.artist);

            return listenFindParams;
        }

        Database::Listen::ArtistStatsFindParameters convertToListenFindParameters(const ScrobblingService::ArtistFindParameters& params)
        {
            return Database::Listen::ArtistStatsFindParameters{ convertToListenFindParameters(static_cast<const ScrobblingService::FindParameters&>(params)), params.linkType };
        }
    }

    std::unique_ptr<IScrobblingService> createScrobblingService(boost::asio::io_context& ioContext, Db& db)
    {
        return std::make_unique<ScrobblingService>(ioContext, db);
    }

    ScrobblingService::ScrobblingService(boost::asio::io_context& ioContext, Db& db)
        : _db{ db }
    {
        LMS_LOG(SCROBBLING, INFO, "Starting service...");
        _scrobblingBackends.emplace(ScrobblingBackend::Internal, std::make_unique<InternalBackend>(_db));
        _scrobblingBackends.emplace(ScrobblingBackend::ListenBrainz, std::make_unique<ListenBrainz::ListenBrainzBackend>(ioContext, _db));
        LMS_LOG(SCROBBLING, INFO, "Service started!");
    }

    ScrobblingService::~ScrobblingService()
    {
        LMS_LOG(SCROBBLING, INFO, "Service stopped!");
    }

    void ScrobblingService::listenStarted(const Listen& listen)
    {
        if (std::optional<ScrobblingBackend> backend{ getUserBackend(listen.userId) })
            _scrobblingBackends[*backend]->listenStarted(listen);
    }

    void ScrobblingService::listenFinished(const Listen& listen, std::optional<std::chrono::seconds> duration)
    {
        if (std::optional<ScrobblingBackend> backend{ getUserBackend(listen.userId) })
            _scrobblingBackends[*backend]->listenFinished(listen, duration);
    }

    void ScrobblingService::addTimedListen(const TimedListen& listen)
    {
        if (std::optional<ScrobblingBackend> backend{ getUserBackend(listen.userId) })
            _scrobblingBackends[*backend]->addTimedListen(listen);
    }

    std::optional<ScrobblingBackend> ScrobblingService::getUserBackend(UserId userId)
    {
        std::optional<ScrobblingBackend> backend;

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };
        if (const User::pointer user{ User::find(session, userId) })
            backend = user->getScrobblingBackend();

        return backend;
    }

    ScrobblingService::ArtistContainer ScrobblingService::getRecentArtists(const ArtistFindParameters& params)
    {
        ArtistContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::ArtistStatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getRecentArtists(session, listenFindParams);
        return res;
    }

    ScrobblingService::ReleaseContainer ScrobblingService::getRecentReleases(const FindParameters& params)
    {
        ReleaseContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::StatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getRecentReleases(session, listenFindParams);
        return res;
    }

    ScrobblingService::TrackContainer ScrobblingService::getRecentTracks(const FindParameters& params)
    {
        TrackContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::StatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getRecentTracks(session, listenFindParams);
        return res;
    }

    std::size_t ScrobblingService::getCount(Database::UserId userId, Database::ReleaseId releaseId)
    {
        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };
        return Database::Listen::getCount(session, userId, releaseId);
    }

    std::size_t ScrobblingService::getCount(Database::UserId userId, Database::TrackId trackId)
    {
        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };
        return Database::Listen::getCount(session, userId, trackId);
    }

    Wt::WDateTime ScrobblingService::getLastListenDateTime(Database::UserId userId, Database::ReleaseId releaseId)
    {
        const auto backend{ getUserBackend(userId) };
        if (!backend)
            return {};

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        const Database::Listen::pointer listen{ Database::Listen::getMostRecentListen(session, userId, *backend, releaseId) };
        return listen ? listen->getDateTime() : Wt::WDateTime{};
    }

    Wt::WDateTime ScrobblingService::getLastListenDateTime(Database::UserId userId, Database::TrackId trackId)
    {
        const auto backend{ getUserBackend(userId) };
        if (!backend)
            return {};

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        const Database::Listen::pointer listen{ Database::Listen::getMostRecentListen(session, userId, *backend, trackId) };
        return listen ? listen->getDateTime() : Wt::WDateTime{};
    }

    // Top
    ScrobblingService::ArtistContainer ScrobblingService::getTopArtists(const ArtistFindParameters& params)
    {
        ArtistContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::ArtistStatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getTopArtists(session, listenFindParams);
        return res;
    }

    ScrobblingService::ReleaseContainer ScrobblingService::getTopReleases(const FindParameters& params)
    {
        ReleaseContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::StatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getTopReleases(session, listenFindParams);
        return res;
    }

    ScrobblingService::TrackContainer ScrobblingService::getTopTracks(const FindParameters& params)
    {
        TrackContainer res;

        const auto backend{ getUserBackend(params.user) };
        if (!backend)
            return res;

        Database::Listen::StatsFindParameters listenFindParams{ convertToListenFindParameters(params) };
        listenFindParams.setScrobblingBackend(backend);

        Session& session{ _db.getTLSSession() };
        auto transaction{ session.createReadTransaction() };

        res = Database::Listen::getTopTracks(session, listenFindParams);
        return res;
    }
} // ns Scrobbling

