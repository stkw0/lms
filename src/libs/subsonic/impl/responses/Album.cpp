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

#include "responses/Album.hpp"

#include "database/Artist.hpp"
#include "database/Cluster.hpp"
#include "database/Release.hpp"
#include "database/User.hpp"
#include "services/feedback/IFeedbackService.hpp"
#include "services/scrobbling/IScrobblingService.hpp"
#include "utils/Service.hpp"
#include "utils/String.hpp"

#include "responses/Artist.hpp"
#include "responses/DiscTitle.hpp"
#include "responses/ItemDate.hpp"
#include "responses/ItemGenre.hpp"
#include "SubsonicId.hpp"

namespace API::Subsonic
{
    using namespace Database;

    Response::Node createAlbumNode(RequestContext& context, const Release::pointer& release, const User::pointer& user, bool id3)
    {
        Response::Node albumNode;

        if (id3) {
            albumNode.setAttribute("name", release->getName());
            albumNode.setAttribute("songCount", release->getTracksCount());
            albumNode.setAttribute(
                "duration", std::chrono::duration_cast<std::chrono::seconds>(
                    release->getDuration())
                .count());
        }
        else
        {
            albumNode.setAttribute("title", release->getName());
            albumNode.setAttribute("isDir", true);
        }

        albumNode.setAttribute("created", StringUtils::toISO8601String(release->getLastWritten()));
        albumNode.setAttribute("id", idToString(release->getId()));
        albumNode.setAttribute("coverArt", idToString(release->getId()));
        if (const auto year{ release->getYear() })
            albumNode.setAttribute("year", *year);

        auto artists{ release->getReleaseArtists() };
        if (artists.empty())
            artists = release->getArtists();

        if (artists.empty() && !id3)
        {
            albumNode.setAttribute("parent", idToString(RootId{}));
        }
        else if (!artists.empty())
        {
            if (!release->getArtistDisplayName().empty())
                albumNode.setAttribute("artist", release->getArtistDisplayName());
            else
                albumNode.setAttribute("artist", Utils::joinArtistNames(artists));

            if (artists.size() == 1)
            {
                albumNode.setAttribute(id3 ? Response::Node::Key{ "artistId" } : Response::Node::Key{ "parent" }, idToString(artists.front()->getId()));
            }
            else
            {
                if (!id3)
                    albumNode.setAttribute("parent", idToString(RootId{}));
            }
        }

        albumNode.setAttribute("playCount", Service<Scrobbling::IScrobblingService>::get()->getCount(user->getId(), release->getId()));

        // Report the first GENRE for this track
        const ClusterType::pointer genreClusterType{ ClusterType::find(context.dbSession, "GENRE") };
        if (genreClusterType)
        {
            auto clusters{ release->getClusterGroups({genreClusterType->getId()}, 1) };
            if (!clusters.empty() && !clusters.front().empty())
                albumNode.setAttribute("genre", clusters.front().front()->getName());
        }

        if (const Wt::WDateTime dateTime{ Service<Feedback::IFeedbackService>::get()->getStarredDateTime(user->getId(), release->getId()) }; dateTime.isValid())
            albumNode.setAttribute("starred", StringUtils::toISO8601String(dateTime));

        if (!context.enableOpenSubsonic)
            return albumNode;

        // OpenSubsonic specific fields (must always be set)
        albumNode.setAttribute("sortName", release->getSortName());

        if (!id3)
            albumNode.setAttribute("mediaType", "album");

        {
            const Wt::WDateTime dateTime{ Service<Scrobbling::IScrobblingService>::get()->getLastListenDateTime(user->getId(), release->getId()) };
            albumNode.setAttribute("played", dateTime.isValid() ? StringUtils::toISO8601String(dateTime) : std::string{ "" });
        }

        {
            std::optional<UUID> mbid{ release->getMBID() };
            albumNode.setAttribute("musicBrainzId", mbid ? mbid->getAsString() : "");
        }

        auto addClusters{ [&](Response::Node::Key field, std::string_view clusterTypeName)
        {
            albumNode.createEmptyArrayValue(field);

            Cluster::FindParameters params;
            params.setRelease(release->getId());
            params.setClusterTypeName(clusterTypeName);

            for (const auto& cluster : Cluster::find(context.dbSession, params).results)
                albumNode.addArrayValue(field, cluster->getName());
        } };

        addClusters("moods", "MOOD");

        // Genres
        albumNode.createEmptyArrayChild("genres");
        if (genreClusterType)
        {
            Cluster::FindParameters params;
            params.setRelease(release->getId());
            params.setClusterType(genreClusterType->getId());

            for (const auto& cluster : Cluster::find(context.dbSession, params).results)
                albumNode.addArrayChild("genres", createItemGenreNode(cluster->getName()));
        }

        albumNode.createEmptyArrayChild("artists");
        for (const Artist::pointer& artist : release->getReleaseArtists())
            albumNode.addArrayChild("artists", createArtistNode(artist));

        albumNode.setAttribute("displayArtist", release->getArtistDisplayName());
        albumNode.addChild("originalReleaseDate", createItemDateNode(release->getOriginalDate(), release->getOriginalYear()));

        {
            bool isCompilation{};
            albumNode.createEmptyArrayValue("releaseTypes");
            for (std::string_view releaseType : release->getReleaseTypeNames())
            {
                if (StringUtils::stringCaseInsensitiveEqual(releaseType, "compilation"))
                    isCompilation = true;

                albumNode.addArrayValue("releaseTypes", releaseType);
            }

            // TODO: the Compilation tag does not have the same meaning
            albumNode.setAttribute("isCompilation", isCompilation);
        }

        // disc titles
        albumNode.createEmptyArrayChild("discTitles");
        for (const DiscInfo& discInfo : release->getDiscs())
        {
            if (!discInfo.name.empty())
                albumNode.addArrayChild("discTitles", createDiscTitle(discInfo));
        }

        return albumNode;
    }
}