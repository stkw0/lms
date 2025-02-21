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

#include "ArtistCollector.hpp"

#include "database/Artist.hpp"
#include "database/Session.hpp"
#include "database/TrackList.hpp"
#include "database/User.hpp"
#include "services/feedback/IFeedbackService.hpp"
#include "services/scrobbling/IScrobblingService.hpp"
#include "utils/Service.hpp"
#include "Filters.hpp"
#include "LmsApplication.hpp"

namespace UserInterface
{
    using namespace Database;

    RangeResults<ArtistId> ArtistCollector::get(std::optional<Database::Range> requestedRange)
    {
        Feedback::IFeedbackService& feedbackService{ *Service<Feedback::IFeedbackService>::get() };
        Scrobbling::IScrobblingService& scrobblingService{ *Service<Scrobbling::IScrobblingService>::get() };

        const Range range{ getActualRange(requestedRange) };

        RangeResults<ArtistId> artists;

        switch (getMode())
        {
        case Mode::Random:
            artists = getRandomArtists(range);
            break;

        case Mode::Starred:
        {
            Feedback::IFeedbackService::ArtistFindParameters params;
            params.setUser(LmsApp->getUserId());
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setSortMethod(ArtistSortMethod::StarredDateDesc);
            params.setRange(range);
            artists = feedbackService.findStarredArtists(params);
            break;
        }

        case Mode::RecentlyPlayed:
        {
            Scrobbling::IScrobblingService::ArtistFindParameters params;
            params.setUser(LmsApp->getUserId());
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setRange(range);

            artists = scrobblingService.getRecentArtists(params);
            break;
        }

        case Mode::MostPlayed:
        {
            Scrobbling::IScrobblingService::ArtistFindParameters params;
            params.setUser(LmsApp->getUserId());
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setRange(range);

            artists = scrobblingService.getTopArtists(params);
            break;
        }

        case Mode::RecentlyAdded:
        {
            Artist::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setSortMethod(ArtistSortMethod::LastWritten);
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                artists = Artist::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }

        case Mode::Search:
        {
            Artist::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setKeywords(getSearchKeywords());
            params.setLinkType(_linkType);
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                artists = Artist::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }

        case Mode::All:
        {
            Artist::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setSortMethod(ArtistSortMethod::BySortName);
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                artists = Artist::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }
        }

        if (range.offset + range.size == getMaxCount())
            artists.moreResults = false;

        return artists;
    }

    RangeResults<Database::ArtistId> ArtistCollector::getRandomArtists(Range range)
    {
        assert(getMode() == Mode::Random);

        if (!_randomArtists)
        {
            Artist::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setLinkType(_linkType);
            params.setSortMethod(ArtistSortMethod::Random);
            params.setRange(Range{ 0, getMaxCount() });

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                _randomArtists = Artist::findIds(LmsApp->getDbSession(), params);
            }
        }

        return _randomArtists->getSubRange(range);
    }
} // ns UserInterface
