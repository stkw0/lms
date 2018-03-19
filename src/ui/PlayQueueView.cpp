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

#include <Wt/WAnchor>
#include <Wt/WImage>
#include <Wt/WText>

#include "utils/Logger.hpp"

#include "LmsApplication.hpp"
#include "PlayQueueView.hpp"

namespace UserInterface {

static const std::string currentPlayQueueName = "__current__playqueue__";

PlayQueue::PlayQueue(Wt::WContainerWidget* parent)
: Wt::WContainerWidget(parent)
{
	auto container = new Wt::WTemplate(Wt::WString::tr("Lms.PlayQueue.template"), this);
	container->addFunction("tr", &Wt::WTemplate::Functions::tr);

	auto saveBtn = new Wt::WText(Wt::WString::tr("Lms.PlayQueue.save-to-playlist"), Wt::XHTMLText);
	container->bindWidget("save-btn", saveBtn);

	auto loadBtn = new Wt::WText(Wt::WString::tr("Lms.PlayQueue.load-from-playlist"), Wt::XHTMLText);
	container->bindWidget("load-btn", loadBtn);

	auto clearBtn = new Wt::WText(Wt::WString::tr("Lms.PlayQueue.clear"), Wt::XHTMLText);
	container->bindWidget("clear-btn", clearBtn);
	clearBtn->clicked().connect(std::bind([=]
	{
		{
			Wt::Dbo::Transaction transaction(DboSession());

			auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());
			playlist.modify()->clear();
			_showMore->setHidden(true);
		}

		_entriesContainer->clear();
		updateInfo();
	}));

	_entriesContainer = new Wt::WContainerWidget();
	container->bindWidget("entries", _entriesContainer);

	_showMore = new Wt::WTemplate(Wt::WString::tr("Lms.Explore.show-more"));
	_showMore->addFunction("tr", &Wt::WTemplate::Functions::tr);
	_showMore->setHidden(true);
	container->bindWidget("show-more", _showMore);

	_nbTracks = new Wt::WText();
	container->bindWidget("nb-tracks", _nbTracks);

	{
		Wt::Dbo::Transaction transaction (DboSession());

		auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());
		if (!playlist)
			playlist = Database::Playlist::create(DboSession(), currentPlayQueueName, false, CurrentUser());

		_trackPos = CurrentUser()->getCurPlayingTrackPos();
	}

	_showMore->clicked().connect(std::bind([=]
	{
		addSome();
	}));

	updateInfo();
	addSome();
}

void
PlayQueue::stop()
{
	updateCurrentTrack(false);
	_trackPos.reset();
}

void
PlayQueue::play(std::size_t pos)
{
	updateCurrentTrack(false);

	Database::Track::id_type trackId;
	{
		Wt::Dbo::Transaction transaction(DboSession());

		auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());

		// If out of range, stop playing
		if (pos >= playlist->getCount())
		{
			stop();
			return;
		}

		_trackPos = pos;
		trackId = playlist->getEntry(*_trackPos)->getTrack().id();
		updateCurrentTrack(true);
	}

	playTrack.emit(trackId);
}

void
PlayQueue::playPrevious()
{
	if (!_trackPos)
		return;

	if (*_trackPos == 0)
		stop();
	else
		play(*_trackPos - 1);
}

void
PlayQueue::playNext()
{
	if (!_trackPos)
	{
		play(0);
		return;
	}

	play(*_trackPos + 1);
}

void
PlayQueue::updateInfo()
{
	Wt::Dbo::Transaction transaction(DboSession());

	auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());
	_nbTracks->setText(Wt::WString::tr("Lms.PlayQueue.nb-tracks").arg(playlist->getCount()));
}

void
PlayQueue::updateCurrentTrack(bool selected)
{
	if (!_trackPos || *_trackPos >= static_cast<std::size_t>(_entriesContainer->count()))
		return;

	auto track = _entriesContainer->widget(*_trackPos);
	if (track)
	{
		if (selected)
			track->addStyleClass("Lms-playqueue-selected");
		else
			track->removeStyleClass("Lms-playqueue-selected");
	}
}

void
PlayQueue::addTracks(const std::vector<Database::Track::pointer>& tracks)
{
	// Use a "session" playqueue in order to store the current playqueue
	// so that the user can disconnect and get its playqueue back

	LMS_LOG(UI, DEBUG) << "Adding tracks to the current queue";

	auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());

	for (auto track : tracks)
		Database::PlaylistEntry::create(DboSession(), track, playlist);

	updateInfo();
	addSome();
}

void
PlayQueue::playTracks(const std::vector<Database::Track::pointer>& tracks)
{
	LMS_LOG(UI, DEBUG) << "Emptying current queue to play new tracks";

	Wt::Dbo::Transaction transaction(DboSession());

	auto playqueue = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());
	playqueue.modify()->clear();

	_entriesContainer->clear();

	addTracks(tracks);

	play(0);
}


void
PlayQueue::addSome()
{
	Wt::Dbo::Transaction transaction (DboSession());

	auto playlist = Database::Playlist::get(DboSession(), currentPlayQueueName, CurrentUser());

	bool moreResults;
	auto playlistEntries = playlist->getEntries(_entriesContainer->count(), 50, moreResults);
	for (auto playlistEntry : playlistEntries)
	{
		auto playlistEntryId = playlistEntry.id();
		auto track = playlistEntry->getTrack();

		Wt::WTemplate* entry = new Wt::WTemplate(Wt::WString::tr("Lms.PlayQueue.template.entry"), _entriesContainer);

		entry->bindString("name", Wt::WString::fromUTF8(track->getName()), Wt::PlainText);

		auto artist = track->getArtist();
		if (artist)
		{
			entry->setCondition("if-has-artist", true);
			Wt::WAnchor *artistAnchor = LmsApplication::createArtistAnchor(track->getArtist().id());
			Wt::WText *artistText = new Wt::WText(artistAnchor);
			artistText->setText(Wt::WString::fromUTF8(artist->getName(), Wt::PlainText));
			entry->bindWidget("artist-name", artistAnchor);
		}
		auto release = track->getRelease();
		if (release)
		{
			entry->setCondition("if-has-release", true);
			Wt::WAnchor *releaseAnchor = LmsApplication::createReleaseAnchor(track->getRelease().id());
			Wt::WText *releaseText = new Wt::WText(releaseAnchor);
			releaseText->setText(Wt::WString::fromUTF8(release->getName(), Wt::PlainText));
			entry->bindWidget("release-name", releaseAnchor);
		}

		auto playBtn = new Wt::WText(Wt::WString::tr("Lms.PlayQueue.play"), Wt::XHTMLText);
		entry->bindWidget("play-btn", playBtn);
		playBtn->clicked().connect(std::bind([=]
		{
			auto pos = _entriesContainer->indexOf(entry);
			if (pos >= 0)
				play(pos);
		}));

		auto delBtn = new Wt::WText(Wt::WString::tr("Lms.PlayQueue.delete"), Wt::XHTMLText);
		entry->bindWidget("del-btn", delBtn);
		delBtn->clicked().connect(std::bind([=]
		{
			// Remove the entry n both the widget tree and the playqueue
			{
				Wt::Dbo::Transaction transaction (DboSession());

				auto entryToRemove = Database::PlaylistEntry::getById(DboSession(), playlistEntryId);
				entryToRemove.remove();
			}

			if (_trackPos)
			{
				auto pos = _entriesContainer->indexOf(entry);
				if (pos > 0 && *_trackPos >= static_cast<std::size_t>(pos))
					(*_trackPos)--;
			}

			_entriesContainer->removeWidget(entry);

			updateInfo();
		}));
	}

	_showMore->setHidden(!moreResults);
}

} // namespace UserInterface

