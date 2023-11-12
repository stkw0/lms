/*
 * Copyright (C) 2020 Emeric Poupon
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

#include "MediaRetrieval.hpp"

#include "av/IAudioFile.hpp"
#include "av/RawResourceHandlerCreator.hpp"
#include "av/TranscodingParameters.hpp"
#include "av/TranscodingResourceHandlerCreator.hpp"
#include "av/Types.hpp"
#include "services/cover/ICoverService.hpp"
#include "services/database/Session.hpp"
#include "services/database/Track.hpp"
#include "services/database/User.hpp"
#include "utils/IResourceHandler.hpp"
#include "utils/Logger.hpp"
#include "utils/FileResourceHandlerCreator.hpp"
#include "utils/Utils.hpp"
#include "utils/String.hpp"
#include "ParameterParsing.hpp"
#include "SubsonicId.hpp"

namespace API::Subsonic
{
    using namespace Database;

    namespace {
        std::optional<Av::Transcoding::OutputFormat> subsonicStreamFormatToAvFormat(std::string_view format)
        {
            for (const auto& [str, avFormat] : std::initializer_list<std::pair<std::string_view, Av::Transcoding::OutputFormat>>{
                {"mp3", Av::Transcoding::OutputFormat::MP3},
                {"opus", Av::Transcoding::OutputFormat::OGG_OPUS},
                {"vorbis", Av::Transcoding::OutputFormat::OGG_VORBIS},
                })
            {
                if (StringUtils::stringCaseInsensitiveEqual(str, format))
                    return avFormat;
            }
            return std::nullopt;
        }

        Av::Transcoding::OutputFormat userTranscodeFormatToAvFormat(Database::TranscodingOutputFormat format)
        {
            switch (format)
            {
            case Database::TranscodingOutputFormat::MP3:            return Av::Transcoding::OutputFormat::MP3;
            case Database::TranscodingOutputFormat::OGG_OPUS:       return Av::Transcoding::OutputFormat::OGG_OPUS;
            case Database::TranscodingOutputFormat::MATROSKA_OPUS:  return Av::Transcoding::OutputFormat::MATROSKA_OPUS;
            case Database::TranscodingOutputFormat::OGG_VORBIS:     return Av::Transcoding::OutputFormat::OGG_VORBIS;
            case Database::TranscodingOutputFormat::WEBM_VORBIS:    return Av::Transcoding::OutputFormat::WEBM_VORBIS;
            }
            return Av::Transcoding::OutputFormat::OGG_OPUS;
        }

        struct StreamParameters
        {
            Av::Transcoding::InputParameters inputParameters;
            std::optional<Av::Transcoding::OutputParameters> outputParameters;
            bool estimateContentLength{};
        };

        StreamParameters getStreamParameters(RequestContext& context)
        {
            // Mandatory params
            const TrackId id{ getMandatoryParameterAs<TrackId>(context.parameters, "id") };

            // Optional params
            std::size_t maxBitRate{ getParameterAs<std::size_t>(context.parameters, "maxBitRate").value_or(0) }; // "If set to zero, no limit is imposed"
            const std::string format{ getParameterAs<std::string>(context.parameters, "format").value_or("") };
            std::size_t timeOffset{ getParameterAs<std::size_t>(context.parameters, "timeOffset").value_or(0) };
            bool estimateContentLength{ getParameterAs<bool>(context.parameters, "estimateContentLength").value_or(false) };

            StreamParameters parameters;

            std::size_t bitrate{};
            parameters.estimateContentLength = estimateContentLength;

            {
                auto transaction{ context.dbSession.createSharedTransaction() };

                const auto track{ Track::find(context.dbSession, id) };
                if (!track)
                    throw RequestedDataNotFoundError{};

                parameters.inputParameters.trackPath = track->getPath();
                parameters.inputParameters.duration = track->getDuration();
                bitrate = track->getBitrate() / 1000;
            }

            if (format == "raw") // raw => no transcode
                return parameters;

            const auto audioFile{ Av::parseAudioFile(parameters.inputParameters.trackPath) };

            // check if transcode is really needed or not
            // same format as requested, bitrate is lower than requested => no need to transcode
            if (const auto streamInfo{ audioFile->getBestStreamInfo() })
            {
                // assume reported codec is "mp3", "opus", "vorbis", etc.
                if (StringUtils::stringCaseInsensitiveEqual(streamInfo->codecName, format) && (maxBitRate == 0 || (bitrate != 0 && bitrate <= maxBitRate)))
                {
                    LMS_LOG(API_SUBSONIC, DEBUG) << "stream parameters are compatible with actual file: no transcode";
                    return parameters;
                }
            }

            auto transaction{ context.dbSession.createSharedTransaction() };

            const User::pointer user{ User::find(context.dbSession, context.userId) };
            if (!user)
                throw UserNotAuthorizedError{};

            Av::Transcoding::OutputParameters& outputParameters{ parameters.outputParameters.emplace() };

            outputParameters.stripMetadata = false; // We want clients to use metadata (offline use, replay gain, etc.)
            outputParameters.offset = std::chrono::seconds{ timeOffset };

            if (std::optional<Av::Transcoding::OutputFormat> requestedFormat{ subsonicStreamFormatToAvFormat(format) })
                outputParameters.format = *requestedFormat;
            else
                outputParameters.format = userTranscodeFormatToAvFormat(user->getSubsonicDefaultTranscodingOutputFormat());

            outputParameters.bitrate = user->getSubsonicDefaultTranscodingOutputBitrate();
            if (maxBitRate != 0)
                outputParameters.bitrate = Utils::clamp(outputParameters.bitrate, std::size_t{ 48000 }, maxBitRate * 1000);

            return parameters;
        }
    }

    void handleDownload(RequestContext& context, const Wt::Http::Request& request, Wt::Http::Response& response)
    {
        std::shared_ptr<IResourceHandler> resourceHandler;

        Wt::Http::ResponseContinuation* continuation{ request.continuation() };
        if (!continuation)
        {
            // Mandatory params
            Database::TrackId id{ getMandatoryParameterAs<Database::TrackId>(context.parameters, "id") };

            std::filesystem::path trackPath;
            {
                auto transaction{ context.dbSession.createSharedTransaction() };

                auto track{ Track::find(context.dbSession, id) };
                if (!track)
                    throw RequestedDataNotFoundError{};

                trackPath = track->getPath();
            }

            resourceHandler = Av::createRawResourceHandler(trackPath);
        }
        else
        {
            resourceHandler = Wt::cpp17::any_cast<std::shared_ptr<IResourceHandler>>(continuation->data());
        }

        continuation = resourceHandler->processRequest(request, response);
        if (continuation)
            continuation->setData(resourceHandler);
    }

    void handleStream(RequestContext& context, const Wt::Http::Request& request, Wt::Http::Response& response)
    {
        std::shared_ptr<IResourceHandler> resourceHandler;

        try
        {
            Wt::Http::ResponseContinuation* continuation = request.continuation();
            if (!continuation)
            {
                StreamParameters streamParameters{ getStreamParameters(context) };
                if (streamParameters.outputParameters)
                    resourceHandler = Av::Transcoding::createResourceHandler(streamParameters.inputParameters, *streamParameters.outputParameters, streamParameters.estimateContentLength);
                else
                    resourceHandler = Av::createRawResourceHandler(streamParameters.inputParameters.trackPath);
            }
            else
            {
                resourceHandler = Wt::cpp17::any_cast<std::shared_ptr<IResourceHandler>>(continuation->data());
            }

            continuation = resourceHandler->processRequest(request, response);
            if (continuation)
                continuation->setData(resourceHandler);
        }
        catch (const Av::Exception& e)
        {
            LMS_LOG(API_SUBSONIC, ERROR) << "Caught Av exception: " << e.what();
        }
    }

    void handleGetCoverArt(RequestContext& context, const Wt::Http::Request& /*request*/, Wt::Http::Response& response)
    {
        // Mandatory params
        const auto trackId{ getParameterAs<TrackId>(context.parameters, "id") };
        const auto releaseId{ getParameterAs<ReleaseId>(context.parameters, "id") };
        const auto artistId{ getParameterAs<ArtistId>(context.parameters, "id") };

        if (!trackId && !releaseId && !artistId)
            throw BadParameterGenericError{ "id" };

        std::size_t size{ getParameterAs<std::size_t>(context.parameters, "size").value_or(1024) };
        size = ::Utils::clamp(size, std::size_t{ 32 }, std::size_t{ 2048 });

        std::shared_ptr<Image::IEncodedImage> cover;
        if (trackId)
            cover = Service<Cover::ICoverService>::get()->getFromTrack(*trackId, size);
        else if (releaseId)
            cover = Service<Cover::ICoverService>::get()->getFromRelease(*releaseId, size);
        else if (artistId)
        {
            // TODO handle a placeholder for artists
            response.setStatus(404);
            return;
        }

        if (!cover && context.enableDefaultCover)
            cover = Service<Cover::ICoverService>::get()->getDefault(size);

        if (!cover)
        {
            response.setStatus(404);
            return;
        }

        response.out().write(reinterpret_cast<const char*>(cover->getData()), cover->getDataSize());
        response.setMimeType(std::string{ cover->getMimeType() });
    }

} // namespace API::Subsonic
