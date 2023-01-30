//
// PSXRip - Disassemble a PlayStation 1 disc image into its contents
//
// Copyright (C) 2014 Christian Bauer <www.cebix.net>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
//

#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/info.h>
#include <libvcd/sector.h>
}

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
using boost::format;

#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <time.h>
#include <vector>
using namespace std;


#define TOOL_VERSION "PSXRip 2.0"
#define timegm _mkgmtime

// Y2k / root entry processing error
string rootEntryReplacementStr;

// Audio sector counter
int audioSectors = 0;

// GMT
int gmtValue = 0;
int gmtValueParent = 0;

// Sector buffer
static char buffer[M2RAW_SECTOR_SIZE];


// To hex function.
std::string toHexString(const uint8_t* data, const size_t size) {
    std::string hex_str;
    hex_str.reserve(size * 2); // reserve enough space to avoid reallocations

    static const char hex_chars[] = "0123456789ABCDEF";

    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        hex_str.push_back(hex_chars[byte >> 4]);
        hex_str.push_back(hex_chars[byte & 0x0F]);
    }

    return hex_str;
}

// Print an ISO long-format time structure to a file.
static void print_ltime(ofstream & f, const iso9660_ltime_t & l, bool creation_time = false)
{
	// Assuming Y2K bug on creation_date / root directory record on some ISO's where the PVD date year is reported as 0000 instead of 19xx or 20xx.
	// stat->tm Bug on root record. When the date on the filesystem reads 00 hex instead of the years after 1900 in hex. This messes up epoch calculations.
	// All the stat->tm.xxx fields are corrupted, so we need a replacement. Luckely the pvd.creation_date is correct all but the year.
	// I cannot write the faulty code back due to epoch on 32 bit not going far enough back (1901).
	// So instead of that it writes the corrected date back. Not 1:1 but the next best thing.
	std::string century_str = {l.lt_year[0], l.lt_year[1]};
	std::string year_str = {l.lt_year[2], l.lt_year[3]};
	std::string month_str = {l.lt_month[0], l.lt_month[1]};
	std::string day_str = {l.lt_day[0], l.lt_day[1]};
	std::string hour_str = {l.lt_hour[0], l.lt_hour[1]};
	std::string minute_str = {l.lt_minute[0], l.lt_minute[1]};
	std::string second_str = {l.lt_second[0], l.lt_second[1]};

	if (creation_time == true && l.lt_gmtoff != 0) {
		gmtValue = l.lt_gmtoff; // Change the global gmtValue if not 0. But only for the creation date.
	}
	
	if (century_str == "00" && std::stoi(day_str) >= 1) {
		century_str = "20";
		rootEntryReplacementStr = century_str + year_str + month_str + day_str + hour_str + minute_str + second_str;
	}
	
	f << format("%.2s%.2s-%.2s-%.2s %.2s:%.2s:%.2s.%.2s %d")
			% century_str % year_str % l.lt_month % l.lt_day
			% l.lt_hour % l.lt_minute % l.lt_second % l.lt_hsecond
			% int(l.lt_gmtoff) << endl;
}


// Dump system area data from image to file.
static void dumpSystemArea(CdIo_t *image, const boost::filesystem::path &fileName)
{
  ofstream file(fileName.generic_string(), ofstream::out | ofstream::binary | ofstream::trunc);
  if (!file) {
    throw runtime_error((format("Cannot create system area file %1%\n") % fileName).str());
  }

  const size_t numSystemAreaSectors = 16;
  for (size_t sector = 0; sector < numSystemAreaSectors; ++sector) {
    char buffer[CDIO_CD_FRAMESIZE_RAW];
    driver_return_code_t r = cdio_read_audio_sectors(image, buffer, sector, 1);
    if (r != DRIVER_OP_SUCCESS) {
      throw runtime_error((format("Error reading sector %1% of image file: %2%") % sector % cdio_driver_errmsg(r)).str());
    }

    file.write(buffer, CDIO_CD_FRAMESIZE_RAW);
    if (!file) {
      throw runtime_error((format("Cannot write to system area file %1%") % fileName).str());
    }
  }
}

// Functor for sorting a container of iso9660_stat_t pointers by LSN
struct CmpByLSN {
	bool operator()(const iso9660_stat_t * lhs, const iso9660_stat_t * rhs)
	{
		return lhs->lsn < rhs->lsn;
	}
};


// Recursively dump the contents of the ISO filesystem starting at 'dir'
// while extending the catalog file.
static void dumpFilesystem(CdIo_t * image, ofstream & catalog, bool writeLBNs, bool writeGUADs,
						   const boost::filesystem::path & outputPath, const string & inputPath = "",
						   const string & dirName = "", unsigned level = 0)
{
	cdio_info("Dumping '%s' as '%s'", inputPath.c_str(), dirName.c_str());

	// Read the directory entries
	CdioList_t * entries = iso9660_fs_readdir(image, inputPath.c_str());
	if (!entries) {
		throw runtime_error((format("Error reading ISO 9660 directory '%1%'") % inputPath).str());
	}

	// Create the output directory
	boost::filesystem::path outputDirName = outputPath / dirName;
	boost::filesystem::create_directory(outputDirName);

	// Open the catalog record for the directory
	CdioListNode_t *node = _cdio_list_begin(entries);

	char evaluateYear[5];
	time_t directoryEpochSelf, directoryEpochParent;
	char datestringSelf[32], datestringParent[32];

	// Process first directory entry "."
	iso9660_stat_t *statSelf = static_cast<iso9660_stat_t *>(_cdio_list_node_data(node));
	if (snprintf(evaluateYear, sizeof(evaluateYear), "%d", statSelf->tm.tm_year), std::string(evaluateYear).length() < 2 || std::string(evaluateYear).length() > 3 || std::stoi(std::string(evaluateYear)) < 70) {
		strncpy(datestringSelf, rootEntryReplacementStr.c_str(), sizeof(datestringSelf));
	} else {
	directoryEpochSelf = timegm(&statSelf->tm); 
	directoryEpochSelf = directoryEpochSelf + (statSelf->timezone * 15 * 60);
	tm* time_info_self = gmtime(&directoryEpochSelf);
	struct tm time_info_val_self = *time_info_self;
	strftime(datestringSelf, sizeof(datestringSelf), "%Y%m%d%H%M%S", &time_info_val_self);
	}

	node = _cdio_list_node_next(node);

	// Process second directory entry ".."
	iso9660_stat_t *statParent = static_cast<iso9660_stat_t *>(_cdio_list_node_data(node));
	if (snprintf(evaluateYear, sizeof(evaluateYear), "%d", statParent->tm.tm_year), std::string(evaluateYear).length() < 2 || std::string(evaluateYear).length() > 3 || std::stoi(std::string(evaluateYear)) < 70) {
	strncpy(datestringParent, rootEntryReplacementStr.c_str(), sizeof(datestringParent));
	} else {
		directoryEpochParent = timegm(&statParent->tm); 
		directoryEpochParent = directoryEpochParent + (statParent->timezone * 15 * 60);
		tm* time_info_parent = gmtime(&directoryEpochParent);
		struct tm time_info_val_parent = *time_info_parent;
		strftime(datestringParent, sizeof(datestringParent), "%Y%m%d%H%M%S", &time_info_val_parent);
	}

	if (level == 0) {
		catalog << "dir";  // root
		if (writeLBNs) {
			catalog << " @" << statSelf->lsn;
		}
		if (writeGUADs) {
			catalog << " GID" << _byteswap_ushort(statSelf->xa.group_id);
			catalog << " UID" << _byteswap_ushort(statSelf->xa.user_id);
			catalog << " ATR" << _byteswap_ushort(statSelf->xa.attributes);
			catalog << " DATES" << datestringSelf;
			catalog << " DATEP" << datestringParent;
			catalog << " TIMEZONES" << std::to_string(statSelf->timezone);
			catalog << " TIMEZONEP" << std::to_string(statParent->timezone);
			catalog << " HIDDEN" << statSelf->hidden;
		}
		catalog << " {\n";
	} else {
		catalog << string(level * 2, ' ') << "dir " << dirName;
		if (writeLBNs) {
			catalog << " @" << statSelf->lsn;
		}
		if (writeGUADs) {
			catalog << " GID" << _byteswap_ushort(statSelf->xa.group_id);
			catalog << " UID" << _byteswap_ushort(statSelf->xa.user_id);
			catalog << " ATR" << _byteswap_ushort(statSelf->xa.attributes);
			catalog << " DATES" << datestringSelf;
			catalog << " DATEP" << datestringParent;
			catalog << " TIMEZONES" << std::to_string(statSelf->timezone);
			catalog << " TIMEZONEP" << std::to_string(statParent->timezone);
			catalog << " HIDDEN" << statSelf->hidden;
		}
		catalog << " {\n";
	}

	// Sort entries by sector number
	vector<iso9660_stat_t *> sortedChildren;

	CdioListNode_t * entry;
	_CDIO_LIST_FOREACH(entry, entries) {
		sortedChildren.push_back(static_cast<iso9660_stat_t *>(_cdio_list_node_data(entry)));
	}

	sort(sortedChildren.begin(), sortedChildren.end(), CmpByLSN());

	// Dump all entries
	for (vector<iso9660_stat_t *>::const_iterator i = sortedChildren.begin(); i != sortedChildren.end(); ++i) {
		iso9660_stat_t * stat = *i;
		string entryName = stat->filename;
		string entryPath = inputPath.empty() ? entryName : (inputPath + "/" + entryName);

		char datestringEntry[32];
		time_t entryEpoch = timegm(&stat->tm); // Turn it back to standard GMT 00:00 UTC
		entryEpoch = entryEpoch + (stat->timezone * 15 * 60);
		tm* time_info_entry = gmtime(&entryEpoch);
		struct tm time_info_entry_val = *time_info_entry;
		strftime(datestringEntry, sizeof(datestringEntry), "%Y%m%d%H%M%S", &time_info_entry_val);

		if (stat->type == iso9660_stat_s::_STAT_DIR) {

			// Entry is a directory, recurse into it unless it is "." or ".."
			if (entryName != "." && entryName != "..") {
				// std::cout << "Parsing: " << stat2->filename << " - " << fileDate << std::endl; // Debug
				dumpFilesystem(image, catalog, writeLBNs, writeGUADs, outputDirName, entryPath, entryName, level + 1);
			}

		} else {

			// Entry is a file, strip the version number
			size_t versionSep = entryName.find_last_of(';');
			if (versionSep != string::npos) {
				entryName = entryName.substr(0, versionSep);
			}

			// Is it an XA form 2 file?
			bool form2File = false;
			bool cddaFile = false;
			if (stat->b_xa) {
				uint16_t attr = uint16_from_be(stat->xa.attributes);
				if (attr & (XA_ATTR_MODE2FORM2 | XA_ATTR_INTERLEAVED)) {
					cdio_info("XA file '%s' size = %u, secsize = %u, group_id = %d, user_id = %d, attributes = %04x, filenum = %d",
					          entryName.c_str(), stat->size, stat->secsize, stat->xa.group_id, stat->xa.user_id, attr, stat->xa.filenum);
					form2File = true;
				}

				if (attr & XA_ATTR_CDDA) {
					cdio_info("XA file '%s' size = %u, secsize = %u, group_id = %d, user_id = %d, attributes = %04x, filenum = %d",
					entryName.c_str(), stat->size, stat->secsize, stat->xa.group_id, stat->xa.user_id, attr, stat->xa.filenum);
					cddaFile = true;
				}
			}

			// For form 2 files, the size in the directory record is usually just
			// the ISO block size (2048) times the number of sectors in the file.
			// The actual file size is larger because the sectors have 2336 bytes.
			size_t blockSize = form2File ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
			size_t fileSize = form2File ? stat->secsize * blockSize : stat->size;

			// Write the catalog record for the file
			catalog << string((level + 1) * 2, ' ') << (form2File ? "xa" : "") << (cddaFile ? "cdda" : "") << "file " << entryName;

			if (writeLBNs || cddaFile) {
				catalog << " @" << stat->lsn;
			}
			if (writeGUADs) {
				catalog << " GID" << _byteswap_ushort(stat->xa.group_id);
				catalog << " UID" << _byteswap_ushort(stat->xa.user_id);
				catalog << " ATR" << _byteswap_ushort(stat->xa.attributes);
				catalog << " DATE" << datestringEntry;
				catalog << " TIMEZONE" << std::to_string(stat->timezone);
				catalog << " HIDDEN" << stat->hidden;
				catalog << " SIZE" << stat->size;
			}
			catalog << " \n";

			// Dump the file contents
			boost::filesystem::path outputFileName = outputDirName / entryName;
			ofstream file(outputFileName.generic_string(), ofstream::out | ofstream::binary | ofstream::trunc);
			if (!file) {
				throw runtime_error((format("Cannot create output file %1%") % outputFileName).str());
			}

			size_t sizeRemaining = fileSize;

			for (lsn_t sector = 0; sector < stat->secsize; ++sector) {
				memset(buffer, 0, blockSize);

				driver_return_code_t r;
				if (form2File) {
					r = cdio_read_mode2_sector(image, buffer, stat->lsn + sector, true);
				} else {
					r = cdio_read_data_sectors(image, buffer, stat->lsn + sector, blockSize, 1);
				}
				if (r != DRIVER_OP_SUCCESS) {
					cerr << format("Error reading sector %1% of image file: %2%") % (stat->lsn + sector) % cdio_driver_errmsg(r) << endl;
					cerr << format("Output file %1% may be incomplete") % outputFileName << endl;
					break;
				}

				size_t sizeToWrite = sizeRemaining > blockSize ? blockSize : sizeRemaining;

				file.write(buffer, sizeToWrite);
				if (!file) {
					throw runtime_error((format("Cannot write to file %1%") % outputFileName).str());
				}

				sizeRemaining -= sizeToWrite;
			}
			file.close();
		}
	}

	// Close the catalog record for the directory
	catalog << string(level * 2, ' ') << "}\n";

	_cdio_list_free(entries, true, (CdioDataFree_t) iso9660_stat_free);
}


// Dump image to system area data, catalog file, and output directory.
static void dumpImage(CdIo_t * image, const boost::filesystem::path & outputPath, bool writeLBNs, bool writeGUADs, int postgapSectors, int postgapType)
{
	// Read ISO volume information
	iso9660_pvd_t pvd;
	if (!iso9660_fs_read_pvd(image, &pvd)) {
		throw runtime_error("Error reading ISO 9660 volume information");
	}
	cout << "Volume ID = " << iso9660_get_volume_id(&pvd) << endl;

	// Construct names of output files
	boost::filesystem::path catalogName = outputPath;
	catalogName.replace_extension(".cat");

	boost::filesystem::path systemAreaName = outputPath;
	systemAreaName.replace_extension(".sys");

	// Create output catalog file
	ofstream catalog(catalogName.generic_string(), ofstream::out | ofstream::trunc);
	if (!catalog) {
		throw runtime_error((format("Cannot create catalog file %1%") % catalogName).str());
	}

	// Dump system area data
	dumpSystemArea(image, systemAreaName);

	cout << "System area data written to " << systemAreaName << "\n";

	catalog << "system_area {\n";
	catalog << "  file " << systemAreaName << "\n";
	catalog << "}\n\n";

	// Output ISO volume information
	catalog << "volume {\n";
	catalog << "  system_id [" << iso9660_get_system_id(&pvd) << "]\n";
	catalog << "  volume_id [" << iso9660_get_volume_id(&pvd) << "]\n";
	catalog << "  volume_set_id [" << iso9660_get_volumeset_id(&pvd) << "]\n";
	catalog << "  publisher_id [" << iso9660_get_publisher_id(&pvd) << "]\n";
	catalog << "  preparer_id [" << iso9660_get_preparer_id(&pvd) << "]\n";
	catalog << "  application_id [" << iso9660_get_application_id(&pvd) << "]\n";
	catalog << "  copyright_file_id [" << vcdinfo_strip_trail(pvd.copyright_file_id, 37) << "]\n";
	catalog << "  abstract_file_id [" << vcdinfo_strip_trail(pvd.abstract_file_id, 37) << "]\n";
	catalog << "  bibliographic_file_id [" << vcdinfo_strip_trail(pvd.bibliographic_file_id, 37) << "]\n";
	catalog << "  creation_date "; print_ltime(catalog, pvd.creation_date, true);
	catalog << "  modification_date "; print_ltime(catalog, pvd.modification_date);
	catalog << "  expiration_date "; print_ltime(catalog, pvd.expiration_date);
	catalog << "  effective_date "; print_ltime(catalog, pvd.effective_date);
	catalog << "  postgap_sectors " << postgapSectors << "\n";
	catalog << "  postgap_type " << postgapType << "\n";
	catalog << "  audio_sectors " << audioSectors << "\n";
	catalog << "}\n\n";

	// Dump ISO filesystem
	if (!iso9660_fs_read_superblock(image, ISO_EXTENSION_NONE)) {
		throw runtime_error("Error reading ISO 9660 volume information");
	}

	cout << "Dumping filesystem to directory " << outputPath << "...\n";
	dumpFilesystem(image, catalog, writeLBNs, writeGUADs, outputPath);

	// Close down
	cout << "Catalog written to " << catalogName << "\n";
}


// Dump an LBN table of the image to the given output stream.
static void dumpLBNTable(CdIo_t * image, const string & inputPath = "", ostream & output = cout)
{
	// Read the directory entries
	CdioList_t * entries = iso9660_fs_readdir(image, inputPath.c_str());
	if (!entries) {
		throw runtime_error((format("Error reading ISO 9660 directory '%1%'") % inputPath).str());
	}

	// Print table header before root directory
	if (inputPath.empty()) {
		output << boost::format("%8s %8s %8s T Path") % "LBN" % "NumSec" % "Size" << endl;
	}

	// Print entry for directory itself
	iso9660_stat_t * stat = static_cast<iso9660_stat_t *>(_cdio_list_node_data(_cdio_list_begin(entries)));  // "." entry
	output << boost::format("%08x %08x %08x d %s") % stat->lsn % stat->secsize % stat->size % inputPath << endl;

	// Sort entries by sector number
	vector<iso9660_stat_t *> sortedChildren;

	CdioListNode_t * entry;
	_CDIO_LIST_FOREACH(entry, entries) {
		sortedChildren.push_back(static_cast<iso9660_stat_t *>(_cdio_list_node_data(entry)));
	}

	sort(sortedChildren.begin(), sortedChildren.end(), CmpByLSN());

	// Print all directory entries
	for (vector<iso9660_stat_t *>::const_iterator i = sortedChildren.begin(); i != sortedChildren.end(); ++i) {
		stat = *i;

		string entryName = stat->filename;
		size_t versionSep = entryName.find_last_of(';');
		if (versionSep != string::npos) {
			entryName = entryName.substr(0, versionSep);  // strip version number
		}

		string entryPath = inputPath.empty() ? entryName : (inputPath + "/" + entryName);

		if (stat->type == iso9660_stat_s::_STAT_DIR) {

			// Entry is a directory, recurse into it unless it is "." or ".."
			if (entryName != "." && entryName != "..") {
				dumpLBNTable(image, entryPath, output);
			}

		} else {

			// Entry is a file
			size_t fileSize = stat->size;
			char typeChar = 'f';

			if (stat->b_xa) {
				uint16_t attr = uint16_from_be(stat->xa.attributes);
				if (attr & (XA_ATTR_MODE2FORM2 | XA_ATTR_INTERLEAVED)) {
					typeChar = 'x';
					fileSize = stat->secsize * M2RAW_SECTOR_SIZE;
				}
				if (attr & XA_ATTR_CDDA) {
					typeChar = 'a';
				}
			}

			output << boost::format("%08x %08x %08x %c %s") % stat->lsn % stat->secsize % fileSize % typeChar % entryPath << endl;
		}
	}
}


// Print usage information and exit.
static void usage(const char * progname, int exitcode = 0, const string & error = "")
{
	cout << "Usage: " << boost::filesystem::path(progname).filename().generic_string() << " [OPTION...] <input>[.bin/cue] [<output_dir>]" << endl;
	cout << "  -l, --lbns                      Write LBNs to catalog file" << endl;
	cout << "  -g, --guads                     Write GIDs, UIDs, ATTRibutes and the DATEs to catalog file" << endl;
	cout << "  -t, --lbn-table                 Print LBN table and exit" << endl;
	cout << "  -v, --verbose                   Be verbose" << endl;
	cout << "  -V, --version                   Display version information and exit" << endl;
	cout << "  -?, --help                      Show this help message" << endl;

	if (!error.empty()) {
		cerr << endl << "Error: " << error << endl;
	}

	exit(exitcode);
}


// Main program
int main(int argc, const char ** argv)
{
	// Parse command line arguments
	boost::filesystem::path inputPath;
	boost::filesystem::path outputPath;
	bool writeLBNs = false;
	bool writeGUADs = false;
	bool printLBNTable = false;

	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];

		if (arg == "--version" || arg == "-V") {
			cout << TOOL_VERSION << endl;
			return 0;
		} else if (arg == "--lbns" || arg == "-l") {
			writeLBNs = true;
		} else if (arg == "--guads" || arg == "-g") {
			writeGUADs = true;
		} else if (arg == "--lbn-table" || arg == "-t") {
			printLBNTable = true;
		} else if (arg == "--verbose" || arg == "-v") {
			cdio_loglevel_default = CDIO_LOG_INFO;
		} else if (arg == "--help" || arg == "-?") {
			usage(argv[0]);
		} else if (arg[0] == '-') {
			usage(argv[0], 64, "Invalid option '" + arg + "'");
		} else {
			if (inputPath.empty()) {
				inputPath = arg;
			} else if (outputPath.empty()) {
				outputPath = arg;
			} else {
				usage(argv[0], 64, "Unexpected extra argument '" + arg + "'");
			}
		}
	}

	if (inputPath.empty()) {
		usage(argv[0], 64, "No input image specified");
	}

	if (outputPath.empty()) {
		outputPath = inputPath;
		outputPath.replace_extension("");
	}

	try {

		// Open the input image
		if (inputPath.extension().empty()) {
			inputPath.replace_extension(".bin");
		}

		// HACK! Since libcdio has issues with mixed cd's in the bin/cue format and i can't find the problem.
		// This strips the audio component of the cue file and have it process.
		boost::filesystem::path backupFile = inputPath;
		backupFile.replace_extension(inputPath.extension().string() + ".original");
		if (inputPath.extension() == ".cue") {
			if (boost::filesystem::exists(inputPath)) {
				if (!boost::filesystem::exists(backupFile)) {
					boost::filesystem::rename(inputPath, backupFile);
					boost::filesystem::copy_file(backupFile, inputPath);
				}
				std::string cueFile = backupFile.generic_string();
				std::string tempFile = inputPath.generic_string();
				std::ifstream cue(cueFile);
				std::ofstream temp(tempFile);
				std::string line;
				std::string fileName;
				bool firstTrackFound = false;
				while (std::getline(cue, line, '\n')) {
					if (line.find("FILE") != std::string::npos) {
						temp << line << std::endl;
					}
					else if (line.find("TRACK 01") != std::string::npos) {
						temp << line << std::endl;
						firstTrackFound = true;
					}
					else if (line.find("INDEX 01") != std::string::npos) {
						temp << line << std::endl;
						if(firstTrackFound) break;
					}
				}
				temp.close();
				cue.close();
			}
		}

		CdIo_t * image = cdio_open(inputPath.generic_string().c_str(), DRIVER_BINCUE);

		boost::filesystem::remove(inputPath);
		boost::filesystem::rename(backupFile, inputPath);


		if (image == NULL) {
			throw runtime_error((format("Error opening input image %1%, or image has wrong type") % inputPath).str());
		}

		cout << "Analyzing image " << inputPath << "...\n";

		// Get total sector count of track 1
		lsn_t last_lsnTrack1 = cdio_get_track_last_lsn(image, 1) + 1; // Zero based, so 0 is sector 1
		cdio_info("Track 1 sector count: %d", last_lsnTrack1);

		// Get postgap sector count of track 1
		msf_t msfTrack1;
		cdio_get_track_msf(image, 1, &msfTrack1);
		int postgapSectors = msfTrack1.m * CDIO_CD_SECS_PER_MIN * CDIO_CD_FRAMES_PER_SEC + msfTrack1.s * CDIO_CD_FRAMES_PER_SEC + msfTrack1.f;
		cdio_info("Postgap sectors: %d", postgapSectors);
		
		// Identify type of postgap
		int postgapType;
		
		if (postgapSectors > 1) {
			char postGapBuffer[CDIO_CD_FRAMESIZE_RAW];
			driver_return_code_t r = cdio_read_audio_sectors(image, postGapBuffer, (last_lsnTrack1 - 1), 1);
			if (r != DRIVER_OP_SUCCESS) {
				throw runtime_error((format("Error reading sector %1% of image file: %2%") % (last_lsnTrack1 - 1) % cdio_driver_errmsg(r)).str());
			}
			std::regex postgapType1("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000000000000000(00)*$"); // Empty
			std::regex postgapType2("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000200000002000(00)*$"); // Mode2
			std::regex postgapType3("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000200000002000(00)*([0-9a-f]{8})$"); // Mode2 with EDC
			std::vector<uint8_t> postGapVector(postGapBuffer, postGapBuffer + CDIO_CD_FRAMESIZE_RAW);
			std::string postGapHex = toHexString(postGapVector.data(), postGapVector.size());

			if (std::regex_match(postGapHex, postgapType1)) {
				postgapType = 1;
			} else if (std::regex_match(postGapHex, postgapType2)) {
				postgapType = 2;
			} else if (std::regex_match(postGapHex, postgapType3)) {
				postgapType = 3;
			} else {
				postgapType = 0; // Unknown, possibly dump it raw. Need to write that.
			}
			cdio_info("Postgap type: %d", postgapType);
		}
		
		// Get the number of tracks and total sector sizes of each track including pregap/postgap
		int trackNumber = 0;
		std::ifstream cueFile(inputPath.generic_string());
		std::string line;
		std::vector<std::string> binFiles;
		while (std::getline(cueFile, line)) {
			if (line.substr(0, 4) == "FILE") {
				size_t start = line.find("\"") + 1;
				size_t end = line.find("\"", start);
				binFiles.push_back(line.substr(start, end - start));
			}
		}
		cueFile.close();

		for (const auto & binFile : binFiles) {
			trackNumber++;
			boost::filesystem::path filePath(binFile);
			std::string binFileWithoutPath = filePath.filename().string();
			std::ifstream file;
			int size, sectors;
			if (boost::filesystem::exists(filePath)) {
				file.open(binFile, std::ios::binary);
			} else if (boost::filesystem::exists(binFileWithoutPath)) {
				std::cout << "WARNING! Incorrect path in .CUE file. However .BIN file was found in the same directory as the .CUE" << std::endl;
				file.open(binFileWithoutPath, std::ios::binary);
			} else {
				std::cout << "Error: " << binFile << " or " << binFileWithoutPath << " does not exist or is not readable." << std::endl;
				continue;
			}

			file.seekg(0, std::ios::end);
			size = file.tellg();
			sectors = size / CDIO_CD_FRAMESIZE_RAW;
			file.close();
			std::cout << "Number of sectors in " << binFileWithoutPath << ": " << sectors << std::endl;

			if (trackNumber > 1) {
				audioSectors += sectors;
			}
		}

		cdio_info("Audio sectors = %d", audioSectors);

		// Is it the correct type?
		discmode_t discMode = cdio_get_discmode(image);
		cdio_info("Disc mode = %d", discMode);
		switch (discMode) {
			case CDIO_DISC_MODE_CD_DATA:
			case CDIO_DISC_MODE_CD_XA:
			case CDIO_DISC_MODE_CD_MIXED:
				break;

			default:
				throw runtime_error("Input image is not a CD-ROM data disc");
		}

		track_t firstTrack = cdio_get_first_track_num(image);
		cdio_info("First track = %d", firstTrack);
		if (firstTrack == CDIO_INVALID_TRACK) {
			throw runtime_error("Cannot determine first track number");
		}

		track_format_t trackFormat = cdio_get_track_format(image, firstTrack);
		cdio_info("Track format = %d", trackFormat);
		if (trackFormat != TRACK_FORMAT_DATA && trackFormat != TRACK_FORMAT_XA) {
			throw runtime_error((format("First track (%1%) is not a data track") % firstTrack).str());
		}

		msf_t startMSF;
		cdio_get_track_msf(image, firstTrack, &startMSF);
		lsn_t startLSN = firstTrack == 1 ? 0 : cdio_msf_to_lsn(&startMSF);
		cdio_info("Start LSN of session = %d", startLSN);

		cdio_fs_anal_t type;
		cdio_iso_analysis_t isoType;
		type = cdio_guess_cd_type(image, startLSN, firstTrack, &isoType);
		cdio_info("Filesystem type = %04x", type);
		if (CDIO_FSTYPE(type) != CDIO_FS_ISO_9660) {
			throw runtime_error("No ISO 9660 filesystem on data track");
		}

		if (printLBNTable) {

			// Print the LBN table
			dumpLBNTable(image);

		} else {

			// Dump the input image
			dumpImage(image, outputPath, writeLBNs, writeGUADs, postgapSectors, postgapType);
		}

		// Close the input image
		cdio_destroy(image);
		cdio_info("Done.");

	} catch (const std::exception & e) {
		cerr << e.what() <<endl;
		return 1;
	}

	return 0;
}
