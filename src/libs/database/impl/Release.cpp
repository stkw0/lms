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

#include "database/Release.hpp"

#include <Wt/Dbo/WtSqlTraits.h>

#include "database/Artist.hpp"
#include "database/Cluster.hpp"
#include "database/Session.hpp"
#include "database/Track.hpp"
#include "database/User.hpp"
#include "utils/ILogger.hpp"
#include "SqlQuery.hpp"
#include "EnumSetTraits.hpp"
#include "IdTypeTraits.hpp"
#include "StringViewTraits.hpp"
#include "Utils.hpp"

namespace Database
{
    namespace
    {
        template <typename ResultType>
        Wt::Dbo::Query<ResultType> createQuery(Session& session, std::string_view itemToSelect, const Release::FindParameters& params)
        {
            auto query{ session.getDboSession().query<ResultType>("SELECT " + std::string{ itemToSelect } + " from release r") };

            if (params.sortMethod == ReleaseSortMethod::ArtistNameThenName
                || params.sortMethod == ReleaseSortMethod::LastWritten
                || params.sortMethod == ReleaseSortMethod::Date
                || params.sortMethod == ReleaseSortMethod::OriginalDate
                || params.sortMethod == ReleaseSortMethod::OriginalDateDesc
                || params.writtenAfter.isValid()
                || params.dateRange
                || params.artist.isValid()
                || params.clusters.size() == 1
                || params.mediaLibrary.isValid())
            {
                query.join("track t ON t.release_id = r.id");
            }

            if (params.mediaLibrary.isValid())
                query.where("t.media_library_id = ?").bind(params.mediaLibrary);

            if (!params.releaseType.empty())
            {
                query.join("release_release_type r_r_t ON r_r_t.release_id = r.id");
                query.join("release_type r_t ON r_t.id = r_r_t.release_type_id")
                    .where("r_t.name = ?").bind(params.releaseType);
            }

            if (params.writtenAfter.isValid())
                query.where("t.file_last_write > ?").bind(params.writtenAfter);

            if (params.dateRange)
            {
                query.where("COALESCE(CAST(SUBSTR(t.date, 1, 4) AS INTEGER), t.year) >= ?").bind(params.dateRange->begin);
                query.where("COALESCE(CAST(SUBSTR(t.date, 1, 4) AS INTEGER), t.year) <= ?").bind(params.dateRange->end);
            }

            for (std::string_view keyword : params.keywords)
                query.where("r.name LIKE ? ESCAPE '" ESCAPE_CHAR_STR "'").bind("%" + Utils::escapeLikeKeyword(keyword) + "%");

            if (params.starringUser.isValid())
            {
                assert(params.feedbackBackend);
                query.join("starred_release s_r ON s_r.release_id = r.id")
                    .where("s_r.user_id = ?").bind(params.starringUser)
                    .where("s_r.backend = ?").bind(*params.feedbackBackend)
                    .where("s_r.sync_state <> ?").bind(SyncState::PendingRemove);
            }

            if (params.artist.isValid() 
                || params.sortMethod == ReleaseSortMethod::ArtistNameThenName)
            {
                query.join("artist a ON a.id = t_a_l.artist_id")
                    .join("track_artist_link t_a_l ON t_a_l.track_id = t.id")
                    .where("a.id = ?").bind(params.artist);

                if (!params.trackArtistLinkTypes.empty())
                {
                    std::ostringstream oss;

                    bool first{ true };
                    for (TrackArtistLinkType linkType : params.trackArtistLinkTypes)
                    {
                        if (!first)
                            oss << " OR ";
                        oss << "t_a_l.type = ?";
                        query.bind(linkType);

                        first = false;
                    }
                    query.where(oss.str());
                }

                if (!params.excludedTrackArtistLinkTypes.empty())
                {
                    std::ostringstream oss;
                    oss << "r.id NOT IN (SELECT DISTINCT r.id FROM release r"
                        " INNER JOIN artist a ON a.id = t_a_l.artist_id"
                        " INNER JOIN track_artist_link t_a_l ON t_a_l.track_id = t.id"
                        " INNER JOIN track t ON t.release_id = r.id"
                        " WHERE (a.id = ? AND (";

                    query.bind(params.artist);

                    bool first{ true };
                    for (const TrackArtistLinkType linkType : params.excludedTrackArtistLinkTypes)
                    {
                        if (!first)
                            oss << " OR ";
                        oss << "t_a_l.type = ?";
                        query.bind(linkType);

                        first = false;
                    }
                    oss << ")))";
                    query.where(oss.str());
                }
            }

            if (params.clusters.size() == 1)
            {
                query.join("track_cluster t_c ON t_c.track_id = t.id")
                    .where("t_c.cluster_id = ?").bind(params.clusters.front());
            }
            else if (params.clusters.size() > 1)
            {
                std::ostringstream oss;
                oss << "r.id IN (SELECT DISTINCT r.id FROM release r"
                    " INNER JOIN track t ON t.release_id = r.id"
                    " INNER JOIN track_cluster t_c ON t_c.track_id = t.id";

                WhereClause clusterClause;
                for (const ClusterId clusterId : params.clusters)
                {
                    clusterClause.Or(WhereClause("t_c.cluster_id = ?"));
                    query.bind(clusterId);
                }

                oss << " " << clusterClause.get();
                oss << " GROUP BY t.id HAVING COUNT(*) = " << params.clusters.size() << ")";

                query.where(oss.str());
            }

            switch (params.sortMethod)
            {
            case ReleaseSortMethod::None:
                break;
            case ReleaseSortMethod::Name:
                query.orderBy("r.name COLLATE NOCASE");
                break;
            case ReleaseSortMethod::ArtistNameThenName:
                query.orderBy("a.name COLLATE NOCASE, r.name COLLATE NOCASE");
                break;
            case ReleaseSortMethod::Random:
                query.orderBy("RANDOM()");
                break;
            case ReleaseSortMethod::LastWritten:
                query.orderBy("t.file_last_write DESC");
                break;
            case ReleaseSortMethod::Date:
                query.orderBy("COALESCE(t.date, CAST(t.year AS TEXT)), r.name COLLATE NOCASE");
                break;
            case ReleaseSortMethod::OriginalDate:
                query.orderBy("COALESCE(original_date, CAST(original_year AS TEXT), date, CAST(year AS TEXT)), r.name COLLATE NOCASE");
                break;
            case ReleaseSortMethod::OriginalDateDesc:
                query.orderBy("COALESCE(original_date, CAST(original_year AS TEXT), date, CAST(year AS TEXT)) DESC, r.name COLLATE NOCASE");
                break;
            case ReleaseSortMethod::StarredDateDesc:
                assert(params.starringUser.isValid());
                query.orderBy("s_r.date_time DESC");
                break;
            }

            return query;
        }
    }

    ReleaseType::ReleaseType(std::string_view name)
        : _name{ std::string(name, 0 , _maxNameLength) }
    {
    }

    ReleaseType::pointer ReleaseType::create(Session& session, std::string_view name)
    {
        return session.getDboSession().add(std::unique_ptr<ReleaseType> {new ReleaseType{ name }});
    }

    ReleaseType::pointer ReleaseType::find(Session& session, ReleaseTypeId id)
    {
        session.checkReadTransaction();

        return session.getDboSession()
            .find<ReleaseType>()
            .where("id = ?").bind(id)
            .resultValue();
    }

    ReleaseType::pointer ReleaseType::find(Session& session, std::string_view name)
    {
        session.checkReadTransaction();

        return session.getDboSession()
            .find<ReleaseType>()
            .where("name = ?").bind(name)
            .resultValue();
    }

    Release::Release(const std::string& name, const std::optional<UUID>& MBID)
        : _name{ std::string(name, 0 , _maxNameLength) },
        _MBID{ MBID ? MBID->getAsString() : "" }
    {
    }

    Release::pointer Release::create(Session& session, const std::string& name, const std::optional<UUID>& MBID)
    {
        return session.getDboSession().add(std::unique_ptr<Release> {new Release{ name, MBID }});
    }

    std::vector<Release::pointer> Release::find(Session& session, const std::string& name, const std::filesystem::path& releaseDirectory)
    {
        session.checkReadTransaction();

        auto res{ session.getDboSession()
                            .query<Wt::Dbo::ptr<Release>>("SELECT DISTINCT r from release r")
                            .join("track t ON t.release_id = r.id")
                            .where("r.name = ?").bind(std::string(name, 0, _maxNameLength))
                            .where("t.file_path LIKE ? ESCAPE '" ESCAPE_CHAR_STR "'").bind(Utils::escapeLikeKeyword(releaseDirectory.string()) + "%")
                            .resultList() };

        return std::vector<Release::pointer>(res.begin(), res.end());
    }

    Release::pointer Release::find(Session& session, const UUID& mbid)
    {
        session.checkReadTransaction();

        return session.getDboSession()
            .find<Release>()
            .where("mbid = ?").bind(std::string{ mbid.getAsString() })
            .resultValue();;
    }

    Release::pointer Release::find(Session& session, ReleaseId id)
    {
        session.checkReadTransaction();

        return session.getDboSession()
            .find<Release>()
            .where("id = ?").bind(id)
            .resultValue();
    }

    bool Release::exists(Session& session, ReleaseId id)
    {
        session.checkReadTransaction();
        return session.getDboSession().query<int>("SELECT 1 FROM release").where("id = ?").bind(id).resultValue() == 1;
    }

    std::size_t Release::getCount(Session& session)
    {
        session.checkReadTransaction();

        return session.getDboSession().query<int>("SELECT COUNT(*) FROM release");
    }

    RangeResults<ReleaseId> Release::findOrphanIds(Session& session, std::optional<Range> range)
    {
        session.checkReadTransaction();

        auto query{ session.getDboSession().query<ReleaseId>("select r.id from release r LEFT OUTER JOIN Track t ON r.id = t.release_id WHERE t.id IS NULL") };
        return Utils::execQuery<ReleaseId>(query, range);
    }

    RangeResults<Release::pointer> Release::find(Session& session, const FindParameters& params)
    {
        session.checkReadTransaction();

        auto query{ createQuery<Wt::Dbo::ptr<Release>>(session, "DISTINCT r", params) };
        return Utils::execQuery<pointer>(query, params.range);
    }

    void Release::find(Session& session, const FindParameters& params, std::function<void(const pointer&)> func)
    {
        session.checkReadTransaction();

        auto query{ createQuery<Wt::Dbo::ptr<Release>>(session, "DISTINCT r", params) };
        Utils::execQuery<pointer>(query, params.range, func);
    }

    RangeResults<ReleaseId> Release::findIds(Session& session, const FindParameters& params)
    {
        session.checkReadTransaction();

        auto query{ createQuery<ReleaseId>(session, "DISTINCT r.id", params) };
        return Utils::execQuery<ReleaseId>(query, params.range);
    }

    std::size_t Release::getCount(Session& session, const FindParameters& params)
    {
        session.checkReadTransaction();

        return createQuery<int>(session, "COUNT(DISTINCT r.id)", params).resultValue();
    }

    std::size_t Release::getDiscCount() const
    {
        assert(session());
        int res{ session()->query<int>("SELECT COUNT(DISTINCT disc_number) FROM track t")
            .join("release r ON r.id = t.release_id")
            .where("r.id = ?")
            .bind(getId()) };

        return res;
    }

    std::vector<DiscInfo> Release::getDiscs() const
    {
        assert(session());
        using ResultType = std::tuple<int, std::string>;
        auto results{ session()->query<ResultType>("SELECT DISTINCT disc_number, disc_subtitle FROM track t")
            .join("release r ON r.id = t.release_id")
            .where("r.id = ?")
            .orderBy("disc_number")
            .bind(getId())
            .resultList() };

        std::vector<DiscInfo> discs;
        for (const auto& res : results)
            discs.emplace_back(DiscInfo{ static_cast<std::size_t>(std::get<int>(res)), std::get<std::string>(res) });

        return discs;
    }

    Wt::WDate Release::getDate() const
    {
        return getDate(false);
    }

    Wt::WDate Release::getOriginalDate() const
    {
        return getDate(true);
    }

    Wt::WDate Release::getDate(bool original) const
    {
        assert(session());

        const char* field{ original ? "original_date" : "date" };

        auto dates{ session()->query<Wt::WDate>(
                std::string {"SELECT "} + "t." + field + " FROM track t INNER JOIN release r ON r.id = t.release_id")
            .where("r.id = ?")
            .groupBy(field)
            .bind(getId())
            .resultList() };

        // various dates => invalid date
        if (dates.empty() || dates.size() > 1)
            return {};

        return dates.front();
    }
    
    std::optional<int> Release::getYear() const
    {
        return getYear(false);
    }

    std::optional<int> Release::getOriginalYear() const
    {
        return getYear(true);
    }

    std::optional<int> Release::getYear(bool original) const
    {
        assert(session());

        const char* field{ original ? "original_year" : "year" };

        auto years{ session()->query<std::optional<int>>(
                std::string {"SELECT "} + "t." + field + " FROM track t INNER JOIN release r ON r.id = t.release_id")
            .where("r.id = ?").bind(getId())
            .groupBy(field)
            .resultList() };

        // various years => invalid years
        if (years.empty() || years.size() > 1)
            return std::nullopt;

        return years.front();
    }

    std::optional<std::string> Release::getCopyright() const
    {
        assert(session());

        Wt::Dbo::collection<std::string> copyrights = session()->query<std::string>
            ("SELECT copyright FROM track t INNER JOIN release r ON r.id = t.release_id")
            .where("r.id = ?")
            .groupBy("copyright")
            .bind(getId());

        std::vector<std::string> values(copyrights.begin(), copyrights.end());

        // various copyrights => no copyright
        if (values.empty() || values.size() > 1 || values.front().empty())
            return std::nullopt;

        return values.front();
    }

    std::optional<std::string> Release::getCopyrightURL() const
    {
        assert(session());

        Wt::Dbo::collection<std::string> copyrights = session()->query<std::string>
            ("SELECT copyright_url FROM track t INNER JOIN release r ON r.id = t.release_id")
            .where("r.id = ?").bind(getId())
            .groupBy("copyright_url");

        std::vector<std::string> values(copyrights.begin(), copyrights.end());

        // various copyright URLs => no copyright URL
        if (values.empty() || values.size() > 1 || values.front().empty())
            return std::nullopt;

        return values.front();
    }

    std::size_t Release::getMeanBitrate() const
    {
        assert(session());

        return session()->query<int>("SELECT COALESCE(AVG(t.bitrate), 0) FROM track t")
            .where("release_id = ?").bind(getId())
            .where("bitrate > 0")
            .resultValue();
    }

    std::vector<Artist::pointer> Release::getArtists(TrackArtistLinkType linkType) const
    {
        assert(session());

        auto res{ session()->query<Wt::Dbo::ptr<Artist>>(
                "SELECT DISTINCT a FROM artist a"
                " INNER JOIN track_artist_link t_a_l ON t_a_l.artist_id = a.id"
                " INNER JOIN track t ON t.id = t_a_l.track_id"
                " INNER JOIN release r ON r.id = t.release_id")
            .where("r.id = ?").bind(getId())
            .where("t_a_l.type = ?").bind(linkType)
            .resultList() };

        return std::vector<Artist::pointer>(res.begin(), res.end());
    }

    std::vector<Release::pointer> Release::getSimilarReleases(std::optional<std::size_t> offset, std::optional<std::size_t> count) const
    {
        assert(session());

        // Select the similar releases using the 5 most used clusters of the release
        auto res{ session()->query<Wt::Dbo::ptr<Release>>(
                "SELECT r FROM release r"
                " INNER JOIN track t ON t.release_id = r.id"
                " INNER JOIN track_cluster t_c ON t_c.track_id = t.id"
                    " WHERE "
                        " t_c.cluster_id IN "
                        "(SELECT DISTINCT c.id FROM cluster c"
                         " INNER JOIN track t ON c.id = t_c.cluster_id"
                         " INNER JOIN track_cluster t_c ON t_c.track_id = t.id"
                         " INNER JOIN release r ON r.id = t.release_id"
                         " WHERE r.id = ?)"
                        " AND r.id <> ?"
                    )
            .bind(getId())
            .bind(getId())
            .groupBy("r.id")
            .orderBy("COUNT(*) DESC, RANDOM()")
            .limit(count ? static_cast<int>(*count) : -1)
            .offset(offset ? static_cast<int>(*offset) : -1)
            .resultList() };

        return std::vector<pointer>(res.begin(), res.end());
    }

    void Release::clearReleaseTypes()
    {
        _releaseTypes.clear();
    }

    void Release::addReleaseType(ObjectPtr<ReleaseType> releaseType)
    {
        _releaseTypes.insert(getDboPtr(releaseType));
    }

    bool Release::hasVariousArtists() const
    {
        // TODO optimize
        return getArtists().size() > 1;
    }

    std::size_t Release::getTracksCount() const
    {
        return _tracks.size();
    }

    std::vector<ObjectPtr<ReleaseType>> Release::getReleaseTypes() const
    {
        return std::vector<ObjectPtr<ReleaseType>>(_releaseTypes.begin(), _releaseTypes.end());
    }

    std::vector<std::string> Release::getReleaseTypeNames() const
    {
        std::vector<std::string> res;

        for (const auto& releaseType : _releaseTypes)
            res.push_back(std::string{ releaseType->getName() });

        return res;
    }

    std::chrono::milliseconds Release::getDuration() const
    {
        assert(session());

        using milli = std::chrono::duration<int, std::milli>;

        Wt::Dbo::Query<milli> query{ session()->query<milli>("SELECT COALESCE(SUM(duration), 0) FROM track t INNER JOIN release r ON t.release_id = r.id")
                .where("r.id = ?").bind(getId()) };

        return query.resultValue();
    }

    Wt::WDateTime Release::getLastWritten() const
    {
        assert(session());

        Wt::Dbo::Query<Wt::WDateTime> query{ session()->query<Wt::WDateTime>("SELECT COALESCE(MAX(file_last_write), '1970-01-01T00:00:00') FROM track t INNER JOIN release r ON t.release_id = r.id")
                .where("r.id = ?").bind(getId()) };

        return query.resultValue();
    }

    std::vector<std::vector<Cluster::pointer>> Release::getClusterGroups(const std::vector<ClusterTypeId>& clusterTypeIds, std::size_t size) const
    {
        assert(session());

        WhereClause where;

        std::ostringstream oss;

        oss << "SELECT c from cluster c INNER JOIN track t ON c.id = t_c.cluster_id INNER JOIN track_cluster t_c ON t_c.track_id = t.id INNER JOIN cluster_type c_type ON c.cluster_type_id = c_type.id INNER JOIN release r ON t.release_id = r.id ";

        where.And(WhereClause("r.id = ?")).bind(getId().toString());
        {
            WhereClause clusterClause;
            for (const ClusterTypeId clusterTypeId : clusterTypeIds)
                clusterClause.Or(WhereClause("c_type.id = ?")).bind(clusterTypeId.toString());
            where.And(clusterClause);
        }
        oss << " " << where.get();
        oss << " GROUP BY c.id ORDER BY COUNT(c.id) DESC";

        auto query{ session()->query<Wt::Dbo::ptr<Cluster>>(oss.str()) };
        for (const std::string& bindArg : where.getBindArgs())
            query.bind(bindArg);

        auto queryRes{ query.resultList() };

        std::map<ClusterTypeId, std::vector<Cluster::pointer>> clustersByType;
        for (const Wt::Dbo::ptr<Cluster>& cluster : queryRes)
        {
            if (clustersByType[cluster->getType()->getId()].size() < size)
                clustersByType[cluster->getType()->getId()].push_back(cluster);
        }

        std::vector<std::vector<Cluster::pointer>> res;
        for (const auto& [clusterTypeId, clusters] : clustersByType)
            res.push_back(clusters);

        return res;
    }

} // namespace Database
