/*
 * Copyright (C) 2013 Emeric Poupon
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

#include "ScannerService.hpp"

#include <ctime>
#include <boost/asio/placeholders.hpp>

#include "database/MediaLibrary.hpp"
#include "database/TrackFeatures.hpp"
#include "database/ScanSettings.hpp"
#include "utils/Exception.hpp"
#include "utils/IConfig.hpp"
#include "utils/ILogger.hpp"
#include "utils/Path.hpp"
#include "utils/Tuple.hpp"

#include "ScanStepCheckDuplicatedDbFiles.hpp"
#include "ScanStepDiscoverFiles.hpp"
#include "ScanStepRemoveOrphanDbFiles.hpp"
#include "ScanStepScanFiles.hpp"
#include "ScanStepComputeClusterStats.hpp"

namespace Scanner
{
    using namespace Database;

    namespace
    {
        Wt::WDate getNextMonday(Wt::WDate current)
        {
            do
            {
                current = current.addDays(1);
            } while (current.dayOfWeek() != 1);

            return current;
        }

        Wt::WDate getNextFirstOfMonth(Wt::WDate current)
        {
            do
            {
                current = current.addDays(1);
            } while (current.day() != 1);

            return current;
        }
    } // namespace

    std::unique_ptr<IScannerService> createScannerService(Db& db)
    {
        return std::make_unique<ScannerService>(db);
    }

    ScannerService::ScannerService(Db& db)
        : _db{ db }
        , _dbSession{ db }
    {
        _ioService.setThreadCount(1);

        refreshScanSettings();

        start();
    }

    ScannerService::~ScannerService()
    {
        LMS_LOG(DBUPDATER, INFO, "Stopping service...");
        stop();
        LMS_LOG(DBUPDATER, INFO, "Service stopped!");
    }

    void ScannerService::start()
    {
        std::scoped_lock lock{ _controlMutex };

        _ioService.post([this]
            {
                if (_abortScan)
                    return;

                scheduleNextScan();
            });

        _ioService.start();
    }

    void ScannerService::stop()
    {
        std::scoped_lock lock{ _controlMutex };

        _abortScan = true;
        _scheduleTimer.cancel();
        _ioService.stop();
    }

    void ScannerService::abortScan()
    {
        bool isRunning{};
        {
            std::scoped_lock lock{ _statusMutex };
            isRunning = _curState == State::InProgress;
        }

        LMS_LOG(DBUPDATER, DEBUG, "Aborting scan...");
        std::scoped_lock lock{ _controlMutex };

        LMS_LOG(DBUPDATER, DEBUG, "Waiting for the scan to abort...");

        _abortScan = true;
        _scheduleTimer.cancel();
        _ioService.stop();
        LMS_LOG(DBUPDATER, DEBUG, "Scan abort done!");

        _abortScan = false;
        _ioService.start();

        if (isRunning)
            _events.scanAborted.emit();
    }

    void ScannerService::requestImmediateScan(bool force)
    {
        abortScan();
        _ioService.post([this, force]
            {
                if (_abortScan)
                    return;

                scheduleScan(force);
            });
    }

    void ScannerService::requestStop()
    {
        abortScan();
    }

    void ScannerService::requestReload()
    {
        abortScan();
        _ioService.post([this]()
            {
                if (_abortScan)
                    return;

                scheduleNextScan();
            });
    }

    ScannerService::Status ScannerService::getStatus() const
    {
        Status res;

        std::shared_lock lock{ _statusMutex };

        res.currentState = _curState;
        res.nextScheduledScan = _nextScheduledScan;
        res.lastCompleteScanStats = _lastCompleteScanStats;
        res.currentScanStepStats = _currentScanStepStats;

        return res;
    }

    void ScannerService::scheduleNextScan()
    {
        LMS_LOG(DBUPDATER, DEBUG, "Scheduling next scan");

        refreshScanSettings();

        const Wt::WDateTime now{ Wt::WDateTime::currentDateTime() };

        Wt::WDateTime nextScanDateTime;
        switch (_settings.updatePeriod)
        {
        case ScanSettings::UpdatePeriod::Daily:
            if (now.time() < _settings.startTime)
                nextScanDateTime = { now.date(), _settings.startTime };
            else
                nextScanDateTime = { now.date().addDays(1), _settings.startTime };
            break;

        case ScanSettings::UpdatePeriod::Weekly:
            if (now.time() < _settings.startTime && now.date().dayOfWeek() == 1)
                nextScanDateTime = { now.date(), _settings.startTime };
            else
                nextScanDateTime = { getNextMonday(now.date()), _settings.startTime };
            break;

        case ScanSettings::UpdatePeriod::Monthly:
            if (now.time() < _settings.startTime && now.date().day() == 1)
                nextScanDateTime = { now.date(), _settings.startTime };
            else
                nextScanDateTime = { getNextFirstOfMonth(now.date()), _settings.startTime };
            break;

        case ScanSettings::UpdatePeriod::Hourly:
            nextScanDateTime = { now.date(), now.time().addSecs(3600) };
            break;

        case ScanSettings::UpdatePeriod::Never:
            LMS_LOG(DBUPDATER, INFO, "Auto scan disabled!");
            break;
        }

        if (nextScanDateTime.isValid())
            scheduleScan(false, nextScanDateTime);

        {
            std::unique_lock lock{ _statusMutex };
            _curState = nextScanDateTime.isValid() ? State::Scheduled : State::NotScheduled;
            _nextScheduledScan = nextScanDateTime;
        }

        _events.scanScheduled.emit(_nextScheduledScan);
    }

    void ScannerService::scheduleScan(bool force, const Wt::WDateTime& dateTime)
    {
        auto cb{ [this, force](boost::system::error_code ec)
        {
            if (ec)
                return;

            scan(force);
        } };

        if (dateTime.isNull())
        {
            LMS_LOG(DBUPDATER, INFO, "Scheduling next scan right now");
            _scheduleTimer.expires_from_now(std::chrono::seconds{ 0 });
            _scheduleTimer.async_wait(cb);
        }
        else
        {
            std::chrono::system_clock::time_point timePoint{ dateTime.toTimePoint() };
            std::time_t t{ std::chrono::system_clock::to_time_t(timePoint) };
            char ctimeStr[26];

            LMS_LOG(DBUPDATER, INFO, "Scheduling next scan at " << std::string(::ctime_r(&t, ctimeStr)));
            _scheduleTimer.expires_at(timePoint);
            _scheduleTimer.async_wait(cb);
        }
    }

    void ScannerService::scan(bool forceScan)
    {
        _events.scanStarted.emit();

        {
            std::unique_lock lock{ _statusMutex };
            _curState = State::InProgress;
            _nextScheduledScan = {};
        }


        LMS_LOG(UI, INFO, "New scan started!");

        refreshScanSettings();

        IScanStep::ScanContext scanContext{ forceScan, ScanStats {}, ScanStepStats {} };
        ScanStats& stats{ scanContext.stats };
        stats.startTime = Wt::WDateTime::currentDateTime();

        for (auto& scanStep : _scanSteps)
        {
            LMS_LOG(DBUPDATER, DEBUG, "Starting scan step '" << scanStep->getStepName() << "'");
            scanContext.currentStepStats = ScanStepStats{ Wt::WDateTime::currentDateTime(), scanStep->getStep() };

            notifyInProgress(scanContext.currentStepStats);
            scanStep->process(scanContext);
            notifyInProgress(scanContext.currentStepStats);
            LMS_LOG(DBUPDATER, DEBUG, "Completed scan step '" << scanStep->getStepName() << "'");
        }

        LMS_LOG(DBUPDATER, INFO, "Scan " << (_abortScan ? "aborted" : "complete") << ". Changes = " << stats.nbChanges() << " (added = " << stats.additions << ", removed = " << stats.deletions << ", updated = " << stats.updates << "), Not changed = " << stats.skips << ", Scanned = " << stats.scans << " (errors = " << stats.errors.size() << "), features fetched = " << stats.featuresFetched << ",  duplicates = " << stats.duplicates.size());

        _dbSession.analyze();

        if (!_abortScan)
        {
            stats.stopTime = Wt::WDateTime::currentDateTime();
            {
                std::unique_lock lock{ _statusMutex };

                _lastCompleteScanStats = stats;
                _currentScanStepStats.reset();
            }

            LMS_LOG(DBUPDATER, DEBUG, "Scan not aborted, scheduling next scan!");
            scheduleNextScan();

            _events.scanComplete.emit(stats);
        }
        else
        {
            LMS_LOG(DBUPDATER, DEBUG, "Scan aborted, not scheduling next scan!");

            std::unique_lock lock{ _statusMutex };

            _curState = State::NotScheduled;
            _currentScanStepStats.reset();
        }
    }

    void ScannerService::refreshScanSettings()
    {
        ScannerSettings newSettings{ readSettings() };
        if (_settings == newSettings)
            return;

        LMS_LOG(DBUPDATER, DEBUG, "Scanner settings updated");
        LMS_LOG(DBUPDATER, DEBUG, "skipDuplicateMBID = " << newSettings.skipDuplicateMBID);
        LMS_LOG(DBUPDATER, DEBUG, "Using scan settings version " << newSettings.scanVersion);

        _settings = std::move(newSettings);

        auto cbFunc{ [this](const ScanStepStats& stats)
            {
                notifyInProgressIfNeeded(stats);
            } };

        ScanStepBase::InitParams params
        {
            _settings,
            cbFunc,
            _abortScan,
            _db
        };

        _scanSteps.clear();
        _scanSteps.push_back(std::make_unique<ScanStepDiscoverFiles>(params));
        _scanSteps.push_back(std::make_unique<ScanStepScanFiles>(params));
        _scanSteps.push_back(std::make_unique<ScanStepRemoveOrphanDbFiles>(params));
        _scanSteps.push_back(std::make_unique<ScanStepComputeClusterStats>(params));
        _scanSteps.push_back(std::make_unique<ScanStepCheckDuplicatedDbFiles>(params));
    }

    ScannerSettings ScannerService::readSettings()
    {
        ScannerSettings newSettings;

        newSettings.skipDuplicateMBID = Service<IConfig>::get()->getBool("scanner-skip-duplicate-mbid", false);
        {
            auto transaction{ _dbSession.createReadTransaction() };

            const ScanSettings::pointer scanSettings{ ScanSettings::get(_dbSession) };

            newSettings.scanVersion = scanSettings->getScanVersion();
            newSettings.startTime = scanSettings->getUpdateStartTime();
            newSettings.updatePeriod = scanSettings->getUpdatePeriod();

            {
                const auto fileExtensions{ scanSettings->getAudioFileExtensions() };
                newSettings.supportedExtensions.reserve(fileExtensions.size());
                std::transform(std::cbegin(fileExtensions), std::end(fileExtensions), std::back_inserter(newSettings.supportedExtensions),
                    [](const std::filesystem::path& extension) { return std::filesystem::path{ StringUtils::stringToLower(extension.string()) }; });
            }

            MediaLibrary::find(_dbSession, [&](const MediaLibrary::pointer& mediaLibrary)
                {
                    newSettings.mediaLibraries.push_back(ScannerSettings::MediaLibraryInfo{ mediaLibrary->getId(), mediaLibrary->getPath().lexically_normal() });
                });

            {
                const auto& tags{ scanSettings->getExtraTagsToScan() };
                std::transform(std::cbegin(tags), std::cend(tags), std::back_inserter(newSettings.extraTags), [](std::string_view tag) { return std::string{ tag };});
            }

            newSettings.artistTagDelimiters = scanSettings->getArtistTagDelimiters();
            newSettings.defaultTagDelimiters = scanSettings->getDefaultTagDelimiters();
        }

        return newSettings;
    }

    void ScannerService::notifyInProgress(const ScanStepStats& stepStats)
    {
        {
            std::unique_lock lock{ _statusMutex };
            _currentScanStepStats = stepStats;
        }

        const std::chrono::system_clock::time_point now{ std::chrono::system_clock::now() };
        _events.scanInProgress(stepStats);
        _lastScanInProgressEmit = now;
    }

    void ScannerService::notifyInProgressIfNeeded(const ScanStepStats& stepStats)
    {
        std::chrono::system_clock::time_point now{ std::chrono::system_clock::now() };

        if (std::chrono::duration_cast<std::chrono::seconds>(now - _lastScanInProgressEmit).count() > 1)
            notifyInProgress(stepStats);
    }

} // namespace Scanner
