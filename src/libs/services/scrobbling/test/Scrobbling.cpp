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

#include <gtest/gtest.h>

#include "utils/ILogger.hpp"
#include "utils/Service.hpp"
#include "utils/StreamLogger.hpp"

int main(int argc, char** argv)
{
    // log to stdout
    Service<ILogger> logger{ std::make_unique<StreamLogger>(std::cout, EnumSet<Severity> {Severity::FATAL, Severity::ERROR}) };

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

