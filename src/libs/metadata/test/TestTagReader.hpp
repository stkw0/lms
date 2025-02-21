/*
 * Copyright (C) 2024 Emeric Poupon
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

#include <map>
#include <vector>
#include <gtest/gtest.h>

#include "Parser.hpp"

namespace MetaData
{
    namespace
    {
        template <typename TagMapType>
        bool tagMapHasMultiValuedTags(const TagMapType& m)
        {
            return std::any_of(std::cbegin(m), std::cend(m), [](const auto& tagPair)
                {
                    return tagPair.second.size() > 1;
                });
        }
    }

    class TestTagReader : public ITagReader
    {
    public:
        static constexpr std::chrono::milliseconds trackDuration{ 180 };
        static constexpr std::size_t trackBitrate{ 128000 };
        static constexpr std::size_t trackBitsPerSample{ 16 };
        static constexpr std::size_t trackSampleRate{ 44000 };

        using Tags = std::unordered_map<TagType, std::vector<std::string_view>>;
        using Performers = std::unordered_map<std::string_view, std::vector<std::string_view>>;
        using ExtraUserTags = std::unordered_map<std::string_view, std::vector<std::string_view>>;
        TestTagReader(Tags&& tags, Performers&& performers = {}, ExtraUserTags&& extraUserTags = {})
            : _tags{ std::move(tags) }
            , _performers{ std::move(performers) }
            , _extraUserTags{ std::move(extraUserTags) }
        {
            _hasMultiValuedTags = tagMapHasMultiValuedTags(_tags)
                || tagMapHasMultiValuedTags(_performers)
                || tagMapHasMultiValuedTags(_extraUserTags);
        }

        bool hasMultiValuedTags() const override
        {
            return _hasMultiValuedTags;
        }

        void visitTagValues(TagType tag, TagValueVisitor visitor) const override
        {
            auto itValues{ _tags.find(tag) };
            if (itValues != std::cend(_tags))
            {
                for (std::string_view value : itValues->second)
                    visitor(value);
            }
        }
        void visitTagValues(std::string_view tag, TagValueVisitor visitor) const override
        {
            auto itValues{ _extraUserTags.find(tag) };
            if (itValues == std::cend(_extraUserTags))
                return;

            for (std::string_view value : itValues->second)
                visitor(value);
        }

        void visitPerformerTags(PerformerVisitor visitor) const override
        {
            for (const auto& [role, names] : _performers)
            {
                for (const auto& name : names)
                    visitor(role, name);
            }
        }

        bool hasEmbeddedCover() const override { return false; };

        std::chrono::milliseconds 	getDuration() const override { return trackDuration; }
        std::size_t                 getBitrate() const override { return trackBitrate; }
        std::size_t                 getBitsPerSample() const override { return trackBitsPerSample; }
        std::size_t                 getSampleRate() const override { return trackSampleRate; }

    private:
        const Tags _tags;
        const Performers _performers;
        const ExtraUserTags _extraUserTags;
        bool _hasMultiValuedTags;
    };
}