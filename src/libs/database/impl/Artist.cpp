/*
 * Copyright (C) 2015 Emeric Poupon
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
#include "database/Artist.hpp"

#include <Wt/Dbo/WtSqlTraits.h>

#include "database/Cluster.hpp"
#include "database/Release.hpp"
#include "database/Session.hpp"
#include "database/Track.hpp"
#include "database/User.hpp"
#include "utils/ILogger.hpp"
#include "SqlQuery.hpp"
#include "Utils.hpp"
#include "EnumSetTraits.hpp"
#include "IdTypeTraits.hpp"

namespace Database
{
    namespace
    {
        template <typename ResultType>
        Wt::Dbo::Query<ResultType> createQuery(Session& session, std::string_view itemToSelect, const Artist::FindParameters& params)
        {
            session.checkReadTransaction();

            auto query{ session.getDboSession().query<ResultType>("SELECT DISTINCT " + std::string{ itemToSelect } + " FROM artist a") };
            if (params.sortMethod == ArtistSortMethod::LastWritten
                || params.writtenAfter.isValid()
                || params.linkType
                || params.track.isValid()
                || params.release.isValid()
                || params.clusters.size() == 1
                || params.mediaLibrary.isValid())
            {
                query.join("track t ON t.id = t_a_l.track_id");
                query.join("track_artist_link t_a_l ON t_a_l.artist_id = a.id");
            }

            if (params.linkType)
                query.where("t_a_l.type = ?").bind(*params.linkType);

            if (params.writtenAfter.isValid())
                query.where("t.file_last_write > ?").bind(params.writtenAfter);

            if (!params.keywords.empty())
            {
                std::vector<std::string> clauses;
                std::vector<std::string> sortClauses;

                for (std::string_view keyword : params.keywords)
                {
                    clauses.push_back("a.name LIKE ? ESCAPE '" ESCAPE_CHAR_STR "'");
                    query.bind("%" + Utils::escapeLikeKeyword(keyword) + "%");
                }

                for (std::string_view keyword : params.keywords)
                {
                    sortClauses.push_back("a.sort_name LIKE ? ESCAPE '" ESCAPE_CHAR_STR "'");
                    query.bind("%" + Utils::escapeLikeKeyword(keyword) + "%");
                }

                query.where("(" + StringUtils::joinStrings(clauses, " AND ") + ") OR (" + StringUtils::joinStrings(sortClauses, " AND ") + ")");
            }

            if (params.starringUser.isValid())
            {
                assert(params.feedbackBackend);
                query.join("starred_artist s_a ON s_a.artist_id = a.id")
                    .where("s_a.user_id = ?").bind(params.starringUser)
                    .where("s_a.backend = ?").bind(*params.feedbackBackend)
                    .where("s_a.sync_state <> ?").bind(SyncState::PendingRemove);
            }

            if (params.clusters.size() == 1)
            {
                query.join("track_cluster t_c ON t_c.track_id = t.id")
                    .where("t_c.cluster_id = ?").bind(params.clusters.front());
            }
            else if (params.clusters.size() > 1)
            {
                std::ostringstream oss;
                oss << "a.id IN (SELECT DISTINCT a.id FROM artist a"
                    " INNER JOIN track t ON t.id = t_a_l.track_id"
                    " INNER JOIN track_artist_link t_a_l ON t_a_l.artist_id = a.id"
                    " INNER JOIN cluster c ON c.id = t_c.cluster_id"
                    " INNER JOIN track_cluster t_c ON t_c.track_id = t.id";

                WhereClause clusterClause;
                for (const ClusterId clusterId : params.clusters)
                {
                    clusterClause.Or(WhereClause("c.id = ?"));
                    query.bind(clusterId);
                }

                oss << " " << clusterClause.get();
                oss << " GROUP BY t.id,a.id HAVING COUNT(DISTINCT c.id) = " << params.clusters.size() << ")";

                query.where(oss.str());
            }

            if (params.track.isValid())
                query.where("t.id = ?").bind(params.track);

            if (params.release.isValid())
                query.where("t.release_id = ?").bind(params.release);

            if (params.mediaLibrary.isValid())
                query.where("t.media_library_id = ?").bind(params.mediaLibrary);

            switch (params.sortMethod)
            {
            case ArtistSortMethod::None:
                break;
            case ArtistSortMethod::ByName:
                query.orderBy("a.name COLLATE NOCASE");
                break;
            case ArtistSortMethod::BySortName:
                query.orderBy("a.sort_name COLLATE NOCASE");
                break;
            case ArtistSortMethod::Random:
                query.orderBy("RANDOM()");
                break;
            case ArtistSortMethod::LastWritten:
                query.orderBy("t.file_last_write DESC");
                break;
            case ArtistSortMethod::StarredDateDesc:
                assert(params.starringUser.isValid());
                query.orderBy("s_a.date_time DESC");
                break;
            }

            return query;
        }

        template <typename ResultType>
        Wt::Dbo::Query<ResultType> createQuery(Session& session, const Artist::FindParameters& params)
        {
            std::string_view itemToSelect;
            
            if constexpr (std::is_same_v<ResultType, ArtistId>)
                itemToSelect = "a.id";
            else if constexpr (std::is_same_v<ResultType, Wt::Dbo::ptr<Artist>>)
                itemToSelect = "a";
            else
                static_assert("Unhandled type");

            return createQuery<ResultType>(session, itemToSelect, params);
        }
    }

    Artist::Artist(const std::string& name, const std::optional<UUID>& MBID)
        : _name{ std::string(name, 0 , _maxNameLength) },
        _sortName{ _name },
        _MBID{ MBID ? MBID->getAsString() : "" }
    {
    }

    Artist::pointer Artist::create(Session& session, const std::string& name, const std::optional<UUID>& MBID)
    {
        return session.getDboSession().add(std::unique_ptr<Artist> {new Artist{ name, MBID }});
    }

    std::size_t Artist::getCount(Session& session)
    {
        session.checkReadTransaction();

        return session.getDboSession().query<int>("SELECT COUNT(*) FROM artist");
    }

    std::vector<Artist::pointer> Artist::find(Session& session, std::string_view name)
    {
        session.checkReadTransaction();

        Wt::Dbo::collection<Wt::Dbo::ptr<Artist>> res = session.getDboSession().find<Artist>()
            .where("name = ?").bind(std::string{ name, 0, _maxNameLength })
            .orderBy("LENGTH(mbid) DESC"); // put mbid entries first

        return std::vector<Artist::pointer>(res.begin(), res.end());
    }

    Artist::pointer Artist::find(Session& session, const UUID& mbid)
    {
        session.checkReadTransaction();
        return session.getDboSession().find<Artist>().where("mbid = ?").bind(std::string{ mbid.getAsString() }).resultValue();
    }

    Artist::pointer Artist::find(Session& session, ArtistId id)
    {
        session.checkReadTransaction();
        return session.getDboSession().find<Artist>().where("id = ?").bind(id).resultValue();
    }

    bool Artist::exists(Session& session, ArtistId id)
    {
        session.checkReadTransaction();
        return session.getDboSession().query<int>("SELECT 1 FROM artist").where("id = ?").bind(id).resultValue() == 1;
    }


    RangeResults<ArtistId> Artist::findOrphanIds(Session& session, std::optional<Range> range)
    {
        session.checkReadTransaction();
        auto query{ session.getDboSession().query<ArtistId>("SELECT DISTINCT a.id FROM artist a WHERE NOT EXISTS(SELECT 1 FROM track t INNER JOIN track_artist_link t_a_l ON t_a_l.artist_id = a.id WHERE t.id = t_a_l.track_id)") };
        return Utils::execQuery<ArtistId>(query, range);
    }

    RangeResults<ArtistId> Artist::findIds(Session& session, const FindParameters& params)
    {
        session.checkReadTransaction();

        auto query{ createQuery<ArtistId>(session, params) };
        return Utils::execQuery<ArtistId>(query, params.range);
    }

    RangeResults<Artist::pointer> Artist::find(Session& session, const FindParameters& params)
    {
        session.checkReadTransaction();

        auto query{ createQuery<Wt::Dbo::ptr<Artist>>(session, params) };
        return Utils::execQuery<Artist::pointer>(query, params.range);
    }

    void Artist::find(Session& session, const FindParameters& params, std::function<void(const pointer&)> func)
    {
        session.checkReadTransaction();

        auto query{ createQuery<Wt::Dbo::ptr<Artist>>(session, params) };
        Utils::execQuery(query, params.range, func);
    }

    RangeResults<ArtistId> Artist::findSimilarArtistIds(EnumSet<TrackArtistLinkType> artistLinkTypes, std::optional<Range> range) const
    {
        assert(session());

        std::ostringstream oss;
        oss <<
            "SELECT a.id FROM artist a"
            " INNER JOIN track_artist_link t_a_l ON t_a_l.artist_id = a.id"
            " INNER JOIN track t ON t.id = t_a_l.track_id"
            " INNER JOIN track_cluster t_c ON t_c.track_id = t.id"
            " WHERE "
            " t_c.cluster_id IN (SELECT DISTINCT c.id from cluster c"
            " INNER JOIN track t ON c.id = t_c.cluster_id"
            " INNER JOIN track_cluster t_c ON t_c.track_id = t.id"
            " INNER JOIN artist a ON a.id = t_a_l.artist_id"
            " INNER JOIN track_artist_link t_a_l ON t_a_l.track_id = t.id"
            " WHERE a.id = ?)"
            " AND a.id <> ?";

        if (!artistLinkTypes.empty())
        {
            oss << " AND t_a_l.type IN (";

            bool first{ true };
            for (TrackArtistLinkType type : artistLinkTypes)
            {
                (void)type;
                if (!first)
                    oss << ", ";
                oss << "?";
                first = false;
            }
            oss << ")";
        }

        auto query{ session()->query<ArtistId>(oss.str())
            .bind(getId())
            .bind(getId())
            .groupBy("a.id")
            .orderBy("COUNT(*) DESC, RANDOM()") };

        for (TrackArtistLinkType type : artistLinkTypes)
            query.bind(type);

        return Utils::execQuery<ArtistId>(query, range);
    }

    std::vector<std::vector<Cluster::pointer>> Artist::getClusterGroups(std::vector<ClusterTypeId> clusterTypeIds, std::size_t size) const
    {
        assert(session());

        WhereClause where;

        std::ostringstream oss;
        oss << "SELECT c FROM cluster c INNER JOIN track t ON c.id = t_c.cluster_id INNER JOIN track_cluster t_c ON t_c.track_id = t.id INNER JOIN cluster_type c_type ON c.cluster_type_id = c_type.id INNER JOIN artist a ON t_a_l.artist_id = a.id INNER JOIN track_artist_link t_a_l ON t_a_l.track_id = t.id";

        where.And(WhereClause("a.id = ?")).bind(getId().toString());
        {
            WhereClause clusterClause;
            for (ClusterTypeId clusterTypeId : clusterTypeIds)
                clusterClause.Or(WhereClause("c_type.id = ?")).bind(clusterTypeId.toString());

            where.And(clusterClause);
        }
        oss << " " << where.get();
        oss << "GROUP BY c.id ORDER BY COUNT(DISTINCT c.id) DESC";

        Wt::Dbo::Query<Wt::Dbo::ptr<Cluster>> query = session()->query<Wt::Dbo::ptr<Cluster>>(oss.str());

        for (const std::string& bindArg : where.getBindArgs())
            query.bind(bindArg);

        Wt::Dbo::collection<Wt::Dbo::ptr<Cluster>> queryRes = query;

        std::map<ClusterTypeId, std::vector<Cluster::pointer>> clustersByType;
        for (Cluster::pointer cluster : queryRes)
        {
            if (clustersByType[cluster->getType()->getId()].size() < size)
                clustersByType[cluster->getType()->getId()].push_back(cluster);
        }

        std::vector<std::vector<Cluster::pointer>> res;
        for (const auto& [clusterTypeId, clusters] : clustersByType)
            res.push_back(clusters);

        return res;
    }

    void Artist::setSortName(const std::string& sortName)
    {
        _sortName = std::string(sortName, 0, _maxNameLength);
    }

} // namespace Database
