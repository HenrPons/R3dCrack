#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "AddressUtil.h"
#include "PuzzleCatalog.h"
#include "secp256k1.h"
#include "KeyFinder.h"
#include "SearchArithmetic.h"
#include "CpuKeySearchDevice.h"
#include "RunControl.h"
#include "ResultSafety.h"

static int failures = 0;

static void expectEqual(const std::string &name, const std::string &actual, const std::string &expected)
{
    if(actual != expected) {
        std::cerr << "FAIL " << name << "\n"
            << "  expected: " << expected << "\n"
            << "  actual:   " << actual << "\n";
        failures++;
    }
}

static void expectTrue(const std::string &name, bool value)
{
    if(!value) {
        std::cerr << "FAIL " << name << "\n";
        failures++;
    }
}

static void expectFalse(const std::string &name, bool value)
{
    if(value) {
        std::cerr << "FAIL " << name << "\n";
        failures++;
    }
}

static std::string readTextFile(const std::string &path)
{
    std::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if(!in) {
        std::cerr << "FAIL read file\n"
            << "  path: " << path << "\n";
        failures++;
        return "";
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

static void expectContains(const std::string &name, const std::string &haystack, const std::string &needle)
{
    if(haystack.find(needle) == std::string::npos) {
        std::cerr << "FAIL " << name << "\n"
            << "  missing: " << needle << "\n";
        failures++;
    }
}

static void expectNotContains(const std::string &name, const std::string &haystack, const std::string &needle)
{
    if(haystack.find(needle) != std::string::npos) {
        std::cerr << "FAIL " << name << "\n"
            << "  forbidden: " << needle << "\n";
        failures++;
    }
}

class CountingDevice : public KeySearchDevice {
public:
    secp256k1::uint256 next;
    secp256k1::uint256 stride;
    unsigned int steps = 0;
    void init(const secp256k1::uint256 &start, int, const secp256k1::uint256 &s) override { next = start; stride = s; }
    void doStep() override { next = next + stride * 4U; steps++; }
    void setTargets(const std::set<KeySearchTarget> &) override {}
    size_t getResults(std::vector<KeySearchResult> &) override { return 0; }
    uint64_t keysPerStep() override { return 4; }
    std::string getDeviceName() override { return "test device"; }
    void getMemoryInfo(uint64_t &free, uint64_t &total) override { free = total = 0; }
    secp256k1::uint256 getNextKey() override { return next; }
};

static void checkInclusiveRange(int end, int stride, unsigned int expectedSteps)
{
    CountingDevice device;
    KeyFinder finder(100, end, PointCompressionType::COMPRESSED, &device, stride);
    std::vector<std::string> targets = {"1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU"};
    finder.setStatusCallback([](KeySearchStatus) {});
    finder.init();
    finder.setTargets(targets);
    finder.run();
    expectTrue("inclusive range visits final batch", device.steps == expectedSteps);
}

static void checkCpuCoverage(unsigned workers, unsigned stride, int compression, const char *startHex="400000000000000000")
{
    using namespace secp256k1;
    const uint256 start(startHex);
    const uint256 end = start + 130;
    std::set<KeySearchTarget> targets;
    std::set<std::string> expected, actual;
    // Include off-stride and out-of-range targets to detect accidental work.
    for(unsigned offset = 0; offset <= 140; ++offset) {
        auto key = start + offset;
        auto point = multiplyPoint(key, G());
        for(int mode = 0; mode < 2; ++mode) {
            if(compression != PointCompressionType::BOTH && compression != mode) continue;
            unsigned int hash[5];
            if(mode == 0) Hash::hashPublicKeyCompressed(point, hash); else Hash::hashPublicKey(point, hash);
            targets.insert(KeySearchTarget(hash));
            if(offset <= 130 && offset % stride == 0) expected.insert(key.toString() + ":" + std::to_string(mode));
        }
    }
    CpuKeySearchDevice cpu(workers);
    cpu.init(start, compression, stride); cpu.setEndKey(end); cpu.setTargets(targets);
    unsigned count = 0;
    while(cpu.getNextKey().cmp(end) <= 0) {
        cpu.doStep(); count += (unsigned)cpu.keysPerStep();
        std::vector<KeySearchResult> results; cpu.getResults(results);
        for(auto &r : results) {
            expectTrue("CPU key matches public point", multiplyPoint(r.privateKey, G()) == r.publicKey);
            expectTrue("CPU batches do not overlap", actual.insert(r.privateKey.toString() + ":" + (r.compressed ? "0" : "1")).second);
        }
    }
    expectTrue("CPU covers every in-range on-stride target", actual == expected);
    expectTrue("CPU final partial batch count is exact", count == 130 / stride + 1);
}

class OverflowDevice : public CountingDevice {
public:
    void doStep() override { ResultSafety::requireComplete(129,128); CountingDevice::doStep(); }
};

int main()
{
    ResultSafety::requireComplete(128,128);
    {
        OverflowDevice device;
        KeyFinder finder(100,200,PointCompressionType::COMPRESSED,&device,1);
        finder.init(); bool rejected=false;
        try { finder.run(); } catch(KeySearchException &) { rejected=true; }
        expectTrue("overflow stops before progress advances",rejected && device.steps==0 && finder.getNextKey()==secp256k1::uint256(100));
    }
    checkCpuCoverage(2,1,PointCompressionType::COMPRESSED,"FFFFFFC0");
    checkCpuCoverage(2,1,PointCompressionType::COMPRESSED,"FFFFFFFFFFFFFFC0");
    for(int bits:{135,140,145}) expectTrue("shared catalog includes omitted GUI puzzles",PuzzleCatalog::find(bits)!=nullptr);
    for(unsigned size:{1031U,4099U}) {
        auto boundary=SearchArithmetic::cpuBoundary(100,100+size-1);
        expectTrue("uneven CPU allocation retains remainder in GPU",boundary==secp256k1::uint256(100+size-size/1024));
        auto next=secp256k1::uint256(100);
        for(unsigned index=0;index<3;++index) {
            auto begin=SearchArithmetic::sliceStart(100,boundary-1,index,3);
            auto end=SearchArithmetic::sliceEnd(100,boundary-1,index,3);
            expectTrue("uneven GPU partition has no gaps",begin==next); next=end+1;
        }
        expectTrue("GPU slices end exactly at CPU start",next==boundary);
    }

#ifdef _WIN32
    {
        RunControl first, collision, worker;
        auto name="Local\\RedCrackTest-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
        expectTrue("control mapping created", first.create(name,73));
        expectTrue("existing mapping cannot be overwritten", !collision.create(name,1));
        expectTrue("mapping collision preserves original state",first.percent(0)==73);
        expectTrue("worker opens mapping",worker.open(name));
        expectTrue("worker reads control",worker.percent(0)==73 && !worker.stopping());
    }
#endif

    checkCpuCoverage(1, 1, PointCompressionType::COMPRESSED);
    checkCpuCoverage(3, 3, PointCompressionType::BOTH);
    expectTrue("partial GPU batch includes endpoint", SearchArithmetic::validPoints(100, 106, 2, 100) == 4);
    expectTrue("empty batch does not search", SearchArithmetic::validPoints(107, 106, 1, 100) == 0);
    expectTrue("CPU/GPU partition union is exact", SearchArithmetic::cpuBoundary(1024, 2047) == secp256k1::uint256(2047));
    // Non-even division and 256-bit arithmetic: every slice meets its neighbour.
    for(unsigned count : {1U,2U,3U,7U,64U}) {
        secp256k1::uint256 low("400000000000000000"), high("7FEFFFFFFFFFFFFFFF");
        auto next=low;
        for(unsigned index=0;index<count;++index) {
            auto begin=SearchArithmetic::sliceStart(low,high,index,count);
            auto end=SearchArithmetic::sliceEnd(low,high,index,count);
            expectTrue("GPU slices have no gaps or overlap", begin==next && begin.cmp(end)<=0);
            next=end+1;
        }
        expectTrue("GPU slices include final key",next==high+1);
    }

    checkInclusiveRange(100, 1, 1);
    checkInclusiveRange(103, 1, 1);
    checkInclusiveRange(104, 1, 2);
    checkInclusiveRange(108, 2, 2);

    expectTrue("stride applies to entire iteration", SearchArithmetic::offset(32, 2, 7, 3) == secp256k1::uint256(213));
    expectTrue("iteration offset does not overflow 64 bits",
        SearchArithmetic::offset(0x100000000ULL, 0x100000000ULL, 1, 2) == secp256k1::uint256("20000000000000002"));
    unsigned int words[8] = {0};
    expectTrue("zero scalar keeps one initialization pass", SearchArithmetic::scalarBits(words) == 1);
    words[2] = 0x40;
    expectTrue("puzzle 71 initializes 71 bits", SearchArithmetic::scalarBits(words) == 71);
    words[2] = 0x80;
    expectTrue("carry into bit 72 is preserved", SearchArithmetic::scalarBits(words) == 72);
    words[7] = 0x80000000;
    expectTrue("full width scalar supported", SearchArithmetic::scalarBits(words) == 256);

    secp256k1::uint256 one(1);

    expectEqual(
        "compressed WIF for private key 1",
        Address::privateKeyToWif(one, true),
        "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn");

    expectEqual(
        "uncompressed WIF for private key 1",
        Address::privateKeyToWif(one, false),
        "5HpHagT65TZzG1PH3CSu63k8DbpvD8s5ip4nEB3kEsreAnchuDf");

    secp256k1::uint256 puzzle66("000000000000000000000000000000000000000000000002832ed74f2b5e35ee");
    secp256k1::ecpoint puzzle66PublicKey = secp256k1::multiplyPoint(puzzle66, secp256k1::G());

    expectEqual(
        "puzzle 66 compressed public key",
        puzzle66PublicKey.toString(true),
        "024EE2BE2D4E9F92D2F5A4A03058617DC45BEFE22938FEED5B7A6B7282DD74CBDD");

    expectEqual(
        "puzzle 66 compressed address",
        Address::fromPublicKey(puzzle66PublicKey, true),
        "13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so");

    expectEqual(
        "puzzle 66 compressed WIF",
        Address::privateKeyToWif(puzzle66, true),
        "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qZfFoWMiwBt943V7CQeX");

    expectTrue("known P2PKH address validates", Address::verifyAddress("1BoatSLRHtKNngkdXEeobR76b53LETtpyT"));
    expectFalse("Base58 rejects zero", Address::verifyAddress("10atSLRHtKNngkdXEeobR76b53LETtpyT"));
    expectFalse("too-long address rejects", Address::verifyAddress("1BoatSLRHtKNngkdXEeobR76b53LETtpyT12345"));
    expectTrue("puzzle catalog has 78 unsolved targets", PuzzleCatalog::all().size() == 78);
    expectTrue("puzzle 71 is hardcoded", PuzzleCatalog::find(71) != NULL);
    expectFalse("solved puzzle 75 is not hardcoded", PuzzleCatalog::find(75) != NULL);

    std::string keyFinderMain = readTextFile("..\\..\\KeyFinder\\main.cpp");
    expectContains(
        "solved self-test mode is hardcoded",
        keyFinderMain,
        "--self-test-solved");
    expectContains(
        "solved self-test fixture uses puzzle 66 address",
        keyFinderMain,
        "13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so");
    expectContains(
        "solved self-test fixture uses bounded local range",
        keyFinderMain,
        "2832ed74f2b5e3000:2832ed74f2b5e4000");

    std::string secp256k1Cl = readTextFile("..\\..\\clMath\\secp256k1.cl");
    expectContains(
        "OpenCL P_MINUS1 constant is P minus one",
        secp256k1Cl,
        "0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFC2E");

    std::string keysearchCl = readTextFile("..\\..\\CLKeySearchDevice\\keysearch.cl");
    expectContains(
        "OpenCL result buffer defines MAX_RESULTS",
        keysearchCl,
        "#define MAX_RESULTS 128");
    expectContains(
        "OpenCL result writes are bounded",
        keysearchCl,
        "if(count < MAX_RESULTS)");

    std::string clDeviceCpp = readTextFile("..\\..\\CLKeySearchDevice\\CLKeySearchDevice.cpp");
    expectContains("OpenCL validates complete result batches", clDeviceCpp,
        "ResultSafety::requireComplete(numResults, MAX_DEVICE_RESULTS)");
    expectNotContains("OpenCL uses standard build options", clDeviceCpp, "-cl-unsafe-math-optimizations");

    std::string guiMain = readTextFile("..\\..\\PuzzleGui\\HybridGui.cpp");
    expectContains("GUI uses worker catalog",guiMain,"PuzzleCatalog::all()");
    expectContains(
        "GUI launches only puzzle mode",
        guiMain,
        "--puzzle ");
    expectContains(
        "GUI exposes solved self-test",
        guiMain,
        "--self-test-solved");
    expectContains(
        "GUI displays WIF from result file",
        guiMain,
        "Copy WIF");
    expectNotContains(
        "GUI does not offer manual public key targets",
        guiMain,
        "--pubkey");
    expectNotContains(
        "GUI does not offer target files",
        guiMain,
        "--target-file");

    if(failures != 0) {
        return 1;
    }

    std::cout << "HostUnitTests passed\n";
    return 0;
}
