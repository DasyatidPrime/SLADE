
// -----------------------------------------------------------------------------
// SLADE - It's a Doom Editor
// Copyright(C) 2008 - 2017 Simon Judd
//
// Email:       sirjuddington@gmail.com
// Web:         http://slade.mancubus.net
// Filename:    ExternalEditManager.cpp
// Description: ExternalEditManager class, keeps track of all entries currently
//              being edited externally for a single ArchivePanel. Also contains
//              some FileMonitor subclasses for handling export/import of
//              various entry types (conversions, etc.)
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110 - 1301, USA.
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
//
// Includes
//
// -----------------------------------------------------------------------------
#include "Main.h"
#include "ExternalEditManager.h"
#include "App.h"
#include "Archive/Archive.h"
#include "Conversions.h"
#include "EntryOperations.h"
#include "General/Executables.h"
#include "General/Misc.h"
#include "Graphics/SImage/SIFormat.h"
#include "Graphics/SImage/SImage.h"
#include "MainEditor.h"
#include "Utility/FileMonitor.h"


// -----------------------------------------------------------------------------
// ExternalEditFileMonitor Class
//
// FileMonitor subclass to handle exporting, monitoring and reimporting an entry
// -----------------------------------------------------------------------------
class ExternalEditFileMonitor : public FileMonitor, Listener
{
public:
	ExternalEditFileMonitor(ArchiveEntry* entry, ExternalEditManager* manager) :
		FileMonitor("", false),
		entry(entry),
		manager(manager)
	{
		// Listen to entry parent archive
		archive = entry->parent();
		listenTo(archive);
	}

	virtual ~ExternalEditFileMonitor() { manager->monitorStopped(this); }

	ArchiveEntry* getEntry() { return entry; }
	void          fileModified() { updateEntry(); }

	virtual void updateEntry() { entry->importFile(filename_); }

	virtual bool exportEntry()
	{
		// Determine export filename/path
		wxFileName fn(App::path(entry->name(), App::Dir::Temp));
		fn.SetExt(entry->type()->extension());

		// Export entry and start monitoring
		bool ok = entry->exportFile(fn.GetFullPath());
		if (ok)
		{
			filename_      = fn.GetFullPath();
			file_modified_ = wxFileModificationTime(filename_);
			Start(1000);
		}
		else
			Global::error = "Failed to export entry";

		return ok;
	}

	void onAnnouncement(Announcer* announcer, string event_name, MemChunk& event_data)
	{
		if (announcer != archive)
			return;

		bool finished = false;

		// Entry removed
		if (event_name == "entry_removed")
		{
			int       index;
			wxUIntPtr ptr;
			event_data.read(&index, sizeof(int));
			event_data.read(&ptr, sizeof(wxUIntPtr));
			if (wxUIntToPtr(ptr) == entry)
				finished = true;
		}

		if (finished)
			delete this;
	}

protected:
	ArchiveEntry*        entry;
	Archive*             archive;
	ExternalEditManager* manager;
	string               gfx_format;
};


// -----------------------------------------------------------------------------
// GfxExternalFileMonitor Class
//
// ExternalEditFileMonitor subclass to handle gfx entries
// -----------------------------------------------------------------------------
class GfxExternalFileMonitor : public ExternalEditFileMonitor
{
public:
	GfxExternalFileMonitor(ArchiveEntry* entry, ExternalEditManager* manager) : ExternalEditFileMonitor(entry, manager)
	{
	}
	virtual ~GfxExternalFileMonitor() {}

	void updateEntry()
	{
		// Read file
		MemChunk data;
		data.importFile(filename_);

		// Read image
		SImage image;
		image.open(data, 0, "png");
		image.convertPaletted(&palette);

		// Convert image to entry gfx format
		SIFormat* format = SIFormat::getFormat(gfx_format);
		if (format)
		{
			MemChunk conv_data;
			if (format->saveImage(image, conv_data, &palette))
			{
				// Update entry data
				entry->importMemChunk(conv_data);
				EntryOperations::setGfxOffsets(entry, offsets.x, offsets.y);
			}
			else
			{
				LOG_MESSAGE(1, "Unable to convert external png to %s", format->name());
			}
		}
	}

	bool exportEntry()
	{
		wxFileName fn(App::path(entry->name(), App::Dir::Temp));

		fn.SetExt("png");

		// Create image from entry
		SImage image;
		if (!Misc::loadImageFromEntry(&image, entry))
		{
			Global::error = "Could not read graphic";
			return false;
		}

		// Set export info
		gfx_format = image.format()->id();
		offsets    = image.offset();
		palette.copyPalette(MainEditor::currentPalette(entry));

		// Write png data
		MemChunk  png;
		SIFormat* fmt_png = SIFormat::getFormat("png");
		if (!fmt_png->saveImage(image, png, &palette))
		{
			Global::error = "Error converting to png";
			return false;
		}

		// Export file and start monitoring if successful
		filename_ = fn.GetFullPath();
		if (png.exportFile(filename_))
		{
			file_modified_ = wxFileModificationTime(filename_);
			Start(1000);
			return true;
		}

		return false;
	}

private:
	string   gfx_format;
	Vec2i offsets;
	Palette  palette;
};


// -----------------------------------------------------------------------------
// MIDIExternalFileMonitor Class
//
// ExternalEditFileMonitor subclass to handle MIDI entries
// -----------------------------------------------------------------------------
class MIDIExternalFileMonitor : public ExternalEditFileMonitor
{
public:
	MIDIExternalFileMonitor(ArchiveEntry* entry, ExternalEditManager* manager) : ExternalEditFileMonitor(entry, manager)
	{
	}
	virtual ~MIDIExternalFileMonitor() {}

	void updateEntry()
	{
		// Can't convert back, just import the MIDI
		entry->importFile(filename_);
	}

	bool exportEntry()
	{
		wxFileName fn(App::path(entry->name(), App::Dir::Temp));
		fn.SetExt("mid");

		// Convert to MIDI data
		MemChunk convdata;

		// MUS
		if (entry->type()->formatId() == "midi_mus")
			Conversions::musToMidi(entry->data(), convdata);

		// HMI/HMP/XMI
		else if (
			entry->type()->formatId() == "midi_xmi" || entry->type()->formatId() == "midi_hmi"
			|| entry->type()->formatId() == "midi_hmp")
			Conversions::zmusToMidi(entry->data(), convdata, 0);

		// GMID
		else if (entry->type()->formatId() == "midi_gmid")
			Conversions::gmidToMidi(entry->data(), convdata);

		else
		{
			Global::error = S_FMT("Type %s can not be converted to MIDI", CHR(entry->type()->name()));
			return false;
		}

		// Export file and start monitoring if successful
		filename_ = fn.GetFullPath();
		if (convdata.exportFile(filename_))
		{
			file_modified_ = wxFileModificationTime(filename_);
			Start(1000);
			return true;
		}

		return false;
	}

	static bool canHandleEntry(ArchiveEntry* entry)
	{
		if (entry->type()->formatId() == "midi" || entry->type()->formatId() == "midi_mus"
			|| entry->type()->formatId() == "midi_xmi" || entry->type()->formatId() == "midi_hmi"
			|| entry->type()->formatId() == "midi_hmp" || entry->type()->formatId() == "midi_gmid")
			return true;

		return false;
	}
};


// -----------------------------------------------------------------------------
// SfxExternalFileMonitor Class
//
// ExternalEditFileMonitor subclass to handle sfx entries
// -----------------------------------------------------------------------------
class SfxExternalFileMonitor : public ExternalEditFileMonitor
{
public:
	SfxExternalFileMonitor(ArchiveEntry* entry, ExternalEditManager* manager) :
		ExternalEditFileMonitor(entry, manager),
		doom_sound(true)
	{
	}
	virtual ~SfxExternalFileMonitor() {}

	void updateEntry()
	{
		// Convert back to doom sound if it was originally
		if (doom_sound)
		{
			MemChunk in, out;
			in.importFile(filename_);
			if (Conversions::wavToDoomSnd(in, out))
			{
				// Import converted data to entry if successful
				entry->importMemChunk(out);
				return;
			}
		}

		// Just import wav to entry if conversion to doom sound
		// failed or the entry was not a convertable type
		entry->importFile(filename_);
	}

	bool exportEntry()
	{
		wxFileName fn(App::path(entry->name(), App::Dir::Temp));
		fn.SetExt("mid");

		// Convert to WAV data
		MemChunk convdata;

		// Doom Sound
		if (entry->type()->formatId() == "snd_doom" || entry->type()->formatId() == "snd_doom_mac")
			Conversions::doomSndToWav(entry->data(), convdata);

		// Doom PC Speaker Sound
		else if (entry->type()->formatId() == "snd_speaker")
			Conversions::spkSndToWav(entry->data(), convdata);

		// AudioT PC Speaker Sound
		else if (entry->type()->formatId() == "snd_audiot")
			Conversions::spkSndToWav(entry->data(), convdata, true);

		// Wolfenstein 3D Sound
		else if (entry->type()->formatId() == "snd_wolf")
			Conversions::wolfSndToWav(entry->data(), convdata);

		// Creative Voice File
		else if (entry->type()->formatId() == "snd_voc")
			Conversions::vocToWav(entry->data(), convdata);

		// Jaguar Doom Sound
		else if (entry->type()->formatId() == "snd_jaguar")
			Conversions::jagSndToWav(entry->data(), convdata);

		// Blood Sound
		else if (entry->type()->formatId() == "snd_bloodsfx")
			Conversions::bloodToWav(entry, convdata);

		else
		{
			Global::error = S_FMT("Type %s can not be converted to WAV", CHR(entry->type()->name()));
			return false;
		}

		// Export file and start monitoring if successful
		filename_ = fn.GetFullPath();
		if (convdata.exportFile(filename_))
		{
			file_modified_ = wxFileModificationTime(filename_);
			Start(1000);
			return true;
		}

		return false;
	}

	static bool canHandleEntry(ArchiveEntry* entry)
	{
		if (entry->type()->formatId() == "snd_doom" || entry->type()->formatId() == "snd_doom_mac"
			|| entry->type()->formatId() == "snd_speaker" || entry->type()->formatId() == "snd_audiot"
			|| entry->type()->formatId() == "snd_wolf" || entry->type()->formatId() == "snd_voc"
			|| entry->type()->formatId() == "snd_jaguar" || entry->type()->formatId() == "snd_bloodsfx")
			return true;

		return false;
	}

private:
	bool doom_sound;
};


// -----------------------------------------------------------------------------
//
// ExternalEditManager Class Functions
//
// -----------------------------------------------------------------------------


// -----------------------------------------------------------------------------
// ExternalEditManager class constructor
// -----------------------------------------------------------------------------
ExternalEditManager::ExternalEditManager() {}

// -----------------------------------------------------------------------------
// ExternalEditManager class destructor
// -----------------------------------------------------------------------------
ExternalEditManager::~ExternalEditManager()
{
	for (unsigned a = 0; a < file_monitors_.size(); a++)
		delete file_monitors_[a];
}

// -----------------------------------------------------------------------------
// Opens [entry] for external editing with [editor] for [category]
// -----------------------------------------------------------------------------
bool ExternalEditManager::openEntryExternal(ArchiveEntry* entry, string editor, string category)
{
	// Check the entry isn't already opened externally
	for (unsigned a = 0; a < file_monitors_.size(); a++)
		if (file_monitors_[a]->getEntry() == entry)
		{
			LOG_MESSAGE(1, "Entry %s is already open in an external editor", entry->name());
			return true;
		}

	// Setup file monitor depending on entry type
	ExternalEditFileMonitor* monitor = nullptr;

	// Gfx entry
	if (entry->type()->editor() == "gfx" && entry->type()->id() != "png")
		monitor = new GfxExternalFileMonitor(entry, this);
	// MIDI entry
	else if (MIDIExternalFileMonitor::canHandleEntry(entry))
		monitor = new MIDIExternalFileMonitor(entry, this);
	// Sfx entry
	else if (SfxExternalFileMonitor::canHandleEntry(entry))
		monitor = new SfxExternalFileMonitor(entry, this);
	// Other entry
	else
		monitor = new ExternalEditFileMonitor(entry, this);

	// Export entry to temp file and start monitoring if successful
	if (!monitor->exportEntry())
	{
		delete monitor;
		return false;
	}

	// Get external editor path
	string exe_path = Executables::externalExe(editor, category).path;
#ifdef WIN32
	if (exe_path.IsEmpty() || !wxFileExists(exe_path))
#else
	if (exe_path.IsEmpty())
#endif
	{
		Global::error = S_FMT("External editor %s has invalid path", editor);
		delete monitor;
		return false;
	}

	// Run external editor
	string command = S_FMT("\"%s\" \"%s\"", exe_path, monitor->filename());
	long   success = wxExecute(command, wxEXEC_ASYNC, monitor->process());
	if (success == 0)
	{
		Global::error = S_FMT("Failed to launch %s", editor);
		delete monitor;
		return false;
	}

	// Add to list of file monitors for tracking
	file_monitors_.push_back(monitor);

	return true;
}

// -----------------------------------------------------------------------------
// Called when a FileMonitor is stopped/deleted
// -----------------------------------------------------------------------------
void ExternalEditManager::monitorStopped(ExternalEditFileMonitor* monitor)
{
	if (VECTOR_EXISTS(file_monitors_, monitor))
		VECTOR_REMOVE(file_monitors_, monitor);
}
