/*
 * Copyright (C) 2018 Emeric Poupon
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

#include "av/IAudioFile.hpp"
#include "metadata/IParser.hpp"
#include "ITagReader.hpp"

namespace MetaData
{
    class AvFormatTagReader : public ITagReader
    {
   public:
        AvFormatTagReader(const std::filesystem::path& path, bool debug);

    private:
        AvFormatTagReader(const AvFormatTagReader&) = delete;
        AvFormatTagReader& operator=(const AvFormatTagReader&) = delete;

        bool hasMultiValuedTags() const override { return false; /* not supported */}
        void visitTagValues(TagType tag, TagValueVisitor visitor) const override;
        void visitTagValues(std::string_view tag, TagValueVisitor visitor) const override;
        void visitPerformerTags(PerformerVisitor visitor) const override;
        bool hasEmbeddedCover() const override { return _hasEmbeddedCover; }

        std::chrono::milliseconds 	getDuration() const override { return _containerInfo.duration; }
        std::size_t                 getBitrate() const override { return _containerInfo.bitrate; }
        std::size_t                 getBitsPerSample() const override { return 0; }
        std::size_t                 getSampleRate() const override { return 0; }

        Av::IAudioFile::MetadataMap _metaDataMap;
        Av::ContainerInfo _containerInfo;
        bool _hasEmbeddedCover{};
    };
} // namespace MetaData

