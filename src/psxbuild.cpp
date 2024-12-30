//
// PSXBuild - Build a PlayStation 1 disc image
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
#include <cdio/iso9660.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/sector.h>
}

#include <algorithm>
#include <cstring>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <iterator>
#include <queue>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
namespace fs = std::filesystem;
using namespace std;

#define TOOL_VERSION "PSXBuild v2.2.6 (Win32 build by ^Ripper)"

#ifdef _WIN32
	#define timegm _mkgmtime
#endif

struct FileNode;
struct DirNode;
struct TrackInfo {
	int trackNumber;
	std::string trackType;
	int startSector;
	int pregapSectors;
	int dataOffset;
	int endSector;
	int totalSectors;
};

int audioSectors = 0;
int strictRebuild = 0;
int track1SectorCount = 0;
int track1SectorCountOffset = 0;
int track1PostgapType = 0;
int timeZone = 0;
int y2kbug = 0;

std::string track_listing = "";
std::vector<TrackInfo> tracks;

fs::path psxripDir;
  
// Mode 2 raw sector buffer
static char buffer[CDIO_CD_FRAMESIZE_RAW];

// Empty Form 2 sector and Raw.
static const uint8_t emptySector[M2F2_SECTOR_SIZE] = {0};
static const uint8_t emptySectorRAW[CDIO_CD_FRAMESIZE_RAW] = {0};

// Maximum number of sectors in an image
const uint32_t MAX_ISO_SECTORS = 74 * 60 * 75;  // 74 minutes

// Decode Base64 to string
std::string base64_decode(const std::string& encodedContent) {
	const std::string base64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string decodedContent;
	size_t i = 0;
	uint8_t charArray4[4];

	for (char c : encodedContent) {
		if (c == '=') {
			break;
		}

		charArray4[i++] = static_cast<uint8_t>(base64Chars.find(c));
		if (i == 4) {
			decodedContent += static_cast<char>((charArray4[0] << 2) | ((charArray4[1] & 0x30) >> 4));
			decodedContent += static_cast<char>(((charArray4[1] & 0x0F) << 4) | ((charArray4[2] & 0x3C) >> 2));
			decodedContent += static_cast<char>(((charArray4[2] & 0x03) << 6) | charArray4[3]);
			i = 0;
		}
	}

	return decodedContent;
}

std::vector<TrackInfo> parseTracksFromString(const std::string& input) {
	std::vector<TrackInfo> tracks;
	std::istringstream inputStream(input); // Treat the string as a stream
	std::string line;

	while (std::getline(inputStream, line)) { // Split into lines
		std::istringstream lineStream(line);
		std::string field;

		TrackInfo track;

		// Parse each field
		std::getline(lineStream, field, ',');
		track.trackNumber = std::stoi(field);

		std::getline(lineStream, field, ',');
		track.trackType = field;

		std::getline(lineStream, field, ',');
		track.startSector = std::stoi(field);

		std::getline(lineStream, field, ',');
		track.pregapSectors = std::stoi(field);

		std::getline(lineStream, field, ',');
		track.dataOffset = std::stoi(field);

		std::getline(lineStream, field, ',');
		track.endSector = std::stoi(field);

		std::getline(lineStream, field, ',');
		track.totalSectors = std::stoi(field);

		// Add the track to the list
		tracks.push_back(track);
	}

	return tracks;
}

std::string sectorsToTime(int sectors) {
	int minutes = sectors / (75 * 60);
	int seconds = (sectors / 75) % 60;
	int frames = sectors % 75;
	std::ostringstream timeStream;
	timeStream << std::setw(2) << std::setfill('0') << minutes << ":"
	           << std::setw(2) << std::setfill('0') << seconds << ":"
	           << std::setw(2) << std::setfill('0') << frames;
	return timeStream.str();
}

void generateCueFile(const std::vector<TrackInfo>& tracks, const fs::path& imageName, const fs::path& imageCueName) {
	// Set the track offset. Only positive offset for strict mode.
	int offset = (strictRebuild == 1) 
	           ? ((track1SectorCountOffset > 0) ? track1SectorCountOffset : 0)
	           : track1SectorCountOffset;
	// Open the .cue file for writing
	std::ofstream cueFile(imageCueName, std::ios::out | std::ios::trunc);
	if (!cueFile.is_open()) {
		throw std::runtime_error("Error creating .cue file: " + imageCueName.string());
	}

	// Write the FILE line (all tracks contained in one .bin file)
	cueFile << "FILE \"" << imageName.filename().string() << "\" BINARY\n";

	for (const auto& track : tracks) {
		// Write the TRACK line
		cueFile << "  TRACK " << std::setw(2) << std::setfill('0') << track.trackNumber
		        << " " << track.trackType << "\n";

		// Write the INDEX 00 line if pregap exists
		if (track.pregapSectors > 0) {
			int index00Sector = track.trackNumber == 1 ? track.startSector : track.startSector + offset;
			cueFile << "    INDEX 00 " << sectorsToTime(index00Sector) << "\n";
		}

		// Write the INDEX 01 line
		int index01Sector = track.trackNumber == 1 ? track.dataOffset : track.dataOffset + offset;
		cueFile << "    INDEX 01 " << sectorsToTime(index01Sector) << "\n";
	}

	cueFile.close();
	std::cout << "Cue file written to " << imageCueName << "..." << std::endl;
}

void writeAudioTracks(std::vector<TrackInfo>& tracks, const std::filesystem::path& inputPath, std::ofstream& image) {
	for (const auto& track : tracks) {
		if (track.trackType == "AUDIO") {
			// Generate the WAV filename using the track information
			// Process pregap if file exists
			std::string wavFileName = std::format("Pregap_{:02}.wav", track.trackNumber);
			std::filesystem::path fullPathWav = psxripDir / wavFileName;
			std::ifstream wavFile;
			char chunkHeader[4];
			uint32_t chunkSize;
			bool dataChunkFound = false;
			std::vector<char> buffer(4096);

			if (fs::exists(fullPathWav)) {
				// Stream the audio data from the WAV file instead of loading it all into memory
				wavFile.open(fullPathWav, std::ios::binary);
				if (!wavFile) {
					throw std::runtime_error("Error opening WAV file: " + fullPathWav.string());
				}

				// Locate the "data" chunk after the RIFF header
				wavFile.seekg(12);  // Skip RIFF, Chunk Size, and WAVE headers (4 + 4 + 4 bytes)

				while (wavFile.read(chunkHeader, 4)) {
					wavFile.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));

					if (std::strncmp(chunkHeader, "data", 4) == 0) {
						dataChunkFound = true;
						break;
					}

					wavFile.seekg(chunkSize, std::ios::cur);
				}

				if (!dataChunkFound) {
					throw std::runtime_error("Invalid WAV file (missing 'data' chunk): " + fullPathWav.string());
				}

				// Stream audio data in chunks to avoid high memory usage
				while (wavFile.read(buffer.data(), buffer.size()) || wavFile.gcount() > 0) {
					image.write(buffer.data(), wavFile.gcount());
					if (!image) {
						std::cerr << "Error writing audio data to image file for track: " << track.trackNumber << std::endl;
						return;
					}
				}

				wavFile.close();

			}

			// Process audio data
			wavFileName = std::format("Track_{:02}.wav", track.trackNumber);
			fullPathWav = psxripDir / wavFileName;

			cdio_info("Writing WAV file: \"%s\" as audio track %2d...", wavFileName.c_str(), track.trackNumber);

			wavFile.open(fullPathWav, std::ios::binary);
			if (!wavFile) {
				throw std::runtime_error("Error opening WAV file: " + fullPathWav.string());
			}

			wavFile.seekg(12);
			dataChunkFound = false;

			while (wavFile.read(chunkHeader, 4)) {
				wavFile.read(reinterpret_cast<char*>(&chunkSize), sizeof(chunkSize));

				if (std::strncmp(chunkHeader, "data", 4) == 0) {
					dataChunkFound = true;
					break;
				}

				wavFile.seekg(chunkSize, std::ios::cur);
			}

			if (!dataChunkFound) {
				throw std::runtime_error("Invalid WAV file (missing 'data' chunk): " + fullPathWav.string());
			}

			while (wavFile.read(buffer.data(), buffer.size()) || wavFile.gcount() > 0) {
				image.write(buffer.data(), wavFile.gcount());
				if (!image) {
					std::cerr << "Error writing audio data to image file for track: " << track.trackNumber << std::endl;
					return;
				}
			}

			wavFile.close();

		}
	}
}

// Convert string to integer.
static bool str_to_num(const string & s, auto & value)
{
	auto end = s.data() + s.size();
	auto result = from_chars(s.data(), end, value);
	return result.ec == errc() && result.ptr == end;
}


// Create an ISO long-format time structure from an ISO8601-like string
static void parse_ltime(const string & s, iso9660_ltime_t & t)
{
	static const regex timeSpec("(\\d{4})-(\\d{2})-(\\d{2})\\s+(\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{2})\\s+(\\d+)");
	smatch m;

	if (! regex_match(s, m, timeSpec)) {
		throw runtime_error(format("'{}' is not a valid date/time specification", s));
	}

	t.lt_year[0] = m.str(1)[0];
	t.lt_year[1] = m.str(1)[1];
	t.lt_year[2] = m.str(1)[2];
	t.lt_year[3] = m.str(1)[3];

	t.lt_month[0] = m.str(2)[0];
	t.lt_month[1] = m.str(2)[1];

	t.lt_day[0] = m.str(3)[0];
	t.lt_day[1] = m.str(3)[1];

	t.lt_hour[0] = m.str(4)[0];
	t.lt_hour[1] = m.str(4)[1];

	t.lt_minute[0] = m.str(5)[0];
	t.lt_minute[1] = m.str(5)[1];

	t.lt_second[0] = m.str(6)[0];
	t.lt_second[1] = m.str(6)[1];

	t.lt_hsecond[0] = m.str(7)[0];
	t.lt_hsecond[1] = m.str(7)[1];

	if (! str_to_num(m[8], t.lt_gmtoff)) {
		throw runtime_error(format("'{}' is not a valid GMT offset specification", m[8].str()));
	}
}


// Set an ISO long-format time structure to an empty value.
static void zero_ltime(iso9660_ltime_t & t)
{
	memset(&t, '0', sizeof(t));
	t.lt_gmtoff = 0;
}


// Base class for filesystem tree visitors
class Visitor {
public:
	Visitor() { }
	virtual ~Visitor() { }

	virtual void visit(FileNode &) { }
	virtual void visit(DirNode &) { }
};


// Base class for filesystem node
struct FSNode {
	FSNode(const string & name_, const fs::path & path_, DirNode * parent_, uint32_t startSector_ = 0)
		: parent(parent_), name(name_), path(path_), firstSector(0), numSectors(0), requestedStartSector(startSector_) { }

	virtual ~FSNode() { }

	// Pointer to parent directory node (NULL if root)
	DirNode * parent;

	// List of child nodes
	vector<FSNode *> children;

	// List of child nodes sorted by name
	vector<FSNode *> sortedChildren;

	// Node name
	string name;

	// Path to item in host filesystem
	fs::path path;

	// First logical sector number
	uint32_t firstSector;

	// Size in sectors
	uint32_t numSectors;

	// First logical sector number requested in catalog (0 = don't care)
	uint32_t requestedStartSector;

	// Polymorphic helper method for accepting a visitor
	virtual void accept(Visitor &) = 0;

	// Pre-order tree traversal
	void traverse(Visitor & v);

	// Pre-order tree traversal, children sorted by name
	void traverseSorted(Visitor & v);

	// Breadth-first tree traversal, children sorted by name
	void traverseBreadthFirstSorted(Visitor & v);
};


// Functor for sorting a container of FSNode pointers by name
struct CmpByName {
	bool operator()(const FSNode * lhs, const FSNode * rhs)
	{
		return lhs->name < rhs->name;
	}
};


// File (leaf) node
struct FileNode : public FSNode {
	FileNode(const string & name_, const fs::path & path_, DirNode * parent_, uint32_t startSector_ = 0, bool isForm2_ = false, bool isAudio_ = false, uint16_t nodeGID_ = 0, uint16_t nodeUID_ = 0, uint16_t nodeATR_ = 0, string nodeDate_ = "", int16_t nodeTimezone_ = 0, uint32_t nodeSize_ = 0, uint32_t nodeSizeOriginal_ = 0, bool nodeHidden_ = false, int nodeY2kbug_ = 0, bool nodeEDC_ = false)
		: FSNode(name_, path_, parent_, startSector_), isForm2(isForm2_), isAudio(isAudio_), nodeGID(nodeGID_), nodeUID(nodeUID_), nodeATR(nodeATR_), nodeDate(nodeDate_), nodeTimezone(nodeTimezone_), nodeSize(nodeSize_), nodeSizeOriginal(nodeSizeOriginal_), nodeHidden(nodeHidden_), nodeY2kbug(nodeY2kbug_), nodeEDC(nodeEDC_)
	{
		// Check for the existence of the file and obtain its size
		size = fs::file_size(path);

		// Calculate the number of sectors in the file extent
		size_t blockSize = isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
		numSectors = (size + blockSize - 1) / blockSize;

		if (numSectors == 0 && !isAudio) { // Disable sector count for DA tracks. They have to be processed seperately.
			numSectors = 1;  // empty files use one sector
		}
	}

	// The list
	uint16_t nodeGID;
	uint16_t nodeUID;
	uint16_t nodeATR;
	string nodeDate;
	string nodeDateParent;
	int16_t nodeTimezone;
	uint32_t nodeSize;
	uint32_t nodeSizeOriginal;
	bool nodeHidden;
	int nodeY2kbug;
	bool nodeEDC;

	// Size in bytes
	uint32_t size;

	// True if form 2 file
	bool isForm2;
	bool isAudio;

	void accept(Visitor & v) { v.visit(*this); }
};


// Directory node
struct DirNode : public FSNode {
	DirNode(const string & name_, const fs::path & path_, DirNode * parent_ = NULL, uint32_t startSector_ = 0, uint16_t nodeGID_ = 0, uint16_t nodeUID_ = 0, uint16_t nodeATR_ = 0, uint16_t nodeATRP_ = 0, string nodeDate_ = "", string nodeDateParent_ = "", int16_t nodeTimezone_ = 0, int16_t nodeTimezoneParent_ = 0, bool nodeHidden_ = false, int nodeY2kbug_ = 0)
		: FSNode(name_, path_, parent_, startSector_), data(NULL), recordNumber(0), nodeGID(nodeGID_), nodeUID(nodeUID_), nodeATR(nodeATR_), nodeATRP(nodeATRP_), nodeDate(nodeDate_), nodeDateParent(nodeDateParent_), nodeTimezone(nodeTimezone_), nodeTimezoneParent(nodeTimezoneParent_), nodeHidden(nodeHidden_), nodeY2kbug(nodeY2kbug_) { }

	// The list
	uint16_t nodeGID;
	uint16_t nodeUID;
	uint16_t nodeATR;
	uint16_t nodeATRP;
	string nodeDate;
	string nodeDateParent;
	int16_t nodeTimezone;
	int16_t nodeTimezoneParent;
	bool nodeHidden;
	int nodeY2kbug;

	// Pointer to directory extent data
	uint8_t * data;

	// Record number of directory in path table
	uint16_t recordNumber;

	void accept(Visitor & v) { v.visit(*this); }
};


// Pre-order tree traversal
void FSNode::traverse(Visitor & v)
{
	accept(v);

	for (vector<FSNode *>::const_iterator i = children.begin(); i != children.end(); ++i) {
		(*i)->traverse(v);
	}
}


// Pre-order tree traversal, children sorted by name
void FSNode::traverseSorted(Visitor & v)
{
	accept(v);

	for (vector<FSNode *>::const_iterator i = sortedChildren.begin(); i != sortedChildren.end(); ++i) {
		(*i)->traverseSorted(v);
	}
}


// Breadth-first tree traversal, children sorted by name
void FSNode::traverseBreadthFirstSorted(Visitor & v)
{
	queue<FSNode *> q;
	q.push(this);

	while (!q.empty()) {
		FSNode * node = q.front();
		q.pop();

		node->accept(v);

		for (vector<FSNode *>::const_iterator i = node->sortedChildren.begin(); i != node->sortedChildren.end(); ++i) {
			q.push(*i);
		}
	}
}


void flattenTree(FSNode* node, std::vector<FSNode*>& flatList) {
	// Add the current node to the flat list
	flatList.push_back(node);

	// Recursively process all children
	for (auto* child : node->children) {
		flattenTree(child, flatList);
	}
}


// Data from catalog file
struct Catalog {
	Catalog() : defaultUID(0), defaultGID(0), root(NULL)
	{
		zero_ltime(creationDate);
		zero_ltime(modificationDate);
		zero_ltime(expirationDate);
		zero_ltime(effectiveDate);
	}

	// Name of file containing system area data
	string systemAreaFile;

	// Volume information
	string systemID;
	string volumeID;
	string volumeSetID;
	string publisherID;
	string preparerID;
	string applicationID;
	string copyrightFileID;
	string abstractFileID;
	string bibliographicFileID;

	// Dates
	iso9660_ltime_t creationDate;
	iso9660_ltime_t modificationDate;
	iso9660_ltime_t expirationDate;
	iso9660_ltime_t effectiveDate;

	// Default user/group IDs
	uint16_t defaultUID;
	uint16_t defaultGID;

	// Root directory of the filesystem tree
	DirNode * root;
};


// Read the next non-empty line from a file, stripping leading and
// trailing whitespace. Returns an empty string if the end of the file was
// reached or an error occurred.
static string nextline(ifstream & file)
{
	string line;

	while (line.empty() && file) {
		getline(file, line);

		auto is_space = [](char c) -> bool { return std::isspace(static_cast<unsigned char>(c)); };
		auto trimmed = line | views::drop_while(is_space) | views::reverse | views::drop_while(is_space) | views::reverse;
		line = string(begin(trimmed), end(trimmed));
	}

	return line;
}


// Check that the given string only consists of d-characters.
static void checkDString(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_dchar(c)) {
			cerr << format("Warning: Illegal character '{}' in {} \"{}\"", c, description, s) << endl;
			break;
		}
	}
}


// Check that the given string only consists of a-characters.
static void checkAString(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_achar(c)) {
			cerr << format("Warning: Illegal character '{}' in {} \"{}\"", c, description, s) << endl;
			break;
		}
	}
}


// Check that the given string is a valid file name.
static void checkFileName(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_dchar(c) && c != '.') {
			throw runtime_error(format("Illegal character '{}' in {} \"{}\"", c, description, s));
		}
	}
}


static void convertToEpochTime(const std::string& date, time_t& epoch_time) {
	std::string new_date = date;
	if (new_date.substr(0, 2) == "00" || new_date.substr(0, 2) == "19") { // The Y2K problem where years are encoded 00 for year 2000 instead of 64 hex.
		if (std::stoi(new_date.substr(2, 2)) >= 70) {
			new_date.replace(0, 2, "19");
		} else {
			new_date.replace(0, 2, "20");
		}
	}

	tm time_info{};
	int year, month, day, hour, min, sec;

	sscanf(new_date.c_str(), "%04d%02d%02d%02d%02d%02d", &year, &month, &day, &hour, &min, &sec);
	time_info.tm_year = year - 1900;
	time_info.tm_mon = month - 1;
	time_info.tm_mday = day;
	time_info.tm_hour = hour;
	time_info.tm_min = min;
	time_info.tm_sec = sec;

	epoch_time = timegm(&time_info);
}

// Check that the given string represents a valid sector number and
// convert it to an integer. Returns 0 if the string is empty;
static uint32_t checkLBN(const string & s, const string & itemName)
{
	uint32_t lbn = 0;
	if (!s.empty()) {
		if (str_to_num(s, lbn)) {
			if (lbn <= ISO_EVD_SECTOR || lbn >= MAX_ISO_SECTORS) {
				throw runtime_error(format("Start LBN '{}' of '{}' is outside the valid range {}..{}", s, itemName, (unsigned) ISO_EVD_SECTOR, MAX_ISO_SECTORS));
			}
		} else {
			throw runtime_error(format("Invalid start LBN '{}' specified for '{}'", s, itemName));
		}
	}
	return lbn;
}


// Parse the "system_area" section of the catalog file.
static void parseSystemArea(ifstream & catalogFile, Catalog & cat)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error("Syntax error in catalog file: unterminated system_area section");
		}

		static const regex fileSpec("file\\s*\"(.+)\"");
		smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (regex_match(line, m, fileSpec)) {

			// File specification
			cat.systemAreaFile = m[1];

		} else {
			throw runtime_error(format("Syntax error in catalog file: \"{}\" unrecognized in system_area section", line));
		}
	}
}


// Parse the "volume" section of the catalog file.
static void parseVolume(ifstream & catalogFile, Catalog & cat)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error("Syntax error in catalog file: unterminated volume section");
		}

		static const regex systemIdSpec("system_id\\s*\\[(.*)\\]");
		static const regex volumeIdSpec("volume_id\\s*\\[(.*)\\]");
		static const regex volumeSetIdSpec("volume_set_id\\s*\\[(.*)\\]");
		static const regex publisherIdSpec("publisher_id\\s*\\[(.*)\\]");
		static const regex preparerIdSpec("preparer_id\\s*\\[(.*)\\]");
		static const regex applicationIdSpec("application_id\\s*\\[(.*)\\]");
		static const regex copyrightFileIdSpec("copyright_file_id\\s*\\[(.*)\\]");
		static const regex abstractFileIdSpec("abstract_file_id\\s*\\[(.*)\\]");
		static const regex bibliographicFileIdSpec("bibliographic_file_id\\s*\\[(.*)\\]");
		static const regex creationDateSpec("creation_date\\s*(.*)");
		static const regex modificationDateSpec("modification_date\\s*(.*)");
		static const regex expirationDateSpec("expiration_date\\s*(.*)");
		static const regex effectiveDateSpec("effective_date\\s*(.*)");
		static const regex track_listingSpec("track_listing\\s*\\[(.*)\\]");
		static const regex track1SectorCountSpec("track1_sector_count\\s*(\\d+)");
		static const regex track1PostgapTypeSpec("track1_postgap_type\\s*(\\d+)");
		static const regex audioSectorsSpec("audio_sectors\\s*(\\d+)");
		static const regex strictRebuildSpec("strict_rebuild\\s*(\\d+)");
		static const regex defaultUIDSpec("default_uid\\s*(\\d+)");
		static const regex defaultGIDSpec("default_gid\\s*(\\d+)");
		smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (regex_match(line, m, systemIdSpec)) {

			// System ID specification
			checkAString(m[1], "system_id");
			cat.systemID = m[1];

		} else if (regex_match(line, m, volumeIdSpec)) {

			// Volume ID specification
			checkDString(m[1], "volume_id");
			cat.volumeID = m[1];

		} else if (regex_match(line, m, volumeSetIdSpec)) {

			// Volume set ID specification
			checkDString(m[1], "volume_set_id");
			cat.volumeSetID = m[1];

		} else if (regex_match(line, m, publisherIdSpec)) {

			// Publisher ID specification
			checkAString(m[1], "publisher_id");
			cat.publisherID = m[1];

		} else if (regex_match(line, m, preparerIdSpec)) {

			// Preparer ID specification
			checkAString(m[1], "preparer_id");
			cat.preparerID = m[1];

		} else if (regex_match(line, m, applicationIdSpec)) {

			// Application ID specification
			checkAString(m[1], "application_id");
			cat.applicationID = m[1];

		} else if (regex_match(line, m, copyrightFileIdSpec)) {

			// Copyright file ID specification
			checkDString(m[1], "copyright_file_id");
			cat.copyrightFileID = m[1];

		} else if (regex_match(line, m, abstractFileIdSpec)) {

			// Abstract file ID specification
			checkDString(m[1], "abstract_file_id");
			cat.abstractFileID = m[1];

		} else if (regex_match(line, m, bibliographicFileIdSpec)) {

			// Bibliographic file ID specification
			checkDString(m[1], "bibliographic_file_id");
			cat.bibliographicFileID = m[1];

		} else if (regex_match(line, m, creationDateSpec)) {

			// Creation date specification
			parse_ltime(m[1], cat.creationDate);
			
			// Get the timezone offset. Offset is in 15 minutes increments, so 36 means 9 hours.
			timeZone = std::stoi(std::to_string(cat.creationDate.lt_gmtoff));

		} else if (regex_match(line, m, modificationDateSpec)) {

			// Modification date specification
			parse_ltime(m[1], cat.modificationDate);

		} else if (regex_match(line, m, expirationDateSpec)) {

			// Expiration date specification
			parse_ltime(m[1], cat.expirationDate);

		} else if (regex_match(line, m, effectiveDateSpec)) {

			// Effective date specification
			parse_ltime(m[1], cat.effectiveDate);

		} else if (regex_match(line, m, track_listingSpec)) {

			// tracklisting
			track_listing = base64_decode(m[1].str());

		} else if (regex_match(line, m, track1SectorCountSpec)) {
			if (! str_to_num(m[1], track1SectorCount)) {
				throw runtime_error(format("'{}' is not a valid integer", m[1].str()));
			}

		} else if (regex_match(line, m, track1PostgapTypeSpec)) {
			if (! str_to_num(m[1], track1PostgapType)) {
				throw runtime_error(format("'{}' is not a valid integer", m[1].str()));
			}

		} else if (regex_match(line, m, audioSectorsSpec)) {
			if (! str_to_num(m[1], audioSectors)) {
				throw runtime_error(format("'{}' is not a valid integer", m[1].str()));
			}

		} else if (regex_match(line, m, strictRebuildSpec)) {
			if (! str_to_num(m[1], strictRebuild)) {
				throw runtime_error(format("'{}' is not a valid integer", m[1].str()));
			}

		} else if (regex_match(line, m, defaultUIDSpec)) {

			// Default user ID specification
			if (! str_to_num(m[1], cat.defaultUID)) {
				throw runtime_error(format("'{}' is not a valid user ID", m[1].str()));
			}

		} else if (regex_match(line, m, defaultGIDSpec)) {

			// Default group ID specification
			if (! str_to_num(m[1], cat.defaultGID)) {
				throw runtime_error(format("'{}' is not a valid group ID", m[1].str()));
			}

		} else {
			throw runtime_error(format("Syntax error in catalog file: \"{}\" unrecognized in volume section", line));
		}
	}
}


// Recursively parse a "dir" section of the catalog file.
static DirNode * parseDir(ifstream & catalogFile, Catalog & cat, const string & dirName, const fs::path & path, DirNode * parent = NULL, uint32_t startSector = 0, uint16_t nodeGID = 0, uint16_t nodeUID = 0, uint16_t nodeATR = 0, uint16_t nodeATRP = 0, string nodeDate = "", string nodeDateParent = "", int16_t nodeTimezone = 0, int16_t nodeTimezoneParent = 0, bool nodeHidden = false, int nodeY2kbug = 0)
{
	DirNode * dir = new DirNode(dirName, path, parent, startSector, nodeGID, nodeUID, nodeATR, nodeATRP, nodeDate, nodeDateParent, nodeTimezone, nodeTimezoneParent, nodeHidden, nodeY2kbug);

	while (true) {
		// Reset everything on each itteration.
		nodeGID = 0;
		nodeUID = 0;
		nodeATR = 0;
		nodeATRP = 0;
		nodeDate = "";
		nodeDateParent = "";
		nodeTimezone = 0;
		nodeTimezoneParent = 0;
		uint32_t nodeSize = 0;
		nodeHidden = false;
		nodeY2kbug = 0;
		bool nodeEDC = false;

		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error(format("Syntax error in catalog file: unterminated directory section \"{}\"", dirName));
		}

		static const regex fileSpec        ("file\\s*(\\S+)(?:\\s*@(\\d+))?(?:\\s*GID(\\d+))?(?:\\s*UID(\\d+))?(?:\\s*ATR(\\d+))?(?:\\s*DATE(\\d+))?(?:\\s*TIMEZONE(\\d+))?(?:\\s*SIZE(\\d+))?(?:\\s*HIDDEN(\\d+))?(?:\\s*Y2KBUG(\\d+))?");
		static const regex xaFileSpec    ("xafile\\s*(\\S+)(?:\\s*@(\\d+))?(?:\\s*GID(\\d+))?(?:\\s*UID(\\d+))?(?:\\s*ATR(\\d+))?(?:\\s*DATE(\\d+))?(?:\\s*TIMEZONE(\\d+))?(?:\\s*SIZE(\\d+))?(?:\\s*HIDDEN(\\d+))?(?:\\s*Y2KBUG(\\d+))?(?:\\s*ZEROEDC(\\d+))?");
		static const regex cddaFileSpec("cddafile\\s*(\\S+)(?:\\s*@(\\d+))?(?:\\s*GID(\\d+))?(?:\\s*UID(\\d+))?(?:\\s*ATR(\\d+))?(?:\\s*DATE(\\d+))?(?:\\s*TIMEZONE(\\d+))?(?:\\s*SIZE(\\d+))?(?:\\s*HIDDEN(\\d+))?(?:\\s*Y2KBUG(\\d+))?");
		static const regex dirStart         ("dir\\s*(\\S+)(?:\\s*@(\\d+))?(?:\\s*GID(\\d+))?(?:\\s*UID(\\d+))?(?:\\s*ATRS(\\d+))?(?:\\s*ATRP(\\d+))?(?:\\s*DATES(\\d*))?(?:\\s*DATEP(\\d*))?(?:\\s*TIMEZONES(\\d+))?(?:\\s*TIMEZONEP(\\d+))?(?:\\s*HIDDEN(\\d+))?(?:\\s*Y2KBUG(\\d+))?\\s*\\{");
		smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (regex_match(line, m, fileSpec)) {

			// File specification
			string fileName = m[1];
			if (!std::string(m[3]).empty() && !std::string(m[4]).empty() && !std::string(m[5]).empty() && !std::string(m[6]).empty()) {
				nodeGID = std::stoi(m[3]);
				nodeUID = std::stoi(m[4]);
				nodeATR = std::stoi(m[5]);
				nodeDate = m[6];
				nodeTimezone = std::stoi(m[7]);
				nodeSize = std::stoi(m[8]);
				nodeHidden = std::stoi(m[9]);
				nodeY2kbug = std::stoi(m[10]);
			}
			checkFileName(fileName, "file name");

			uint32_t startSector = checkLBN(m[2], fileName);

			FileNode * file = new FileNode(fileName + ";1", path / fileName, dir, startSector, false, false, nodeGID, nodeUID, nodeATR, nodeDate, nodeTimezone, nodeSize, nodeSize, nodeHidden, nodeY2kbug, false);
			dir->children.push_back(file);

		} else if (regex_match(line, m, xaFileSpec)) {

			// XA file specification
			string fileName = m[1];
			if (!std::string(m[3]).empty() && !std::string(m[4]).empty() && !std::string(m[5]).empty() && !std::string(m[6]).empty()) {
				nodeGID = std::stoi(m[3]);
				nodeUID = std::stoi(m[4]);
				nodeATR = std::stoi(m[5]);
				nodeDate = m[6];
				nodeTimezone = std::stoi(m[7]);
				nodeSize = std::stoi(m[8]);
				nodeHidden = std::stoi(m[9]);
				nodeY2kbug = std::stoi(m[10]);
				nodeEDC = std::stoi(m[11]);
			}
			checkFileName(fileName, "file name");

			uint32_t startSector = checkLBN(m[2], fileName);

			FileNode * file = new FileNode(fileName + ";1", path / fileName, dir, startSector, true, false, nodeGID, nodeUID, nodeATR, nodeDate, nodeTimezone, nodeSize, nodeSize, nodeHidden, nodeY2kbug, nodeEDC);
			dir->children.push_back(file);

		} else if (regex_match(line, m, cddaFileSpec)) {

			// CDDA file specification
			string fileName = m[1];
			if (!std::string(m[3]).empty() && !std::string(m[4]).empty() && !std::string(m[5]).empty() && !std::string(m[6]).empty()) {
				nodeGID = std::stoi(m[3]);
				nodeUID = std::stoi(m[4]);
				nodeATR = std::stoi(m[5]);
				nodeDate = m[6];
				nodeTimezone = std::stoi(m[7]);
				nodeSize = std::stoi(m[8]);
				nodeHidden = std::stoi(m[9]);
				nodeY2kbug = std::stoi(m[10]);
			}
			checkFileName(fileName, "file name");

			uint32_t startSector = checkLBN(m[2], fileName);

			FileNode * file = new FileNode(fileName + ";1", path / fileName, dir, startSector, false, true, nodeGID, nodeUID, nodeATR, nodeDate, nodeTimezone, nodeSize, nodeSize, nodeHidden, nodeY2kbug, false);
			dir->children.push_back(file);

		} else if (regex_match(line, m, dirStart)) {

			// Subdirectory section
			string subDirName = m[1];
			if (!std::string(m[3]).empty() && !std::string(m[4]).empty() && !std::string(m[5]).empty() && !std::string(m[6]).empty()) {
				nodeGID = std::stoi(m[3]);
				nodeUID = std::stoi(m[4]);
				nodeATR = std::stoi(m[5]);
				nodeATRP = std::stoi(m[6]);
				nodeDate = m[7];
				nodeDateParent = m[8];
				nodeTimezone = std::stoi(m[9]);
				nodeTimezoneParent = std::stoi(m[10]);
				nodeHidden = std::stoi(m[11]);
				nodeY2kbug = std::stoi(m[12]);
			}
			checkDString(subDirName, "directory name");

			uint32_t startSector = checkLBN(m[2], subDirName);

			DirNode * subDir = parseDir(catalogFile, cat, subDirName, path / subDirName, dir, startSector, nodeGID, nodeUID, nodeATR, nodeATRP, nodeDate, nodeDateParent, nodeTimezone, nodeTimezoneParent, nodeHidden, nodeY2kbug);
			dir->children.push_back(subDir);

		} else {
			throw runtime_error(format("Syntax error in catalog file: \"{}\" unrecognized in directory section", line));
		}
	}

	// Create the sorted list of children
	dir->sortedChildren = dir->children;
	sort(dir->sortedChildren.begin(), dir->sortedChildren.end(), CmpByName());

	return dir;
}


// Parse the catalog file and fill in the Catalog structure.
static void parseCatalog(ifstream & catalogFile, Catalog & cat, const fs::path & fsBase)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {

			// End of file
			return;
		}

		static const regex systemAreaStart("system_area\\s*\\{");
		static const regex volumeStart("volume\\s*\\{");
		static const regex rootDirStart("dir\\s*(?:\\s*@(\\d+))?(?:\\s*GID(\\d+))?(?:\\s*UID(\\d+))?(?:\\s*ATRS(\\d+))?(?:\\s*ATRP(\\d+))?(?:\\s*DATES(\\d*))?(?:\\s*DATEP(\\d*))?(?:\\s*TIMEZONES(\\d+))?(?:\\s*TIMEZONEP(\\d+))?(?:\\s*HIDDEN(\\d+))?(?:\\s*Y2KBUG(\\d+))?\\s*\\{");
		smatch m;

		if (regex_match(line, systemAreaStart)) {

			// Parse system_area section
			parseSystemArea(catalogFile, cat);

		} else if (regex_match(line, volumeStart)) {

			// Parse volume section
			parseVolume(catalogFile, cat);

		} else if (regex_match(line, m, rootDirStart)) {
				uint16_t nodeGID = std::stoi(m[2]);
				uint16_t nodeUID = std::stoi(m[3]);
				uint16_t nodeATR = std::stoi(m[4]);
				uint16_t nodeATRP = std::stoi(m[5]);
				string nodeDate = m[6];
				string nodeDateParent = m[7];
				int16_t nodeTimezone = std::stoi(m[8]);
				int16_t nodeTimezoneParent = std::stoi(m[9]);
				int nodeY2kbug = std::stoi(m[11]);
				if (nodeY2kbug == 1 || nodeY2kbug == 11) {
					y2kbug = 1;
				}
			// Parse root directory entry
			if (cat.root) {
				throw runtime_error("More than one root directory section in catalog file");
			} else {
				cat.root = parseDir(catalogFile, cat, "", fsBase, NULL, 0, nodeGID, nodeUID, nodeATR, nodeATRP, nodeDate, nodeDateParent, nodeTimezone, nodeTimezoneParent, 0, nodeY2kbug);
			}

		} else {
			throw runtime_error(format("Syntax error in catalog file: \"{}\" unrecognized", line));
		}
	}
}


// Visitor which prints the filesystem tree to cout
class PrintVisitor : public Visitor {
public:
	void visit(FileNode & file)
	{
		cout << file.path << " (" << file.numSectors << " sectors @ " << file.firstSector << ", " << file.size << " bytes)" << endl;
	}

	void visit(DirNode & dir)
	{
		cout << dir.path << " (" << dir.numSectors << " sectors @ " << dir.firstSector << ", PT record " << dir.recordNumber << ")" << endl;
	}
};


// Visitor which calculates the number of sectors required for each directory,
// setting the "numSectors" field of all directory nodes
class CalcDirSize : public Visitor {
public:
	void visit(DirNode & dir)
	{
		uint32_t size = 0;

		// "." and ".." records
		size += iso9660_dir_calc_record_size(1, sizeof(iso9660_xa_t));
		size += iso9660_dir_calc_record_size(1, sizeof(iso9660_xa_t));

		// Records for all direct children
		for (vector<FSNode *>::const_iterator i = dir.sortedChildren.begin(); i != dir.sortedChildren.end(); ++i) {
			uint32_t recordSize = iso9660_dir_calc_record_size((*i)->name.size(), sizeof(iso9660_xa_t));

			if (size / ISO_BLOCKSIZE != (size + recordSize) / ISO_BLOCKSIZE) {

				// Record would cross a sector boundary, add padding
				recordSize += (ISO_BLOCKSIZE - size) % ISO_BLOCKSIZE;
			}

			size += recordSize;
		}

		// Round up to full sectors
		dir.numSectors = (size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
	}
};


// Visitor which allocates sectors to all file and directory extents, setting
// the "firstSector" field of all nodes
class AllocSectors : public Visitor {
public:
	AllocSectors(uint32_t startSector_) : currentSector(startSector_) { }

	uint32_t getCurrentSector() const { return currentSector; }
	
	std::vector<FileNode*> overflowFiles;

	void visitNode(FSNode & node)
	{
		bool isAudio = false;
		// Check if the node is a FileNode
		if (FileNode* fileNode = dynamic_cast<FileNode*>(&node)) {
			isAudio = fileNode->isAudio; // Access isAudio
		}

		// Minimum start sector requested?
		if (node.requestedStartSector && isAudio == false) { // Ignore for DA but keep the value instead of setting it to 0. Its needed for the MakeDirectories function.

			// Yes, before current sector?
			if (node.requestedStartSector < currentSector) {

				// Yes, ignore the request and print a warning
				node.firstSector = currentSector;
				cerr << "Warning: " << node.path << " will start at sector " << node.firstSector << " instead of " << node.requestedStartSector << endl;

			} else {

				// Heed the request
				node.firstSector = node.requestedStartSector;
			}

		} else {

			// Allocate contiguously
			node.firstSector = currentSector;
		}

		currentSector = node.firstSector + node.numSectors;
	}

	void visit(DirNode & dir) { visitNode(dir); }
	void visit(FileNode & file) { visitNode(file); }

	void allocateOverflowFiles()
	{
		for (auto* file : overflowFiles) {
			size_t blockSize = file->isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
			uint32_t overflowSectors = (file->size + blockSize - 1) / blockSize;

			file->requestedStartSector = currentSector;  // Update start sector for overflow file
			file->firstSector = currentSector;           // Assign firstSector
			file->numSectors = overflowSectors;          // Update the number of sectors

			currentSector += overflowSectors;
			std::cerr << "Re-allocating overflow file: \"" << file->path.string()
			          << "\" to sector " << file->firstSector << std::endl;
		}
	}

	void allocate(std::vector<FSNode*>& flatList) {
		for (auto* node : flatList) {
			// Handle FileNode
			if (FileNode* fileNode = dynamic_cast<FileNode*>(node)) {
				if (fileNode->isAudio == true) { continue; }
				// Calculate allocated size and actual size
				size_t blockSize = fileNode->isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
				uint32_t allocatedSectorsFile = (fileNode->size + blockSize - 1) / blockSize;
				uint32_t allocatedSectorsTOC  = (fileNode->nodeSizeOriginal + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;

				if (allocatedSectorsFile > allocatedSectorsTOC) {
					std::cerr << "Overflow detected: \"" << fileNode->path.string()
					          << "\" (sector count: " << allocatedSectorsFile
					          << ", max allowed: " << allocatedSectorsTOC << ")" << std::endl;
					overflowFiles.push_back(fileNode);  // Add to overflow list
					continue;
				}
			}
			if (node->requestedStartSector) {
				node->firstSector = node->requestedStartSector;
			} else {
				node->firstSector = currentSector;
			}
				currentSector = node->firstSector + node->numSectors;
		}
	}


private:
	uint32_t currentSector;
};


// Visitor which creates the directory data, setting the "data" field of
// directory nodes
class MakeDirectories : public Visitor {
public:
	MakeDirectories(const Catalog & cat_) : cat(cat_) { }

	void visit(DirNode & dir)
	{
		uint32_t dirSize = dir.numSectors * ISO_BLOCKSIZE;

		// Create the directory extent
		iso9660_xa_t xaAttr;
		iso9660_xa_t xaAttrP;
		iso9660_xa_init(&xaAttr, 0, 0, dir.nodeATR, 0);
		iso9660_xa_init(&xaAttrP, 0, 0, dir.nodeATRP, 0);

		uint32_t parentSector = dir.parent ? dir.parent->firstSector : dir.firstSector;
		uint32_t parentSize = (dir.parent ? dir.parent->numSectors : dir.numSectors) * ISO_BLOCKSIZE;

		time_t dirTime;
		time_t dirTimeParent;
		convertToEpochTime(dir.nodeDate, dirTime);
		convertToEpochTime(dir.nodeDateParent, dirTimeParent);

		uint8_t * data = new uint8_t[dirSize];
		iso9660_dir_init_new_su(data,
		                        dir.firstSector, dirSize, &xaAttr, sizeof(xaAttr),
		                        parentSector, parentSize, &xaAttrP, sizeof(xaAttrP),
		                        &dirTime, &dirTimeParent, (dir.nodeTimezone * 15), (dir.nodeTimezoneParent * 15), dir.nodeY2kbug);

		// Add the records for all children
		for (vector<FSNode *>::const_iterator i = dir.sortedChildren.begin(); i != dir.sortedChildren.end(); ++i) {
			FSNode * node = *i;
			uint32_t size = node->numSectors * ISO_BLOCKSIZE;
			uint8_t flags;
			string nodeDate = "";
			int16_t nodeTimezone = 0;
			int nodeY2kbug = 0;

			if (FileNode * file = dynamic_cast<FileNode *>(node)) {
				flags = (file->nodeHidden) ? ISO_FILE | ISO_EXISTENCE : ISO_FILE;
				nodeDate = file->nodeDate;
				nodeTimezone = file->nodeTimezone;
				nodeY2kbug = file->nodeY2kbug;
				if (file->isForm2) {
					iso9660_xa_init(&xaAttr, file->nodeUID, file->nodeGID, file->nodeATR, 1);
				} else if (file->isAudio) {
					iso9660_xa_init(&xaAttr, file->nodeUID, file->nodeGID, file->nodeATR, 0);
					size = file->nodeSize;
					node->firstSector = node->requestedStartSector + track1SectorCountOffset; // Add the offset between the original and the new rebuild sector count to fix the CDDA entries.
				} else {
					iso9660_xa_init(&xaAttr, file->nodeUID, file->nodeGID, file->nodeATR, 0);
					size = file->size;
				}
			} else if (DirNode * dir = dynamic_cast<DirNode *>(node)) {
				iso9660_xa_init(&xaAttr, dir->nodeUID, dir->nodeGID, dir->nodeATR, 0);
				nodeDate = dir->nodeDate;
				nodeTimezone = dir->nodeTimezone;
				nodeY2kbug = dir->nodeY2kbug;
				flags = (dir->nodeHidden) ? ISO_DIRECTORY | ISO_EXISTENCE : ISO_DIRECTORY;
			} else {
				throw runtime_error("Internal filesystem tree corrupt");
			}

			time_t nodeTime;
 			convertToEpochTime(nodeDate, nodeTime);

 			iso9660_dir_add_entry_su(data, node->name.c_str(), node->firstSector, size, flags, &xaAttr, sizeof(xaAttr), &nodeTime, (nodeTimezone * 15), nodeY2kbug);
 			
		}

		dir.data = data;
	}

private:

	// Reference to Catalog information
	const Catalog & cat;
};


// Visitor which constructs the path tables from a filesystem tree,
// assigning the "recordNumber" field of all directory nodes
class PathTables : public Visitor {
public:
	PathTables()
	{
		iso9660_pathtable_init(lTable);
		iso9660_pathtable_init(mTable);
	}

	void visit(DirNode & dir)
	{
		uint16_t parentRecord = dir.parent ? dir.parent->recordNumber : 1;
		dir.recordNumber = iso9660_pathtable_l_add_entry(lTable, dir.name.c_str(), dir.firstSector, parentRecord);
		dir.recordNumber = iso9660_pathtable_m_add_entry(mTable, dir.name.c_str(), dir.firstSector, parentRecord);
	}

	size_t size() const { return iso9660_pathtable_get_size( lTable ); }
	const void * getLTable() const { return lTable; }
	const void * getMTable() const { return mTable; }

private:
	uint8_t lTable[ISO_BLOCKSIZE];  // LSB-first table
	uint8_t mTable[ISO_BLOCKSIZE];  // MSB-first table
};


// Visitor which writes all directory and file data to the image file
class WriteData : public Visitor {
public:
	WriteData(ofstream & image_, uint32_t startSector_) : image(image_), currentSector(startSector_) { }

	void visit(FileNode & file)
	{
		if (file.isAudio) { return; } // Do not write DA files back as audio tracks. Process seperately.

		ifstream f(file.path, ifstream::in | ifstream::binary);
		if (!f) {
			throw runtime_error(format("Cannot open file {}", file.path.string()));
		}

		cdio_info("Writing \"%ls\"...", file.path.c_str());

		writeGap(file.firstSector);

		char data[M2RAW_SECTOR_SIZE];
		size_t blockSize = file.isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;

		for (uint32_t sector = 0; sector < file.numSectors; ++sector) {
			uint8_t subMode = SM_DATA;
			if (sector == file.numSectors - 1) {
				subMode |= (SM_EOF | SM_EOR);  // last sector
			}

			memset(data, 0, blockSize);
			f.read(data, blockSize);

			if (file.isForm2) {
				_vcd_make_mode2(buffer, data + CDIO_CD_SUBHEADER_SIZE, currentSector, data[0], data[1], data[2], data[3]);
				if (file.nodeEDC == true) { // If the Mode 2 Form 2 files need to be stripped of their EDC checksum (Like Audio/Video/.STR/.XXA)
					if ((buffer[18] & 0x20) == 0x20) {
						buffer[2348] = '\0';
						buffer[2349] = '\0';
						buffer[2350] = '\0';
						buffer[2351] = '\0';
					}
				}
			} else {
				_vcd_make_mode2(buffer, data, currentSector, 0, 0, subMode, 0);
			}

			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

	void visit(DirNode & dir)
	{
		writeGap(dir.firstSector);

		for (uint32_t sector = 0; sector < dir.numSectors; ++sector) {
			uint8_t subMode = SM_DATA;
			if (sector == dir.numSectors - 1) {
				subMode |= (SM_EOF | SM_EOR);  // last sector
			}

			_vcd_make_mode2(buffer, dir.data + sector * ISO_BLOCKSIZE, currentSector, 0, 0, subMode, 0);
			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

	// Write empty sectors as a gap until we reach the specified sector.
	void writeGap(uint32_t until)
	{
		while (currentSector < until) {
			_vcd_make_mode2(buffer, emptySector, currentSector, 0, 0, SM_FORM2, 0);
			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

	// New method for writing from a flat list
	void writeFromFlatList(const std::vector<FSNode*>& flatList)
	{
		for (auto* node : flatList) {
			node->accept(*this);  // Call the appropriate visit method
		}
	}

private:
	ofstream & image;
	uint32_t currentSector;
};


// Write the system area to the image file, optionally using the file
// specified in the catalog as input.
static void writeSystemArea(ofstream & image, const Catalog & cat)
{
	const size_t numSystemSectors = 16;
	const size_t systemAreaSize = numSystemSectors * CDIO_CD_FRAMESIZE_RAW;

	unique_ptr<char[]> data(new char[systemAreaSize]);
	memset(data.get(), 0, systemAreaSize);

	size_t fileSize = 0;

	if (!cat.systemAreaFile.empty()) {

		// Copy the data (max. 32K) from the system area file
		ifstream f(cat.systemAreaFile.c_str(), ifstream::in | ifstream::binary);
		if (!f) {
			throw runtime_error(format("Cannot open system area file \"{}\"", cat.systemAreaFile));
		}

		fileSize = f.read(data.get(), systemAreaSize).gcount();
		if (f.bad()) {
			throw runtime_error(format("Error reading system area file \"{}\"", cat.systemAreaFile));
		}
	}

	size_t numFileSectors = (fileSize + CDIO_CD_FRAMESIZE_RAW - 1) / CDIO_CD_FRAMESIZE_RAW;

	// Write system area to image file
	for (size_t sector = 0; sector < numFileSectors; ++sector) {

		// Data sectors
		memcpy(buffer, data.get() + sector * CDIO_CD_FRAMESIZE_RAW, CDIO_CD_FRAMESIZE_RAW);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);
	}

	for (size_t sector = numFileSectors; sector < numSystemSectors; ++sector) {

		// Empty sectors
		memset(buffer, 0, CDIO_CD_FRAMESIZE_RAW);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);
	}
}


// Print usage information and exit.
static void usage(const char * progname, int exitcode = 0, const string & error = "")
{
	cout << "Usage: " << fs::path(progname).filename().string() << " [OPTION...] <input>[.cat] [<output>[.bin]]" << endl;
	cout << "  -c, --cuefile                   Create a .cue file" << endl;
	cout << "  -v, --verbose                   Be verbose" << endl;
	cout << "  -V, --version                   Display version information and exit" << endl;
	cout << "  -?, --help                      Show this help message" << endl;

	if (!error.empty()) {
		cerr << endl << "Error: " << error << endl;
	}

	exit(exitcode);
}


// Main program
int main(int argc, char ** argv)
{
	// Parse command line arguments
	fs::path inputPath;
	fs::path outputPath;
	bool verbose = false;
	bool writeCueFile = false;

	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];

		if (arg == "--version" || arg == "-V") {
			cout << TOOL_VERSION << endl;
			return 0;
		} else if (arg == "--cuefile" || arg == "-c") {
			writeCueFile = true;
		} else if (arg == "--verbose" || arg == "-v") {
			cdio_loglevel_default = CDIO_LOG_INFO;
			verbose = true;
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
		usage(argv[0], 64, "No input catalog file specified");
	}

	if (outputPath.empty()) {
		outputPath = inputPath;
		outputPath.replace_extension("");
	}

	try {

		// Read and parse the catalog file
		fs::path catalogName = inputPath;
		if (catalogName.extension().empty()) {
			catalogName.replace_extension(".cat");
		}

		Catalog cat;

		ifstream catalogFile(catalogName);
		if (!catalogFile) {
			throw runtime_error(format("Cannot open catalog file {}", catalogName.string()));
		}

		fs::path fsBasePath = inputPath;
		fsBasePath.replace_extension("");
		psxripDir = fsBasePath / "_PSXRIP";

		cout << "Reading catalog file " << catalogName << "...\n";
		cout << "Reading filesystem from directory " << fsBasePath << "...\n";

		parseCatalog(catalogFile, cat, fsBasePath);

		if (!cat.root) {
			throw runtime_error("No root directory specified in catalog file");
		}

		catalogFile.close();

		// Calculate the sector numbers of the fixed data structures
		const uint32_t pvdSector = ISO_PVD_SECTOR;
		const uint32_t evdSector = pvdSector + 1;
		const uint32_t pathTableStartSector = evdSector + 1;
		const uint32_t numPathTableSectors = 1;  // number of sectors in one path table, currently fixed to 1
		const uint32_t rootDirStartSector = pathTableStartSector + numPathTableSectors * 4;  // 2 types and 2 copies in path table group

		// Calculate the sizes of all directories
		CalcDirSize calcDir;
		cat.root->traverseSorted(calcDir);

		// Allocate start sectors to all nodes
		AllocSectors alloc(rootDirStartSector);
		std::vector<FSNode*> flatList;
		// Strict rebuild uses the exact file / directory order according to the start sector.
		if (strictRebuild == 1) {
			std::cerr << "\nStrict mode set! All files are written back to their original LSN.\n"
			          << "Files bigger then their allowed space are remapped to the end of track 1.\n" << std::endl;
			flattenTree(cat.root, flatList);
			std::sort(flatList.begin(), flatList.end(), [](FSNode* a, FSNode* b) {
				return a->requestedStartSector < b->requestedStartSector;
			});
			alloc.allocate(flatList);
			alloc.allocateOverflowFiles(); // Allocate overflow files
			std::sort(flatList.begin(), flatList.end(), [](FSNode* a, FSNode* b) {
				return a->requestedStartSector < b->requestedStartSector;
			});
		} else {
			cat.root->traverse(alloc);  // must use the same traversal order as "WriteData" below
		}

		uint32_t volumeSize = alloc.getCurrentSector();

		// Add postgap sectors of data track 1 to the volumeSize as they are not counted.
		volumeSize = volumeSize + 150;

		// Add offset calculation from track1SectorCount and actual volumeSize for CDDA audio entries.
		track1SectorCountOffset = volumeSize - track1SectorCount;

		// Add audio sectors to the volumeSize
		volumeSize = volumeSize + audioSectors;

		if (volumeSize > MAX_ISO_SECTORS) {
			cerr << "Warning: Output image larger than "
			     << (MAX_ISO_SECTORS * CDIO_CD_FRAMESIZE_RAW / (1024*1024)) << " MiB\n";
		}

		// Create the directory data
		MakeDirectories makeDirs(cat);
		cat.root->traverseSorted(makeDirs);

		// Create the path tables
		PathTables pathTables;
		cat.root->traverseBreadthFirstSorted(pathTables);

		if (pathTables.size() > ISO_BLOCKSIZE) {
			throw runtime_error("The path table is larger than one sector. This is currently not supported.");
		}

		if (verbose) {
			PrintVisitor pv;
			cat.root->traverse(pv);
		}

		// Create the image file
		fs::path imageName = outputPath;
		imageName.replace_extension(".bin");
		fs::path imageCueName = outputPath;
		imageCueName.replace_extension(".cue");

		ofstream image(imageName, ofstream::out | ofstream::binary | ofstream::trunc);
		if (!image) {
			throw runtime_error(format("Error creating image file {}", imageName.string()));
		}

		// Write the system area
		cdio_info("Writing system area...");
		writeSystemArea(image, cat);

		// Write the PVD
		cdio_info("Writing volume descriptors...");
		iso9660_pvd_t volumeDesc;

		struct tm rootTm;
		time_t rootTime;

		iso9660_ltime_t creationDate = cat.creationDate;

		int creationYear_int = std::stoi(std::string(creationDate.lt_year, 4));
		if (creationYear_int == 0 || creationYear_int == 100) {
			creationDate.lt_year[0] = '2';
			creationDate.lt_year[1] = '0';
			creationDate.lt_year[2] = '0';
			creationDate.lt_year[3] = '0';
		} else if (creationYear_int > 0 && creationYear_int < 30) {
			creationDate.lt_year[0] = '2';
			creationDate.lt_year[1] = '0';
		} else if (creationYear_int >= 70 && creationYear_int < 99) {
			creationDate.lt_year[0] = '1';
			creationDate.lt_year[1] = '9';
		} else if (creationYear_int >= 1900 && creationYear_int < 1970) {
			creationDate.lt_year[0] = '2';
			creationDate.lt_year[1] = '0';
		}

		iso9660_get_ltime(&creationDate, &rootTm);
		rootTime = timegm(&rootTm) - (timeZone * (15 * 60));
		gmtime_s(&rootTm, &rootTime);
		if (y2kbug == 1) {
			rootTm.tm_year -= 100;
		}

		iso9660_dir_t rootDirRecord;
		memset(&rootDirRecord, 0, sizeof(rootDirRecord));

		rootDirRecord.length = to_711(iso9660_dir_calc_record_size(0, 0));
		rootDirRecord.extent = to_733(rootDirStartSector);
		rootDirRecord.size = to_733(cat.root->numSectors * ISO_BLOCKSIZE);
		iso9660_set_dtime_with_timezone(&rootTm, (timeZone * 15), &rootDirRecord.recording_time, 0);
		rootDirRecord.file_flags = ISO_DIRECTORY;
		rootDirRecord.volume_sequence_number = to_723(1);
		rootDirRecord.filename.len = 1;

		iso9660_set_pvd(&volumeDesc,
		                cat.volumeID.c_str(), cat.publisherID.c_str(), cat.preparerID.c_str(), cat.applicationID.c_str(),
		                volumeSize, &rootDirRecord,
		                pathTableStartSector, pathTableStartSector + numPathTableSectors * 2,
		                pathTables.size(), &rootTime, y2kbug);

		iso9660_strncpy_pad(volumeDesc.system_id, cat.systemID.c_str(), ISO_MAX_SYSTEM_ID, ISO9660_ACHARS);
		iso9660_strncpy_pad(volumeDesc.volume_set_id, cat.volumeSetID.c_str(), ISO_MAX_VOLUMESET_ID, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.copyright_file_id, cat.copyrightFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.abstract_file_id, cat.abstractFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.bibliographic_file_id, cat.bibliographicFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);

		volumeDesc.creation_date = cat.creationDate;
		volumeDesc.modification_date = cat.modificationDate;
		volumeDesc.expiration_date = cat.expirationDate;
		volumeDesc.effective_date = cat.effectiveDate;

		volumeDesc.opt_type_l_path_table = to_731(pathTableStartSector + numPathTableSectors);
		volumeDesc.opt_type_m_path_table = to_732(pathTableStartSector + numPathTableSectors * 3);

		_vcd_make_mode2(buffer, &volumeDesc, pvdSector, 0, 0, SM_DATA | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the volume descriptor set terminator
		iso9660_set_evd(&volumeDesc);

		_vcd_make_mode2(buffer, &volumeDesc, evdSector, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the path tables
		cdio_info("Writing path tables...");
		_vcd_make_mode2(buffer, pathTables.getLTable(), pathTableStartSector + numPathTableSectors * 0, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getLTable(), pathTableStartSector + numPathTableSectors * 1, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getMTable(), pathTableStartSector + numPathTableSectors * 2, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getMTable(), pathTableStartSector + numPathTableSectors * 3, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the directory and file data
		if (strictRebuild == 1) {
			WriteData writer(image, rootDirStartSector);
			writer.writeFromFlatList(flatList);
		} else {
			WriteData writeData(image, rootDirStartSector);
			cat.root->traverse(writeData);  // must use the same traversal order as "AllocSectors" above
		}

		// Write postgap. Usually 150 blank sectors which is standard.
		fs::path lastSectorFilePath = psxripDir / "Last_sector.bin";
		for (int i = 0; i < 150; i++) {
			if (i == 149 && fs::exists(lastSectorFilePath)) {
				_vcd_make_mode2(buffer, emptySectorRAW, i + alloc.getCurrentSector(), 0, 0, CN_EMPTY, 0);
				std::ifstream lastSectorFile(lastSectorFilePath, std::ios::binary);
				if (lastSectorFile.is_open()) {
					char fileSector[CDIO_CD_FRAMESIZE_RAW] = {0};
					lastSectorFile.read(fileSector, CDIO_CD_FRAMESIZE_RAW);
					constexpr int DATA_OFFSET = 24;
					std::memcpy(buffer + DATA_OFFSET, fileSector + DATA_OFFSET, CDIO_CD_FRAMESIZE_RAW - DATA_OFFSET);
					lastSectorFile.close();
				}
			} else {
				if (track1PostgapType == 1) {
					_vcd_make_mode2(buffer, emptySectorRAW, i+alloc.getCurrentSector(), 0, 0, CN_EMPTY, 0); // Type 1 is empty
				} else if (track1PostgapType == 2){
					_vcd_make_mode2(buffer, emptySectorRAW, i+alloc.getCurrentSector(), 0, 0, SM_FORM2, 0); // Type 2 has Mode2 bytes set.
				} else if (track1PostgapType == 3){
					_vcd_make_mode2(buffer, emptySectorRAW, i+alloc.getCurrentSector(), 0, 0, SM_FORM2, 0); // Type 3 has Mode2 bytes set and EDC.
				} else {
					_vcd_make_mode2(buffer, emptySectorRAW, i+alloc.getCurrentSector(), 0, 0, CN_EMPTY, 0); // Unknown or Empty with garbage in last sector.
				}
			}
			if (buffer[18] == 0x20 && track1PostgapType != 3) { // Zero out the last 4 EDC bytes for type 2.
				buffer[2348] = '\0';
				buffer[2349] = '\0';
				buffer[2350] = '\0';
				buffer[2351] = '\0';
			}
			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);
		}

		// Parse the track information from the catalog file.
		std::vector<TrackInfo> tracks = parseTracksFromString(track_listing);

		// Append the stored .wav files.
		writeAudioTracks(tracks, inputPath, image);

		// Write the .cue file
		generateCueFile(tracks, imageName, imageCueName);

		// Close the image file
		if (!image) {
			throw runtime_error(format("Error writing to image file {}", imageName.string()));
		}
		image.close();

		cout << "Image file written to " << imageName << "..." << endl;

		cdio_info("Done.");

	} catch (const std::exception & e) {
		cerr << e.what() << endl;
		return 1;
	}

	return 0;
}
