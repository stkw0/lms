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

#include "TrackCollector.hpp"

#include <algorithm>

#include "database/Session.hpp"
#include "database/Track.hpp"
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

    RangeResults<TrackId> TrackCollector::get(std::optional<Range> requestedRange)
    {
        Feedback::IFeedbackService& feedbackService{ *Service<Feedback::IFeedbackService>::get() };
        Scrobbling::IScrobblingService& scrobblingService{ *Service<Scrobbling::IScrobblingService>::get() };

        const Range range{ getActualRange(requestedRange) };

        RangeResults<TrackId> tracks;

        switch (getMode())
        {
        case Mode::Random:
            tracks = getRandomTracks(range);
            break;

        case Mode::Starred:
        {
            Feedback::IFeedbackService::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setRange(range);
            params.setUser(LmsApp->getUserId());
            tracks = feedbackService.findStarredTracks(params);
            break;
        }

        case TrackCollector::Mode::RecentlyPlayed:
        {
            Scrobbling::IScrobblingService::FindParameters params;
            params.setUser(LmsApp->getUserId());
            params.setClusters(getFilters().getClusterIds());
            params.setRange(range);

            tracks = scrobblingService.getRecentTracks(params);
            break;
        }

        case Mode::MostPlayed:
        {
            Scrobbling::IScrobblingService::FindParameters params;
            params.setUser(LmsApp->getUserId());
            params.setClusters(getFilters().getClusterIds());
            params.setRange(range);

            tracks = scrobblingService.getTopTracks(params);
            break;
        }

        case Mode::RecentlyAdded:
        {
            Track::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setSortMethod(TrackSortMethod::LastWritten);
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                tracks = Track::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }

        case Mode::Search:
        {
            Track::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setKeywords(getSearchKeywords());
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                tracks = Track::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }

        case Mode::All:
        {
            Track::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setRange(range);

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                tracks = Track::findIds(LmsApp->getDbSession(), params);
            }
            break;
        }
        }

        if (range.offset + range.size == getMaxCount())
            tracks.moreResults = false;

        return tracks;
    }

    RangeResults<TrackId> TrackCollector::getRandomTracks(Range range)
    {
        assert(getMode() == Mode::Random);

        if (!_randomTracks)
        {
            Track::FindParameters params;
            params.setClusters(getFilters().getClusterIds());
            params.setSortMethod(TrackSortMethod::Random);
            params.setRange(Range{ 0, getMaxCount() });

            {
                auto transaction{ LmsApp->getDbSession().createReadTransaction() };
                _randomTracks = Track::findIds(LmsApp->getDbSession(), params);
            }
        }

        return _randomTracks->getSubRange(range);
    }

} // ns UserInterface
