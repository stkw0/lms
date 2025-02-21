/*
 * Copyright (C) 2023 Emeric Poupon
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

#include "responses/Artist.hpp"

#include "database/Artist.hpp"
#include "database/Release.hpp"
#include "database/TrackArtistLink.hpp"
#include "database/User.hpp"
#include "services/feedback/IFeedbackService.hpp"
#include "utils/Service.hpp"
#include "utils/String.hpp"

#include "SubsonicId.hpp"

namespace API::Subsonic
{
    using namespace Database;

    namespace Utils
    {
        std::string joinArtistNames(const std::vector<Artist::pointer>& artists)
        {
            if (artists.size() == 1)
                return artists.front()->getName();

            std::vector<std::string> names;
            names.resize(artists.size());

            std::transform(std::cbegin(artists), std::cend(artists), std::begin(names),
                [](const Artist::pointer& artist)
                {
                    return artist->getName();
                });

            return StringUtils::joinStrings(names, ", ");
        }

        std::string_view toString(TrackArtistLinkType type)
        {
            switch (type)
            {
            case TrackArtistLinkType::Arranger: return "arranger";
            case TrackArtistLinkType::Artist: return "artist";
            case TrackArtistLinkType::Composer: return "composer";
            case TrackArtistLinkType::Conductor: return "conductor";
            case TrackArtistLinkType::Lyricist: return "lyricist";
            case TrackArtistLinkType::Mixer: return "mixer";
            case TrackArtistLinkType::Performer: return "performer";
            case TrackArtistLinkType::Producer: return "producer";
            case TrackArtistLinkType::ReleaseArtist: return "albumartist";
            case TrackArtistLinkType::Remixer: return "remixer";
            case TrackArtistLinkType::Writer: return "writer";
            }

            return "unknown";
        }
    }

    Response::Node createArtistNode(RequestContext& context, const Artist::pointer& artist, const User::pointer& user, bool id3)
    {
        Response::Node artistNode{ createArtistNode(artist) };

        artistNode.setAttribute("id", idToString(artist->getId()));
        artistNode.setAttribute("name", artist->getName());
        artistNode.setAttribute("coverArt", idToString(artist->getId()));

        if (id3)
        {
            const std::size_t count{ Release::getCount(context.dbSession, Release::FindParameters {}.setArtist(artist->getId())) };
            artistNode.setAttribute("albumCount", count);
        }

        if (const Wt::WDateTime dateTime{ Service<Feedback::IFeedbackService>::get()->getStarredDateTime(user->getId(), artist->getId()) }; dateTime.isValid())
            artistNode.setAttribute("starred", StringUtils::toISO8601String(dateTime));

        // OpenSubsonic specific fields (must always be set)
        if (context.enableOpenSubsonic)
        {
            if (!id3)
                artistNode.setAttribute("mediaType", "artist");

            {
                std::optional<UUID> mbid{ artist->getMBID() };
                artistNode.setAttribute("musicBrainzId", mbid ? mbid->getAsString() : "");
            }

            artistNode.setAttribute("sortName", artist->getSortName());

            // roles
            Response::Node roles;
            artistNode.createEmptyArrayValue("roles");
            for (const TrackArtistLinkType linkType : TrackArtistLink::findUsedTypes(context.dbSession, artist->getId()))
                artistNode.addArrayValue("roles", Utils::toString(linkType));
        }

        return artistNode;
    }

    Response::Node createArtistNode(const Artist::pointer& artist)
    {
        Response::Node artistNode;

        artistNode.setAttribute("id", idToString(artist->getId()));
        artistNode.setAttribute("name", artist->getName());

        return artistNode;
    }
}