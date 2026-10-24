#include <stdio.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

#include "KeyFinder.h"
#include "AddressUtil.h"
#include "util.h"
#include "secp256k1.h"
#include "CmdParse.h"
#include "Logger.h"
#include "ConfigFile.h"
#include "PuzzleCatalog.h"

#include "DeviceManager.h"
#include "CpuKeySearchDevice.h"
#include "SearchArithmetic.h"
#include "RunControl.h"

#ifdef BUILD_CUDA
#include "CudaKeySearchDevice.h"
#endif

#ifdef BUILD_OPENCL
#include "CLKeySearchDevice.h"
#endif

struct RunConfig {
    bool cpu = false;
    unsigned cpuThreads = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1;
    unsigned usage = 100;
    std::string controlName;
    std::string partition;
    unsigned partitionIndex = 0, partitionCount = 1;
    // startKey is the first key. We store it so that if the --continue
    // option is used, the correct progress is displayed. startKey and
    // nextKey are only equal at the very beginning. nextKey gets saved
    // in the checkpoint file.
    secp256k1::uint256 startKey = 1;
    secp256k1::uint256 nextKey = 1;

    // The last key to be checked
    secp256k1::uint256 endKey = secp256k1::N - 1;

    uint64_t statusInterval = 1800;
    uint64_t checkpointInterval = 60000;

    unsigned int threads = 0;
    unsigned int blocks = 0;
    unsigned int pointsPerThread = 0;

    int compression = PointCompressionType::COMPRESSED;

    std::vector<std::string> targets;

    std::string checkpointFile = "";

    int device = 0;
    int puzzleBits = 0;

    std::string resultsFile = "";

    uint64_t totalkeys = 0;
    uint64_t elapsed = 0;
    secp256k1::uint256 stride = 1;

    bool follow = false;
};

static RunConfig _config;
#ifdef _WIN32
static RunControl _control;
#endif

std::vector<DeviceManager::DeviceInfo> _devices;

void writeCheckpoint(secp256k1::uint256 nextKey);

static const PuzzleDefinition SOLVED_SELF_TEST_PUZZLE = {
    66,
    "20000000000000000",
    "3ffffffffffffffff",
    "13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so",
    "6.6",
    "",
    "024EE2BE2D4E9F92D2F5A4A03058617DC45BEFE22938FEED5B7A6B7282DD74CBDD"
};

static const char *SOLVED_SELF_TEST_KEYSPACE = "2832ed74f2b5e3000:2832ed74f2b5e4000";

static uint64_t _lastUpdate = 0;
static uint64_t _runningTime = 0;
static uint64_t _startTime = 0;

/**
* Callback to display the private key
*/
void resultCallback(KeySearchResult info)
{
#ifdef _WIN32
    if(_control.data) InterlockedExchange(&_control.data->state, 5);
#endif
	if(_config.resultsFile.length() != 0) {
		Logger::log(LogLevel::Info, "Found key for address '" + info.address + "'. Written to '" + _config.resultsFile + "'");

		std::string s = info.address + " " + info.privateKey.toString(16) + " " + Address::privateKeyToWif(info.privateKey, info.compressed) + " " + info.publicKey.toString(info.compressed);
		util::appendToFile(_config.resultsFile, s);

		return;
	}

	std::string logStr = "Address:     " + info.address + "\n";
	logStr += "Private key: " + info.privateKey.toString(16) + "\n";
	logStr += "WIF:         " + Address::privateKeyToWif(info.privateKey, info.compressed) + "\n";
	logStr += "Compressed:  ";

	if(info.compressed) {
		logStr += "yes\n";
	} else {
		logStr += "no\n";
	}

	logStr += "Public key:  \n";

	if(info.compressed) {
		logStr += info.publicKey.toString(true) + "\n";
	} else {
		logStr += info.publicKey.x.toString(16) + "\n";
		logStr += info.publicKey.y.toString(16) + "\n";
	}

	Logger::log(LogLevel::Info, logStr);
}

/**
Callback to display progress
*/
void statusCallback(KeySearchStatus info)
{
#ifdef _WIN32
    if(_control.data) {
        InterlockedExchange64(&_control.data->keysPerSecond, (LONG64)(info.speed * 1000000.0));
        InterlockedExchange64(&_control.data->total, (LONG64)info.total);
    }
#endif
	std::string speedStr;

	if(info.speed < 0.01) {
		speedStr = "< 0.01 MKey/s";
	} else {
		speedStr = util::format("%.2f", info.speed) + " MKey/s";
	}

	std::string totalStr = "(" + util::formatThousands(_config.totalkeys + info.total) + " total)";

	std::string timeStr = "[" + util::formatSeconds((unsigned int)((_config.elapsed + info.totalTime) / 1000)) + "]";

	std::string usedMemStr = util::format((info.deviceMemory - info.freeMemory) /(1024 * 1024));

	std::string totalMemStr = util::format(info.deviceMemory / (1024 * 1024));

    std::string targetStr = util::format(info.targets) + " target" + (info.targets > 1 ? "s" : "");


	// Fit device name in 16 characters, pad with spaces if less
	std::string devName = info.deviceName.substr(0, 16);
	devName += std::string(16 - devName.length(), ' ');

    const char *formatStr = NULL;

    if(_config.follow) {
        formatStr = "%s %s/%sMB | %s %s %s %s\n";
    } else {
        formatStr = "\r%s %s / %sMB | %s %s %s %s";
    }

	printf(formatStr, devName.c_str(), usedMemStr.c_str(), totalMemStr.c_str(), targetStr.c_str(), speedStr.c_str(), totalStr.c_str(), timeStr.c_str());
    // stdout is fully buffered when redirected to the GUI's pipe. Publish
    // each status sample immediately, rather than flushing every GPU step.
    fflush(stdout);

    if(_config.checkpointFile.length() > 0) {
        uint64_t t = util::getSystemTime();
        if(t - _lastUpdate >= _config.checkpointInterval) {
            Logger::log(LogLevel::Info, "Checkpoint");
            writeCheckpoint(info.nextKey);
            _lastUpdate = t;
        }
    }
}

/**
 * Parses the start:end key pair. Possible values are:
 start
 start:end
 start:+offset
 :end
 :+offset
 */
bool parseKeyspace(const std::string &s, secp256k1::uint256 &start, secp256k1::uint256 &end)
{
    size_t pos = s.find(':');

    if(pos == std::string::npos) {
        start = secp256k1::uint256(s);
        end = secp256k1::N - 1;
    } else {
        std::string left = s.substr(0, pos);

        if(left.length() == 0) {
            start = secp256k1::uint256(1);
        } else {
            start = secp256k1::uint256(left);
        }

        std::string right = s.substr(pos + 1);

        if(right[0] == '+') {
            end = start + secp256k1::uint256(right.substr(1));
        } else {
            end = secp256k1::uint256(right);
        }
    }

    return true;
}

void usage()
{
    printf("--cpu                   Search using the CPU (no OpenCL device required)\n");
    printf("--cpu-threads N         CPU worker count (default: logical processors)\n");
    printf("--usage PERCENT         Active-time target, 0..100 (0 pauses)\n");
    printf("--partition-index N    Zero-based device slice within the partition\n");
    printf("--partition-count N    Number of slices (default 1)\n");
    printf("--partition full|gpu|cpu Full range or hybrid GPU/CPU partition\n");
    printf("RedCrack worker (derived from BitCrack)\n");
    printf("This OpenCL/CPU build only searches hardcoded public Bitcoin puzzle targets.\n\n");

    printf("--help                  Display this message\n");
    printf("--puzzle N              Select hardcoded Bitcoin puzzle target N\n");
    printf("--list-puzzles          List hardcoded puzzle targets\n");
    printf("--self-test-solved      Run a hardcoded solved puzzle fixture\n");
    printf("-c, --compressed        Use compressed points\n");
    printf("-u, --uncompressed      Use Uncompressed points\n");
    printf("--compression  MODE     Specify compression where MODE is\n");
    printf("                          COMPRESSED or UNCOMPRESSED or BOTH\n");
    printf("-d, --device ID         Use device ID\n");
    printf("-b, --blocks N          N blocks\n");
    printf("-t, --threads N         N threads per block\n");
    printf("-p, --points N          N points per thread\n");
    printf("-o, --out FILE          Write keys to FILE\n");
    printf("-f, --follow            Follow text output\n");
    printf("--list-devices          List available devices\n");
    printf("--keyspace KEYSPACE     Optional subrange inside the selected puzzle:\n");
    printf("                          START:END\n");
    printf("                          START:+COUNT\n");
    printf("                          :END\n");
    printf("                          :+COUNT\n");
    printf("                        Where START, END, COUNT are in hex format\n");
    printf("--stride N              Increment by N keys at a time\n");
    printf("--share M/N             Divide the keyspace into N equal shares, process the Mth share\n");
    printf("--continue FILE         Save/load progress from FILE\n");
}


/**
 Finds default parameters depending on the device
 */
typedef struct {
	int threads;
	int blocks;
	int pointsPerThread;
}DeviceParameters;

DeviceParameters getDefaultParameters(const DeviceManager::DeviceInfo &device)
{
	DeviceParameters p;
	p.threads = 256;
    p.blocks = 32;
	p.pointsPerThread = 32;

	return p;
}

static KeySearchDevice *getDeviceContext(DeviceManager::DeviceInfo &device, int blocks, int threads, int pointsPerThread)
{
#ifdef BUILD_CUDA
    if(device.type == DeviceManager::DeviceType::CUDA) {
        return new CudaKeySearchDevice((int)device.physicalId, threads, pointsPerThread, blocks);
    }
#endif

#ifdef BUILD_OPENCL
    if(device.type == DeviceManager::DeviceType::OpenCL) {
        return new CLKeySearchDevice(device.physicalId, threads, pointsPerThread, blocks);
    }
#endif

    return NULL;
}

static void printDeviceList(const std::vector<DeviceManager::DeviceInfo> &devices)
{
    for(int i = 0; i < devices.size(); i++) {
        printf("ID:     %d\n", devices[i].id);
        printf("Name:   %s\n", devices[i].name.c_str());
        printf("Memory: %lldMB\n", devices[i].memory / ((uint64_t)1024 * 1024));
        printf("Compute units: %d\n", devices[i].computeUnits);
        printf("\n");
    }
}

bool readAddressesFromFile(const std::string &fileName, std::vector<std::string> &lines)
{
    if(fileName == "-") {
        return util::readLinesFromStream(std::cin, lines);
    } else {
        return util::readLinesFromStream(fileName, lines);
    }
}

int parseCompressionString(const std::string &s)
{
    std::string comp = util::toLower(s);

    if(comp == "both") {
        return PointCompressionType::BOTH;
    }

    if(comp == "compressed") {
        return PointCompressionType::COMPRESSED;
    }

    if(comp == "uncompressed") {
        return PointCompressionType::UNCOMPRESSED;
    }

    throw std::string("Invalid compression format: '" + s + "'");
}

static std::string getCompressionString(int mode)
{
    switch(mode) {
    case PointCompressionType::BOTH:
        return "both";
    case PointCompressionType::UNCOMPRESSED:
        return "uncompressed";
    case PointCompressionType::COMPRESSED:
        return "compressed";
    }

    throw std::string("Invalid compression setting '" + util::format(mode) + "'");
}

void writeCheckpoint(secp256k1::uint256 nextKey)
{
    const std::string temporary = _config.checkpointFile + ".tmp";
    std::ofstream tmp(temporary, std::ios::out | std::ios::trunc);

    tmp << "start=" << _config.startKey.toString() << std::endl;
    tmp << "next=" << nextKey.toString() << std::endl;
    tmp << "end=" << _config.endKey.toString() << std::endl;
    tmp << "puzzle=" << _config.puzzleBits << std::endl;
    tmp << "partition=" << _config.partition << std::endl;
    tmp << "blocks=" << _config.blocks << std::endl;
    tmp << "threads=" << _config.threads << std::endl;
    tmp << "points=" << _config.pointsPerThread << std::endl;
    tmp << "compression=" << getCompressionString(_config.compression) << std::endl;
    tmp << "device=" << _config.device << std::endl;
    tmp << "elapsed=" << (_config.elapsed + util::getSystemTime() - _startTime) << std::endl;
    tmp << "stride=" << _config.stride.toString();
    tmp.close();
    if(!tmp) {
        throw KeySearchException("Unable to write checkpoint: " + temporary);
    }
#ifdef _WIN32
    if(!MoveFileExA(temporary.c_str(), _config.checkpointFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if(std::rename(temporary.c_str(), _config.checkpointFile.c_str()) != 0) {
#endif
        throw KeySearchException("Unable to replace checkpoint: " + _config.checkpointFile);
    }
}

void readCheckpointFile()
{
    if(_config.checkpointFile.length() == 0) {
        return;
    }

    ConfigFileReader reader(_config.checkpointFile);

    if(!reader.exists()) {
        return;
    }

    Logger::log(LogLevel::Info, "Loading ' " + _config.checkpointFile + "'");

    std::map<std::string, ConfigFileEntry> entries = reader.read();
    if(entries["partition"].value != _config.partition) {
        throw std::string("Checkpoint partition differs; use a separate CPU/GPU checkpoint");
    }

    if(entries.find("puzzle") != entries.end()) {
        uint32_t checkpointPuzzle = util::parseUInt32(entries["puzzle"].value);
        if(_config.puzzleBits != 0 && checkpointPuzzle != (uint32_t)_config.puzzleBits) {
            throw std::string("Checkpoint puzzle does not match selected --puzzle");
        }
    }

    _config.startKey = secp256k1::uint256(entries["start"].value);
    _config.nextKey = secp256k1::uint256(entries["next"].value);
    _config.endKey = secp256k1::uint256(entries["end"].value);

    if(_config.threads == 0 && entries.find("threads") != entries.end()) {
        _config.threads = util::parseUInt32(entries["threads"].value);
    }
    if(_config.blocks == 0 && entries.find("blocks") != entries.end()) {
        _config.blocks = util::parseUInt32(entries["blocks"].value);
    }
    if(_config.pointsPerThread == 0 && entries.find("points") != entries.end()) {
        _config.pointsPerThread = util::parseUInt32(entries["points"].value);
    }
    if(entries.find("compression") != entries.end()) {
        _config.compression = parseCompressionString(entries["compression"].value);
    }
    if(entries.find("elapsed") != entries.end()) {
        _config.elapsed = util::parseUInt64(entries["elapsed"].value);
    }
    if(entries.find("stride") != entries.end()) {
        _config.stride = secp256k1::uint256(entries["stride"].value);
    }

    if(_config.startKey.cmp(_config.endKey) > 0 || _config.nextKey.cmp(_config.startKey) < 0 ||
        _config.stride.cmp(0) == 0 || _config.stride.cmp(secp256k1::N) >= 0) {
        throw std::string("Invalid checkpoint range, next key, or stride");
    }

    _config.totalkeys = (_config.nextKey - _config.startKey).toUint64();
}

int run()
{
    if(_config.nextKey.cmp(_config.endKey) > 0) {
        Logger::log(LogLevel::Info, "Checkpoint range already completed");
        return 0;
    }
    if(!_config.cpu && (_config.device < 0 || _config.device >= _devices.size())) {
        Logger::log(LogLevel::Error, "device " + util::format(_config.device) + " does not exist");
        return 1;
    }

    Logger::log(LogLevel::Info, "Compression: " + getCompressionString(_config.compression));
    Logger::log(LogLevel::Info, "Puzzle:     #" + util::format((uint32_t)_config.puzzleBits));
    Logger::log(LogLevel::Info, "Starting at: " + _config.nextKey.toString());
    Logger::log(LogLevel::Info, "Ending at:   " + _config.endKey.toString());
    Logger::log(LogLevel::Info, "Counting by: " + _config.stride.toString());

    try {

        _lastUpdate = util::getSystemTime();
        _startTime = util::getSystemTime();

        // Use default parameters if they have not been set
        DeviceParameters params = {256, 32, 32};
        if(!_config.cpu) params = getDefaultParameters(_devices[_config.device]);

        if(_config.blocks == 0) {
            _config.blocks = params.blocks;
        }

        if(_config.threads == 0) {
            _config.threads = params.threads;
        }

        if(_config.pointsPerThread == 0) {
            _config.pointsPerThread = params.pointsPerThread;
        }

        // Get device context
        std::unique_ptr<KeySearchDevice> owned(_config.cpu ? static_cast<KeySearchDevice*>(new CpuKeySearchDevice(_config.cpuThreads)) :
            getDeviceContext(_devices[_config.device], _config.blocks, _config.threads, _config.pointsPerThread));
        KeySearchDevice *d = owned.get();

        KeyFinder f(_config.nextKey, _config.endKey, _config.compression, d, _config.stride);

        f.setResultCallback(resultCallback);
        f.setStatusInterval(_config.statusInterval);
        f.setStatusCallback(statusCallback);
#ifdef _WIN32
        if(!_config.controlName.empty() && !_control.open(_config.controlName)) {
            throw KeySearchException("Unable to open GUI control channel");
        }
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        f.setRunControl([] {
            const unsigned p = _control.percent(_config.usage);
            if(_control.data && _control.data->state != 5) InterlockedExchange(&_control.data->state, p ? 1 : 2);
            return p;
        }, [] { return _control.stopping(); });
#else
        f.setRunControl([] { return _config.usage; }, [] { return false; });
#endif

        f.init();

        f.setTargets(_config.targets);

        f.run();

        if(!_config.checkpointFile.empty()) {
            writeCheckpoint(f.getNextKey());
        }

#ifdef _WIN32
        if(_control.data && _control.data->state != 5) InterlockedExchange(&_control.data->state, 3);
#endif
    } catch(KeySearchException ex) {
        Logger::log(LogLevel::Info, "Error: " + ex.msg);
        return 1;
    }

    return 0;
}

/**
 * Parses a string in the form of x/y
 */
bool parseShare(const std::string &s, uint32_t &idx, uint32_t &total)
{
    size_t pos = s.find('/');
    if(pos == std::string::npos) {
        return false;
    }

    try {
        idx = util::parseUInt32(s.substr(0, pos));
    } catch(...) {
        return false;
    }

    try {
        total = util::parseUInt32(s.substr(pos + 1));
    } catch(...) {
        return false;
    }

    if(idx == 0 || total == 0) {
        return false;
    }

    if(idx > total) {
        return false;
    }

    return true;
}

int main(int argc, char **argv)
{
	bool optCompressed = false;
	bool optUncompressed = false;
    bool listDevices = false;
    bool listPuzzles = false;
    bool optShares = false;
    bool optThreads = false;
    bool optBlocks = false;
    bool optPoints = false;
    bool optStride = false;
    bool puzzleWasSet = false;
    bool keyspaceWasSet = false;
    bool selfTestSolved = false;
    secp256k1::uint256 keyspaceStart;
    secp256k1::uint256 keyspaceEnd;

    uint32_t shareIdx = 0;
    uint32_t numShares = 0;

#ifdef _WIN32
    _config.cpuThreads = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if(!_config.cpuThreads) _config.cpuThreads = 1;
#endif
    // Catch --help first
    for(int i = 1; i < argc; i++) {
        if(std::string(argv[i]) == "--cpu") _config.cpu = true;
        if(std::string(argv[i]) == "--help") {
            usage();
            return 0;
        }
        if(std::string(argv[i]) == "--list-puzzles") {
            PuzzleCatalog::print();
            return 0;
        }
    }

    // Check for supported devices
    if(!_config.cpu) try {
        _devices = DeviceManager::getDevices();

        if(_devices.size() == 0) {
            Logger::log(LogLevel::Error, "No devices available");
            return 1;
        }
    } catch(DeviceManager::DeviceManagerException ex) {
        Logger::log(LogLevel::Error, "Error detecting devices: " + ex.msg);
        return 1;
    }

    // Check for arguments
	if(argc == 1) {
		usage();
		return 0;
	}


	CmdParse parser;
	parser.add("-d", "--device", true);
	parser.add("-t", "--threads", true);
	parser.add("-b", "--blocks", true);
	parser.add("-p", "--points", true);
    parser.add("-d", "--device", true);
    parser.add("-c", "--compressed", false);
    parser.add("-u", "--uncompressed", false);
    parser.add("", "--compression", true);
	parser.add("-o", "--out", true);
    parser.add("-f", "--follow", false);
    parser.add("", "--list-devices", false);
    parser.add("", "--list-puzzles", false);
    parser.add("", "--self-test-solved", false);
    parser.add("", "--puzzle", true);
    parser.add("", "--keyspace", true);
    parser.add("", "--continue", true);
    parser.add("", "--share", true);
    parser.add("", "--stride", true);
    parser.add("", "--cpu", false);
    parser.add("", "--cpu-threads", true);
    parser.add("", "--usage", true);
    parser.add("", "--control", true);
    parser.add("", "--partition", true);
    parser.add("", "--partition-index", true);
    parser.add("", "--partition-count", true);

    try {
        parser.parse(argc, argv);
    } catch(std::string err) {
        Logger::log(LogLevel::Error, "Error: " + err);
        return 1;
    }

    std::vector<OptArg> args = parser.getArgs();

	for(unsigned int i = 0; i < args.size(); i++) {
		OptArg optArg = args[i];
		std::string opt = args[i].option;

		try {
            if(optArg.equals("", "--cpu")) {
                _config.cpu = true;
            } else if(optArg.equals("", "--cpu-threads")) {
                _config.cpuThreads = util::parseUInt32(optArg.arg);
                if(!_config.cpuThreads || _config.cpuThreads > 4096) throw std::string("CPU threads must be 1..4096");
            } else if(optArg.equals("", "--usage")) {
                _config.usage = util::parseUInt32(optArg.arg);
                if(_config.usage > 100) throw std::string("Usage must be 0..100");
            } else if(optArg.equals("", "--control")) {
                _config.controlName = optArg.arg;
            } else if(optArg.equals("", "--partition-index")) {
                _config.partitionIndex = util::parseUInt32(optArg.arg);
            } else if(optArg.equals("", "--partition-count")) {
                _config.partitionCount = util::parseUInt32(optArg.arg);
            } else if(optArg.equals("", "--partition")) {
                _config.partition = optArg.arg;
                if(_config.partition != "gpu" && _config.partition != "cpu" && _config.partition != "full") throw std::string("Partition must be full, gpu or cpu");
            } else if(optArg.equals("-t", "--threads")) {
				_config.threads = util::parseUInt32(optArg.arg);
                optThreads = true;
            } else if(optArg.equals("-b", "--blocks")) {
                _config.blocks = util::parseUInt32(optArg.arg);
                optBlocks = true;
			} else if(optArg.equals("-p", "--points")) {
				_config.pointsPerThread = util::parseUInt32(optArg.arg);
                optPoints = true;
			} else if(optArg.equals("-d", "--device")) {
				_config.device = util::parseUInt32(optArg.arg);
			} else if(optArg.equals("-c", "--compressed")) {
				optCompressed = true;
            } else if(optArg.equals("-u", "--uncompressed")) {
                optUncompressed = true;
            } else if(optArg.equals("", "--compression")) {
                _config.compression = parseCompressionString(optArg.arg);
			} else if(optArg.equals("-o", "--out")) {
				_config.resultsFile = optArg.arg;
            } else if(optArg.equals("", "--list-devices")) {
                listDevices = true;
            } else if(optArg.equals("", "--list-puzzles")) {
                listPuzzles = true;
            } else if(optArg.equals("", "--self-test-solved")) {
                selfTestSolved = true;
            } else if(optArg.equals("", "--puzzle")) {
                _config.puzzleBits = util::parseUInt32(optArg.arg);
                puzzleWasSet = true;
            } else if(optArg.equals("", "--continue")) {
                _config.checkpointFile = optArg.arg;
            } else if(optArg.equals("", "--keyspace")) {
                keyspaceWasSet = true;
                size_t keyspaceSep = optArg.arg.find(':');
                if(keyspaceSep == std::string::npos || keyspaceSep == optArg.arg.length() - 1) {
                    throw std::string("expected bounded range START:END, START:+COUNT, :END, or :+COUNT");
                }

                secp256k1::uint256 start;
                secp256k1::uint256 end;

                parseKeyspace(optArg.arg, start, end);

                if(start.cmp(secp256k1::N) > 0) {
                    throw std::string("argument is out of range");
                }
                if(start.isZero()) {
                    throw std::string("argument is out of range");
                }

                if(end.cmp(secp256k1::N) > 0) {
                    throw std::string("argument is out of range");
                }

                if(start.cmp(end) > 0) {
                    throw std::string("Invalid argument");
                }

                keyspaceStart = start;
                keyspaceEnd = end;
            } else if(optArg.equals("", "--share")) {
                if(!parseShare(optArg.arg, shareIdx, numShares)) {
                    throw std::string("Invalid argument");
                }
                optShares = true;
            } else if(optArg.equals("", "--stride")) {
                try {
                    _config.stride = secp256k1::uint256(optArg.arg);
                } catch(...) {
                    throw std::string("invalid argument: : expected hex string");
                }

                if(_config.stride.cmp(secp256k1::N) >= 0) {
                    throw std::string("argument is out of range");
                }

                if(_config.stride.cmp(0) == 0) {
                    throw std::string("argument is out of range");
                }
                optStride = true;
            } else if(optArg.equals("-f", "--follow")) {
                _config.follow = true;
            }

		} catch(std::string err) {
			Logger::log(LogLevel::Error, "Error " + opt + ": " + err);
			return 1;
		}
	}

    if(listDevices) {
        printDeviceList(_devices);
        return 0;
    }

    if(listPuzzles) {
        PuzzleCatalog::print();
        return 0;
    }

	// Verify device exists
	if(!_config.cpu && (_config.device < 0 || _config.device >= _devices.size())) {
		Logger::log(LogLevel::Error, "device " + util::format(_config.device) + " does not exist");
		return 1;
	}

	std::vector<std::string> ops = parser.getOperands();
    if(ops.size() != 0) {
        Logger::log(LogLevel::Error, "Raw target addresses are disabled in this OpenCL/CPU build. Use --puzzle N.");
        return 1;
    }

    if(selfTestSolved) {
        if(puzzleWasSet) {
            Logger::log(LogLevel::Error, "--self-test-solved cannot be combined with --puzzle.");
            return 1;
        }
        if(keyspaceWasSet || optShares || optStride || !_config.partition.empty() || _config.checkpointFile.length() > 0) {
            Logger::log(LogLevel::Error, "--self-test-solved uses a fixed hardcoded fixture range and cannot be combined with --keyspace, --share, --stride, or --continue.");
            return 1;
        }
        if(_config.compression != PointCompressionType::COMPRESSED || optUncompressed) {
            Logger::log(LogLevel::Error, "--self-test-solved uses the compressed solved puzzle fixture only.");
            return 1;
        }

        _config.puzzleBits = SOLVED_SELF_TEST_PUZZLE.bits;
        parseKeyspace(SOLVED_SELF_TEST_KEYSPACE, keyspaceStart, keyspaceEnd);
        keyspaceWasSet = true;

        if(_config.resultsFile.length() == 0) {
            _config.resultsFile = "selftest_results.txt";
        }
    } else if(!puzzleWasSet) {
        Logger::log(LogLevel::Error, "Missing --puzzle N. Use --list-puzzles to see hardcoded public puzzle targets.");
        return 1;
    }

    const PuzzleDefinition *puzzle = selfTestSolved ? &SOLVED_SELF_TEST_PUZZLE : PuzzleCatalog::find(_config.puzzleBits);
    if(puzzle == NULL) {
        Logger::log(LogLevel::Error, "Puzzle #" + util::format((uint32_t)_config.puzzleBits) + " is not in the hardcoded unsolved puzzle catalog.");
        return 1;
    }

    secp256k1::uint256 puzzleMin(puzzle->rangeMin);
    secp256k1::uint256 puzzleMax(puzzle->rangeMax);

    _config.startKey = puzzleMin;
    _config.nextKey = puzzleMin;
    _config.endKey = puzzleMax;
    _config.targets.clear();
    _config.targets.push_back(puzzle->address);

    if(keyspaceWasSet) {
        if(keyspaceStart.cmp(puzzleMin) < 0 || keyspaceEnd.cmp(puzzleMax) > 0) {
            Logger::log(LogLevel::Error, "--keyspace must stay inside the selected puzzle range "
                + std::string(puzzle->rangeMin) + ":" + std::string(puzzle->rangeMax));
            return 1;
        }

        _config.startKey = keyspaceStart;
        _config.nextKey = keyspaceStart;
        _config.endKey = keyspaceEnd;
    }

    if(!_config.partitionCount || _config.partitionCount > 4096 ||
        _config.partitionIndex >= _config.partitionCount ||
        (_config.partition.empty() && (_config.partitionCount != 1 || _config.partitionIndex != 0))) {
        Logger::log(LogLevel::Error, "Invalid partition slice; specify a partition, count 1..4096 and index 0..count-1");
        return 1;
    }
    if(!_config.partition.empty()) {
        if(keyspaceWasSet || optShares || optStride) {
            Logger::log(LogLevel::Error, "--partition cannot be combined with keyspace, share or stride");
            return 1;
        }
        const auto boundary = SearchArithmetic::cpuBoundary(puzzleMin, puzzleMax);
        if(_config.partition == "cpu") _config.startKey = _config.nextKey = boundary;
        else if(_config.partition == "gpu") _config.endKey = boundary - 1;
        const auto start = _config.startKey, end = _config.endKey;
        _config.startKey = _config.nextKey = SearchArithmetic::sliceStart(start, end, _config.partitionIndex, _config.partitionCount);
        _config.endKey = SearchArithmetic::sliceEnd(start, end, _config.partitionIndex, _config.partitionCount);
    }
    // Calculate where to start and end in the keyspace when the --share option is used
    if(optShares) {
        Logger::log(LogLevel::Info, "Share " + util::format(shareIdx) + " of " + util::format(numShares));
        secp256k1::uint256 numKeys = _config.endKey - _config.nextKey + 1;

        secp256k1::uint256 diff = numKeys.mod(numShares);
        numKeys = numKeys - diff;

        secp256k1::uint256 shareSize = numKeys.div(numShares);

        secp256k1::uint256 startPos = _config.nextKey + (shareSize * (shareIdx - 1));

        if(shareIdx < numShares) {
            secp256k1::uint256 endPos = _config.nextKey + (shareSize * (shareIdx)) - 1;
            _config.endKey = endPos;
        }

        _config.nextKey = startPos;
        _config.startKey = startPos;
    }

	// Check option for compressed, uncompressed, or both
	if(optCompressed && optUncompressed) {
		_config.compression = PointCompressionType::BOTH;
	} else if(optCompressed) {
		_config.compression = PointCompressionType::COMPRESSED;
	} else if(optUncompressed) {
		_config.compression = PointCompressionType::UNCOMPRESSED;
	}

#ifdef _WIN32
    CheckpointLock checkpointLock;
    if(!_config.checkpointFile.empty() && !checkpointLock.acquire(_config.checkpointFile)) {
        Logger::log(LogLevel::Error, "Checkpoint is in use or its folder is not writable.");
        return 1;
    }
#endif
    if(_config.checkpointFile.length() > 0) {
        const secp256k1::uint256 requestedStart = _config.startKey;
        const secp256k1::uint256 requestedEnd = _config.endKey;
        const secp256k1::uint256 requestedStride = _config.stride;
        try {
            readCheckpointFile();
        } catch(std::string err) {
            Logger::log(LogLevel::Error, "Error --continue: " + err);
            return 1;
        }

        if(((keyspaceWasSet || optShares || !_config.partition.empty()) &&
            (!(_config.startKey == requestedStart) || !(_config.endKey == requestedEnd))) ||
            (optStride && !(_config.stride == requestedStride))) {
            Logger::log(LogLevel::Error, "Checkpoint conflicts with requested range or stride; use a separate checkpoint file.");
            return 1;
        }

        if(_config.startKey.cmp(puzzleMin) < 0 || _config.endKey.cmp(puzzleMax) > 0) {
            Logger::log(LogLevel::Error, "Checkpoint range is outside selected puzzle range.");
            return 1;
        }
    }

    return run();
}
