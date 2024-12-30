//
// PSXRip - Disassemble a PlayStation 1 disc image into its contents
//
// Copyright (C) Christian Bauer <www.cebix.net>
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

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/info.h>
#include <libvcd/sector.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <string>
#include <stdexcept>
#include <stdio.h>
#include <time.h>
#include <vector>
namespace fs = std::filesystem;
using namespace std;

#define TOOL_VERSION "PSXRip v2.2.6 (Win32 build by ^Ripper)"

#ifdef _WIN32
	#define timegm _mkgmtime
#endif

bool fixAllDates = false;
bool writeStrict = false;

fs::path psxripDir;

// Y2k / root entry processing error
struct tm rootEntryReplacementTm;

// Sector buffer
static char buffer[M2RAW_SECTOR_SIZE];
static char bufferRAW[CDIO_CD_FRAMESIZE_RAW];

// Gzip compress and Base64 encode text string
std::string base64_encode(const std::string& input) {
	const std::string base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string base64EncodedContent;
	size_t i = 0;
	size_t j = 0;
	uint8_t charArray3[3];
	uint8_t charArray4[4];

	for (uint8_t byte : input) {
		charArray3[i++] = byte;
		if (i == 3) {
			charArray4[0] = (charArray3[0] & 0xfc) >> 2;
			charArray4[1] = ((charArray3[0] & 0x03) << 4) + ((charArray3[1] & 0xf0) >> 4);
			charArray4[2] = ((charArray3[1] & 0x0f) << 2) + ((charArray3[2] & 0xc0) >> 6);
			charArray4[3] = charArray3[2] & 0x3f;

			for (i = 0; i < 4; ++i) {
				base64EncodedContent += base64Chars[charArray4[i]];
			}
			i = 0;
		}
	}

	if (i) {
		for (j = i; j < 3; ++j) {
			charArray3[j] = '\0';
		}

		charArray4[0] = (charArray3[0] & 0xfc) >> 2;
		charArray4[1] = ((charArray3[0] & 0x03) << 4) + ((charArray3[1] & 0xf0) >> 4);
		charArray4[2] = ((charArray3[1] & 0x0f) << 2) + ((charArray3[2] & 0xc0) >> 6);

		for (j = 0; j < i + 1; ++j) {
			base64EncodedContent += base64Chars[charArray4[j]];
		}

		while (i++ < 3) {
			base64EncodedContent += '=';
		}
	}

	return base64EncodedContent;
}

// Print an ISO long-format time structure to a file.
static void print_ltime(ofstream & f, const iso9660_ltime_t & l, bool creation_time = false)
{
	// Assuming Y2K bug on creation_date / root directory record on some ISO's where the PVD date year is reported as 0000 instead of 19xx or 20xx.
	// stat->tm Bug on root record. When the date on the filesystem reads 00 hex instead of the years after 1900 in hex. This messes up epoch calculations.
	// All the stat->tm.xxx fields are corrupted, so we need a replacement. Luckely the pvd.creation_date is correct all but the year.
	std::string century_str = {l.lt_year[0], l.lt_year[1]};
	std::string year_str = {l.lt_year[2], l.lt_year[3]};
	std::string month_str = {l.lt_month[0], l.lt_month[1]};
	std::string day_str = {l.lt_day[0], l.lt_day[1]};
	std::string hour_str = {l.lt_hour[0], l.lt_hour[1]};
	std::string minute_str = {l.lt_minute[0], l.lt_minute[1]};
	std::string second_str = {l.lt_second[0], l.lt_second[1]};

	if (creation_time == true) {
		rootEntryReplacementTm.tm_year = std::stoi(year_str);
		rootEntryReplacementTm.tm_mon = std::stoi(month_str) - 1;
		rootEntryReplacementTm.tm_mday = std::stoi(day_str);
		rootEntryReplacementTm.tm_hour = std::stoi(hour_str);
		rootEntryReplacementTm.tm_min = std::stoi(minute_str);
		rootEntryReplacementTm.tm_sec = std::stoi(second_str);
		if (rootEntryReplacementTm.tm_year < 70 && fixAllDates == true) { 
			rootEntryReplacementTm.tm_year = rootEntryReplacementTm.tm_year += 100;
		}
	}

	if (fixAllDates == true) {
		if ((century_str == "00" || century_str == "19") && std::stoi(day_str) >= 1) {
			if (std::stoi(year_str) >= 70) {
				century_str = "19";
			} else {
				century_str = "20";
			}
		}
	}

	f << format("{:.2}{:.2}-{:.2}-{:.2} {:.2}:{:.2}:{:.2}.{:.2} {}",
		century_str, year_str, l.lt_month, l.lt_day,
		l.lt_hour, l.lt_minute, l.lt_second, l.lt_hsecond,
		int(l.lt_gmtoff)) << endl;
}


// Dump system area data from image to file.
static void dumpSystemArea(CdIo_t * image, const fs::path & fileName)
{
	ofstream file(fileName, ofstream::out | ofstream::binary | ofstream::trunc);
	if (!file) {
		throw runtime_error(format("Cannot create system area file {}\n", fileName.string()));
	}

	const size_t numSystemAreaSectors = 16;
	for (size_t sector = 0; sector < numSystemAreaSectors; ++sector) {
		driver_return_code_t r = cdio_read_audio_sector(image, bufferRAW, sector);
		if (r != DRIVER_OP_SUCCESS) {
			throw runtime_error(format("Error reading sector {} of image file: {}", sector, cdio_driver_errmsg(r)));
		}

		file.write(bufferRAW, CDIO_CD_FRAMESIZE_RAW);
		if (!file) {
			throw runtime_error(format("Cannot write to system area file {}", fileName.string()));
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
static void dumpFilesystem(CdIo_t * image, ofstream & catalog, bool writeLBNs,
						   const fs::path & outputPath, const string & inputPath = "",
						   const string & dirName = "", unsigned level = 0)
{
	cdio_info("Dumping '%s' as '%s'", inputPath.c_str(), dirName.c_str());

	// Read the directory entries
	CdioList_t * entries = iso9660_fs_readdir(image, inputPath.c_str());
	if (!entries) {
		throw runtime_error(format("Error reading ISO 9660 directory '{}'", inputPath));
	}

	// Create the output directory
	fs::path outputDirName = outputPath / dirName;
	fs::create_directory(outputDirName);

	// Open the catalog record for the directory
	CdioListNode_t *node = _cdio_list_begin(entries);

	time_t directoryEpochSelf, directoryEpochParent;
	char datestringSelf[32], datestringParent[32];
	int y2k = 0;

	// Process first directory entry "."
	iso9660_stat_t *statSelf = static_cast<iso9660_stat_t *>(_cdio_list_node_data(node));
	// Fix broken Y2K dates and the mess libcdio makes with that.
	if (statSelf->tm.tm_year < 70) {
		statSelf->tm.tm_year = rootEntryReplacementTm.tm_year;
		statSelf->tm.tm_mon = rootEntryReplacementTm.tm_mon;
		statSelf->tm.tm_mday = rootEntryReplacementTm.tm_mday;
		statSelf->tm.tm_sec = rootEntryReplacementTm.tm_sec;
		statSelf->tm.tm_min = rootEntryReplacementTm.tm_min;
		statSelf->tm.tm_hour = rootEntryReplacementTm.tm_hour;
		strftime(datestringSelf, sizeof(datestringSelf), "%Y%m%d%H%M%S", &statSelf->tm);
	} else {
		directoryEpochSelf = timegm(&statSelf->tm); 
		directoryEpochSelf = directoryEpochSelf + (statSelf->timezone * 15 * 60);
		tm* time_info_self = gmtime(&directoryEpochSelf);
		struct tm time_info_val_self = *time_info_self;
		strftime(datestringSelf, sizeof(datestringSelf), "%Y%m%d%H%M%S", &time_info_val_self);
	}
	if (statSelf->y2kbug == 1 && fixAllDates == false) {
		y2k += 1;
	}

	node = _cdio_list_node_next(node);

	// Process second directory entry ".."
	iso9660_stat_t *statParent = static_cast<iso9660_stat_t *>(_cdio_list_node_data(node));
	if (statParent->tm.tm_year < 70) {
		statParent->tm.tm_year = rootEntryReplacementTm.tm_year;
		statParent->tm.tm_mon = rootEntryReplacementTm.tm_mon;
		statParent->tm.tm_mday = rootEntryReplacementTm.tm_mday;
		statParent->tm.tm_sec = rootEntryReplacementTm.tm_sec;
		statParent->tm.tm_min = rootEntryReplacementTm.tm_min;
		statParent->tm.tm_hour = rootEntryReplacementTm.tm_hour;
		strftime(datestringParent, sizeof(datestringParent), "%Y%m%d%H%M%S", &statParent->tm);
	} else {
		directoryEpochParent = timegm(&statParent->tm); 
		directoryEpochParent = directoryEpochParent + (statParent->timezone * 15 * 60);
		tm* time_info_parent = gmtime(&directoryEpochParent);
		struct tm time_info_val_parent = *time_info_parent;
		strftime(datestringParent, sizeof(datestringParent), "%Y%m%d%H%M%S", &time_info_val_parent);
	}
	if (statParent->y2kbug == 1 && fixAllDates == false) {
		y2k += 10;
	}

	if (level == 0) {
		catalog << "dir";  // root
		if (writeLBNs) {
			catalog << " @" << statSelf->lsn;
		}
		catalog << " GID" << _byteswap_ushort(statSelf->xa.group_id);
		catalog << " UID" << _byteswap_ushort(statSelf->xa.user_id);
		catalog << " ATRS" << _byteswap_ushort(statSelf->xa.attributes);
		catalog << " ATRP" << _byteswap_ushort(statParent->xa.attributes);
		catalog << " DATES" << datestringSelf;
		catalog << " DATEP" << datestringParent;
		catalog << " TIMEZONES" << std::to_string(statSelf->timezone);
		catalog << " TIMEZONEP" << std::to_string(statParent->timezone);
		catalog << " HIDDEN" << statSelf->hidden;
		catalog << " Y2KBUG" << y2k;
		catalog << " {\n";
	} else {
		catalog << string(level * 2, ' ') << "dir " << dirName;
		if (writeLBNs) {
			catalog << " @" << statSelf->lsn;
		}
		catalog << " GID" << _byteswap_ushort(statSelf->xa.group_id);
		catalog << " UID" << _byteswap_ushort(statSelf->xa.user_id);
		catalog << " ATRS" << _byteswap_ushort(statSelf->xa.attributes);
		catalog << " ATRP" << _byteswap_ushort(statParent->xa.attributes);
		catalog << " DATES" << datestringSelf;
		catalog << " DATEP" << datestringParent;
		catalog << " TIMEZONES" << std::to_string(statSelf->timezone);
		catalog << " TIMEZONEP" << std::to_string(statParent->timezone);
		catalog << " HIDDEN" << statSelf->hidden;
		catalog << " Y2KBUG" << y2k;
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
				dumpFilesystem(image, catalog, writeLBNs, outputDirName, entryPath, entryName, level + 1);
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
					cdio_info("DA file '%s' size = %u, secsize = %u, group_id = %d, user_id = %d, attributes = %04x, filenum = %d",
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
			catalog << " GID" << _byteswap_ushort(stat->xa.group_id);
			catalog << " UID" << _byteswap_ushort(stat->xa.user_id);
			catalog << " ATR" << _byteswap_ushort(stat->xa.attributes);
			catalog << " DATE" << datestringEntry;
			catalog << " TIMEZONE" << std::to_string(stat->timezone);
			catalog << " SIZE" << stat->size;
			catalog << " HIDDEN" << stat->hidden;
			catalog << " Y2KBUG" << stat->y2kbug;

			// Dump the file contents
			fs::path outputFileName = outputDirName / entryName;
			ofstream file(outputFileName, ofstream::out | ofstream::binary | ofstream::trunc);
			if (!file) {
				throw runtime_error(format("Cannot create output file {}", outputFileName.string()));
			}

			size_t sizeRemaining = fileSize;

			bool edcTest = false;

			for (uint32_t sector = 0; sector < stat->secsize; ++sector) {
				driver_return_code_t r;
				if (form2File) {
					// Check for EDC status. Tricky one as XA files can have audio and video sectors. Mode 2-1/Mode 2-2 interleaved. So until a positive hit, keep scanning.
					if (edcTest == false) {
						r = cdio_read_audio_sector(image, bufferRAW, stat->lsn + sector);
						if (r != DRIVER_OP_SUCCESS) {
							cerr << format("Error reading sector {} of image file: {}", stat->lsn + sector, cdio_driver_errmsg(r)) << endl;
							cerr << format("Output file {} may be incomplete", outputFileName.string()) << endl;
							break;
						}
						if ((bufferRAW[18] & 0x20) == 0x20 && bufferRAW[2348] == '\0' && bufferRAW[2349] == '\0' && bufferRAW[2350] == '\0' &&	bufferRAW[2351] == '\0') { // If bit 6 is "1" and the last 4 bytes are "0" then its Mode 2/Form 2 with zeroed out EDC.
							edcTest = true;
						}
					}
					r = cdio_read_mode2_sector(image, buffer, stat->lsn + sector, 1);
				} else if (cddaFile) {
					cdio_info("Skipping CD-DA file...");
					break;
				} else {
					r = cdio_read_data_sectors(image, buffer, stat->lsn + sector, blockSize, 1);
				}
				if (r != DRIVER_OP_SUCCESS) {
					cerr << format("Error reading sector {} of image file: {}", stat->lsn + sector, cdio_driver_errmsg(r)) << endl;
					cerr << format("Output file {} may be incomplete", outputFileName.string()) << endl;
					break;
				}

				size_t sizeToWrite = sizeRemaining > blockSize ? blockSize : sizeRemaining;

				file.write(buffer, sizeToWrite);
				if (!file) {
					throw runtime_error(format("Cannot write to file {}", outputFileName.string()));
				}

				sizeRemaining -= sizeToWrite;
			}
			if (edcTest == true && form2File) {
			 	catalog << " ZEROEDC" << "1";
			 	edcTest = false;
			} else if (form2File) {
			 	catalog << " ZEROEDC" << "0";
			}
			catalog << " \n";
			file.close();
		}
	}

	// Close the catalog record for the directory
	catalog << string(level * 2, ' ') << "}\n";

	_cdio_list_free(entries, true, (CdioDataFree_t) iso9660_stat_free);
}


// Dump image to system area data, catalog file, and output directory.
static void dumpImage(CdIo_t * image, const fs::path & outputPath, bool writeLBNs, string trackListingEncoded, int track1PostgapType, int track1SectorCount, int audioSectors)
{
	// Read ISO volume information
	iso9660_pvd_t pvd;
	if (!iso9660_fs_read_pvd(image, &pvd)) {
		throw runtime_error("Error reading ISO 9660 volume information");
	}
	cout << "Volume ID = " << iso9660_get_volume_id(&pvd) << endl;

	// Construct names of output files
	fs::path catalogName = outputPath;
	catalogName.replace_extension(".cat");

	fs::path systemAreaName = outputPath;
	systemAreaName.replace_extension(".sys");

	// Create output catalog file
	ofstream catalog(catalogName, ofstream::out | ofstream::trunc);
	if (!catalog) {
		throw runtime_error(format("Cannot create catalog file {}", catalogName.string()));
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
	catalog << "  track_listing [" << trackListingEncoded << "]\n";
	catalog << "  track1_sector_count " << track1SectorCount << "\n";
	catalog << "  track1_postgap_type " << track1PostgapType << "\n";
	catalog << "  audio_sectors " << audioSectors << "\n";
	catalog << "  strict_rebuild " << (writeStrict ? "1" : "0") << "\n";
	catalog << "}\n\n";

	// Dump ISO filesystem
	if (!iso9660_fs_read_superblock(image, ISO_EXTENSION_NONE)) {
		throw runtime_error("Error reading ISO 9660 volume information");
	}

	cout << "Dumping filesystem to directory " << outputPath << "...\n";
	dumpFilesystem(image, catalog, writeLBNs, outputPath);

	// Close down
	cout << "Catalog written to " << catalogName << "\n";
}


// Dump an LBN table of the image to the given output stream.
static void dumpLBNTable(CdIo_t * image, const string & inputPath = "", ostream & output = cout)
{
	// Read the directory entries
	CdioList_t * entries = iso9660_fs_readdir(image, inputPath.c_str());
	if (!entries) {
		throw runtime_error(format("Error reading ISO 9660 directory '{}'", inputPath));
	}

	// Print table header before root directory
	if (inputPath.empty()) {
		output << format("{:>8} {:>8} {:>8} T Path", "LBN", "NumSec", "Size") << endl;
	}

	// Print entry for directory itself
	iso9660_stat_t * stat = static_cast<iso9660_stat_t *>(_cdio_list_node_data(_cdio_list_begin(entries)));  // "." entry
	output << format("{:08x} {:08x} {:08x} d {}", stat->lsn, stat->secsize, stat->size, inputPath) << endl;

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

			output << format("{:08x} {:08x} {:08x} {:c} {}", stat->lsn, stat->secsize, fileSize, typeChar, entryPath) << endl;
		}
	}
}


// Print usage information and exit.
static void usage(const char * progname, int exitcode = 0, const string & error = "")
{
	cout << "Usage: " << fs::path(progname).filename().string() << " [OPTION...] <input>[.bin/cue] [<output_dir>]" << endl;
	cout << "  -f, --fix                       Fix problematic file/directory/catalog dates" << endl;
	cout << "                                  instead of preserving them" << endl;
	cout << "  -l, --lbns                      Write LBNs to catalog file" << endl;
	cout << "  -s, --strict                    Rebuild writes to original LBN. Implied -l." << endl;
	cout << "                                  Oversized files get remapped." << endl;
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
	fs::path inputPath;
	fs::path outputPath;
	bool writeLBNs = false;
	bool printLBNTable = false;

	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];

		if (arg == "--version" || arg == "-V") {
			cout << TOOL_VERSION << endl;
			return 0;
		} else if (arg == "--fix" || arg == "-f") {
			fixAllDates = true;
		} else if (arg == "--lbns" || arg == "-l") {
			writeLBNs = true;
		} else if (arg == "--strict" || arg == "-s") {
			writeStrict = true;
			writeLBNs = true;
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

	fs::path psxripDir = outputPath / "_PSXRIP";

	try {

		// Open the input image (Force .cue extension on input argument! Libcdio will moan otherwise.)
		inputPath.replace_extension(".cue");

		cout << "Analyzing image " << inputPath << "...\n";

		bool isMultiBin = false;
		int trackCount = 0;
		int fileCount = 0;
		int audioSectors = 0;
		string trackListingEncoded = "";

		// Ensure the output directory exists
		if (!fs::exists(psxripDir)) {
			if (!fs::create_directories(psxripDir)) {
				printf("Failed to create output directory\n");
				return 1;
			}
		}

		if (fs::exists(inputPath)) {
			// Open the cue file and encode in base64 string.
			std::ifstream file(inputPath, std::ios::in | std::ios::binary | std::ios::ate);
			if (!file) {
					throw std::runtime_error("Failed to open the file: " + inputPath.string());
			}

			std::streamsize size = file.tellg();
			file.seekg(0, std::ios::beg);

			std::string cueContent(size, '\0');
			if (!file.read(&cueContent[0], size)) {
					throw std::runtime_error("Failed to read the file: " + inputPath.string());
			}

			file.close();

			std::regex trackRegex("(?:^|\\n)\\s*TRACK\\b", std::regex::icase);
			std::regex fileRegex("(?:^|\\n)\\s*FILE\\b", std::regex::icase);

			trackCount = std::distance(
				std::sregex_iterator(cueContent.begin(), cueContent.end(), trackRegex),
				std::sregex_iterator()
			);

			fileCount = std::distance(
				std::sregex_iterator(cueContent.begin(), cueContent.end(), fileRegex),
				std::sregex_iterator()
			);

			if (fileCount == 1 && trackCount >= 1) {
				isMultiBin = false; // Single-bin
			} else if (fileCount > 1 && fileCount == trackCount) {
				isMultiBin = true; // Multi-bin
			} else {
				throw std::runtime_error("Error: The .cue file is inconsistent. FILE and TRACK counts do not align.");
			}

			cdio_info("Cue file parsed. Files: %02d, Tracks: %02d, Multiple .BIN files: %s",	fileCount, trackCount, isMultiBin ? "true" : "false");

		} else {
			throw std::runtime_error("Error: '" + inputPath.string() + "' file not found.");
		}

		cdio_info("Libcdio track parser:");
		cdio_info("Track  Index 00  Index 01  Silence  Start LBA  Pregap  Data LBA  End LBA   Total  Filename");

 		CdIo_t * image = cdio_open(inputPath.generic_string().c_str(), DRIVER_BINCUE);

		if (image == NULL) {
			throw runtime_error(format("Error opening input image {}, or image has wrong type", inputPath.string()));
		}

		// Get the TOC (Table of Contents) of the CD
		track_t first_track = cdio_get_first_track_num(image);
		track_t last_track = cdio_get_last_track_num(image);
		if (last_track > first_track) {
			cout << "Dumping audio tracks to directory \"" << psxripDir.string() << "\"...\n";
		}

		// Iterate through the tracks to find the audio track
		lsn_t last_sector_track1_postgap = 0;
		// { "CDIO_DISC_MODE_CD_DA", "CDIO_DISC_MODE_CD_DATA", "CDIO_DISC_MODE_CD_XA", "CDIO_DISC_MODE_CD_MIXED", ... }
		discmode_t discMode = cdio_get_discmode(image);
		const char *track1Format = "";
		std::string csvTracks = "";

		cdio_info("PSXRip track reparser:");
		cdio_info("Track  Filesystem  Sector type      Start LBA  Pregap  Data LBA  End LBA   Total");

		for (track_t track = first_track; track <= last_track; track++) {
			track_format_t format = cdio_get_track_format(image, track);
			// { "AUDIO", "CD-i", "XA", "DATA", "PSX", "ERROR" }
			const char *format_str = (format >= 0 && format < 6) ? track_format2str[format] : "ERROR";
			if (track == 1) {
				track1Format = format_str;
			}

			trackmode_t format_mode = cdio_get_track_mode(image, track);
			// { "AUDIO", "MODE1/2048", "MODE1/2352", "MODE2/2336", "MODE2/2048", "MODE2/2324", "MODE2/2336", "MODE2/2352", "ERROR" }
			const char *format_mode_str = (format_mode >= 0 && format_mode < 9) ? track_format_mode2str[format_mode] : "ERROR";

			// Get the start, end and total sectors of the track
			// These are reliable and the values are the same as in libcdio's cue parser.
			// cdio_get_track_end_sector was created to remove hacky code.
			lsn_t pregap_sector = std::max(cdio_get_track_pregap_lba(image, track), 0); // Pregap size. Corrected if bogus negative number.
			lsn_t start_sector  = cdio_get_track_lba(image, track) - pregap_sector;     // Track start sector before pregap
			lsn_t data_sector   = cdio_get_track_lba(image, track);                     // Data start sector after the pregap (the actual data)
			lsn_t end_sector    = cdio_get_track_end_sector(image, track);              // Track ending sector.
			lsn_t total_sector  = end_sector - start_sector + 1;                        // Total sector amount for the track including pregap.
			// These are unreliable across different disc types and modes!
			// cdio_get_track_last_lsn()
			// cdio_get_track_sec_count()
			// Lead out is added to the last track and messes up the calculations.

			if (track == 1) {
				last_sector_track1_postgap = end_sector;
			}

			if (track == 2 && isMultiBin == false) {
				last_sector_track1_postgap = start_sector - 1;
			}

			cdio_info("%-5d  %-10s  %-15s  %9d  %6d  %8d  %7d  %6d",
									track, format_str, format_mode_str, start_sector, pregap_sector, data_sector, end_sector, total_sector);

			csvTracks += std::to_string(track) + "," +
			             std::string(format_mode_str) + "," +
			             std::to_string(start_sector) + "," +
			             std::to_string(pregap_sector) + "," +
			             std::to_string(data_sector) + "," +
			             std::to_string(end_sector) + "," +
			             std::to_string(total_sector) + "\n";

			// Check if the track is an audio track
			if (format == TRACK_FORMAT_AUDIO) {
				audioSectors += total_sector; // including pregap for whole .bin file.

				// Create a filename based on the track number
				char filename[50];
				sprintf(filename, "Track_%02d.wav", track);

				fs::path fullPathAudio = psxripDir / filename;
				std::string fullPathAudioStr = fullPathAudio.string();

				// Open a file to write the audio data
				FILE *audio_file = fopen(fullPathAudioStr.c_str(), "wb");
				if (audio_file == NULL) {
					printf("Failed to open output file\n");
					return 1;
				}

				// Calculate the size of the audio data in bytes
				uint32_t data_size = (total_sector - pregap_sector) * CDIO_CD_FRAMESIZE_RAW;

				const uint8_t wav_header[44] = {
					'R', 'I', 'F', 'F', 0, 0, 0, 0, // Chunk ID and Chunk Size (to be set)
					'W', 'A', 'V', 'E',             // Format
					'f', 'm', 't', ' ',             // Subchunk1 ID
					16, 0, 0, 0,                    // Subchunk1 Size (16 for PCM)
					1, 0,                           // Audio Format (1 for PCM)
					2, 0,                           // Num Channels (2 for stereo)
					0x44, 0xAC, 0x00, 0x00,         // Sample Rate (44100 Hz)
					0x10, 0xB1, 0x02, 0x00,         // Byte Rate (44100 * 2 * 16/8)
					4, 0,                           // Block Align (NumChannels * BitsPerSample/8)
					16, 0,                          // Bits per Sample (16)
					'd', 'a', 't', 'a', 0, 0, 0, 0  // Subchunk2 ID and Subchunk2 Size (to be set)
				};

				// Copy the static header and set file-specific sizes
				uint8_t header[44];
				memcpy(header, wav_header, 44);
				*(uint32_t *)(header + 4) = 36 + data_size; // Chunk Size
				*(uint32_t *)(header + 40) = data_size;     // Subchunk2 Size

				// Write WAV header
				fwrite(header, 1, 44, audio_file);

				// Read each sector and write audio data to the file (Including pregap! Can contain hidden data!)
				for (lsn_t sector = data_sector; sector <= end_sector; sector++) {
					driver_return_code_t r = cdio_read_audio_sector(image, bufferRAW, sector);
					if (r != DRIVER_OP_SUCCESS) {
							std::cerr << "Error reading sector " << sector << " of image file: " << cdio_driver_errmsg(r) << std::endl;
					}
					fwrite(bufferRAW, 1, CDIO_CD_FRAMESIZE_RAW, audio_file);
				}

				fclose(audio_file);
				
				// Now take care of the pregap
				if (pregap_sector > 0) {
					sprintf(filename, "Pregap_%02d.wav", track);

					fullPathAudio = psxripDir / filename;
					fullPathAudioStr = fullPathAudio.string();

					audio_file = fopen(fullPathAudioStr.c_str(), "wb");
					if (audio_file == NULL) {
						printf("Failed to open output file\n");
						return 1;
					}
					data_size = (total_sector - pregap_sector) * CDIO_CD_FRAMESIZE_RAW;
					memcpy(header, wav_header, 44);
					*(uint32_t *)(header + 4) = 36 + data_size;
					*(uint32_t *)(header + 40) = data_size;

					fwrite(header, 1, 44, audio_file);

					for (lsn_t sector = start_sector; sector < data_sector; sector++) {
						driver_return_code_t r = cdio_read_audio_sector(image, bufferRAW, sector);
						if (r != DRIVER_OP_SUCCESS) {
								std::cerr << "Error reading sector " << sector << " of image file: " << cdio_driver_errmsg(r) << std::endl;
						}
						fwrite(bufferRAW, 1, CDIO_CD_FRAMESIZE_RAW, audio_file);
					}

					fclose(audio_file);
				}
			}
		}

		// Base64 encode the cvsTracks for the catalog file.
		trackListingEncoded = base64_encode(csvTracks);

		// Identifying the postgap type of the data track.
		int track1PostgapType = 0;

		if (strcmp(track1Format, "XA") == 0) {
			driver_return_code_t r = cdio_read_audio_sector(image, bufferRAW, last_sector_track1_postgap);
			if (r != DRIVER_OP_SUCCESS) {
				throw runtime_error(format("Error reading sector {} of image file: {}", last_sector_track1_postgap, cdio_driver_errmsg(r)));
			}
			std::string postGapHex;
			for (size_t i = 0; i < CDIO_CD_FRAMESIZE_RAW; ++i) {
				postGapHex += std::format("{:02X}", bufferRAW[i]);
			}
			std::regex postgapType1("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000000000000000(00)*$"); // Empty
			std::regex postgapType2("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000200000002000(00)*$"); // Mode2
			std::regex postgapType3("^00FFFFFFFFFFFFFFFFFFFF00.{8}0000200000002000(00)*([0-9A-F]){8}$"); // Mode2 with EDC

			if (std::regex_match(postGapHex, postgapType1)) {
				track1PostgapType = 1;
			} else if (std::regex_match(postGapHex, postgapType2)) {
				track1PostgapType = 2;
			} else if (std::regex_match(postGapHex, postgapType3)) {
				track1PostgapType = 3;
			} else {
				track1PostgapType = 0; // Unknown or Empty with garbage in last sector. Dump last sector for rebuild.

				std::string lastSectorFile = "Last_sector.bin";
				fs::path fullPathLastSector = psxripDir / lastSectorFile;
				std::string fullPathLastSectorStr = fullPathLastSector.string();

				FILE *last_sector_file = fopen(fullPathLastSectorStr.c_str(), "wb");
				if (last_sector_file == NULL) {
					printf("Failed to open output file\n");
					return 1;
				}
				fwrite(bufferRAW, 1, CDIO_CD_FRAMESIZE_RAW, last_sector_file);
				fclose(last_sector_file);
			}
		}

		cdio_info("Track 2+ audio sectors = %d", audioSectors);

		// Is it the correct type?
		// discmode_t discMode = cdio_get_discmode(image); // Already declared earlier.
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
			throw runtime_error(format("First track ({}) is not a data track", firstTrack));
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
			dumpImage(image, outputPath, writeLBNs, trackListingEncoded, track1PostgapType, last_sector_track1_postgap + 1, audioSectors);
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
