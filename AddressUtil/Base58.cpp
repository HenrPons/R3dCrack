#include <map>
#include <vector>
#include "CryptoUtil.h"

#include "AddressUtil.h"


static const std::string BASE58_STRING = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

struct Base58Map {
	static std::map<char, int> createBase58Map()
	{
		std::map<char, int> m;
		for(int i = 0; i < 58; i++) {
			m[BASE58_STRING[i]] = i;
		}

		return m;
	}

	static std::map<char, int> myMap;
};

std::map<char, int> Base58Map::myMap = Base58Map::createBase58Map();

static void sha256Bytes(const std::vector<unsigned char> &bytes, unsigned int digest[8])
{
	unsigned int msg[16] = { 0 };

	for(size_t i = 0; i < bytes.size(); i++) {
		msg[i / 4] |= ((unsigned int)bytes[i]) << (24 - ((i % 4) * 8));
	}

	msg[bytes.size() / 4] |= 0x80 << (24 - ((bytes.size() % 4) * 8));
	msg[15] = (unsigned int)(bytes.size() * 8);

	crypto::sha256Init(digest);
	crypto::sha256(msg, digest);
}

static void appendBase58CheckChecksum(std::vector<unsigned char> &payload)
{
	unsigned int first[8] = { 0 };
	unsigned int second[8] = { 0 };
	std::vector<unsigned char> firstBytes;

	sha256Bytes(payload, first);

	for(int i = 0; i < 8; i++) {
		firstBytes.push_back((unsigned char)(first[i] >> 24));
		firstBytes.push_back((unsigned char)(first[i] >> 16));
		firstBytes.push_back((unsigned char)(first[i] >> 8));
		firstBytes.push_back((unsigned char)first[i]);
	}

	sha256Bytes(firstBytes, second);

	payload.push_back((unsigned char)(second[0] >> 24));
	payload.push_back((unsigned char)(second[0] >> 16));
	payload.push_back((unsigned char)(second[0] >> 8));
	payload.push_back((unsigned char)second[0]);
}

static std::string encodeBase58Bytes(const std::vector<unsigned char> &bytes)
{
	size_t zeroes = 0;
	while(zeroes < bytes.size() && bytes[zeroes] == 0) {
		zeroes++;
	}

	std::vector<unsigned char> value(bytes.begin(), bytes.end());
	std::string encoded;
	size_t start = zeroes;

	while(start < value.size()) {
		int remainder = 0;

		for(size_t i = start; i < value.size(); i++) {
			int current = (remainder << 8) + value[i];
			value[i] = (unsigned char)(current / 58);
			remainder = current % 58;
		}

		encoded = BASE58_STRING[remainder] + encoded;

		while(start < value.size() && value[start] == 0) {
			start++;
		}
	}

	return std::string(zeroes, '1') + encoded;
}

std::string Address::privateKeyToWif(const secp256k1::uint256 &key, bool compressed)
{
	unsigned int words[8] = { 0 };
	std::vector<unsigned char> payload;

	payload.push_back(0x80);
	key.exportWords(words, 8, secp256k1::uint256::BigEndian);

	for(int i = 0; i < 8; i++) {
		payload.push_back((unsigned char)(words[i] >> 24));
		payload.push_back((unsigned char)(words[i] >> 16));
		payload.push_back((unsigned char)(words[i] >> 8));
		payload.push_back((unsigned char)words[i]);
	}

	if(compressed) {
		payload.push_back(0x01);
	}

	appendBase58CheckChecksum(payload);

	return encodeBase58Bytes(payload);
}



/**
 * Converts a base58 string to uint256
 */
secp256k1::uint256 Base58::toBigInt(const std::string &s)
{
	secp256k1::uint256 value;

	for(unsigned int i = 0; i < s.length(); i++) {
		value = value.mul(58);

		int c = Base58Map::myMap[s[i]];
		value = value.add(c);
	}

	return value;
}

void Base58::toHash160(const std::string &s, unsigned int hash[5])
{
	secp256k1::uint256 value = toBigInt(s);
	unsigned int words[6];

	value.exportWords(words, 6, secp256k1::uint256::BigEndian);

	// Extract words, ignore checksum
	for(int i = 0; i < 5; i++) {
		hash[i] = words[i];
	}
}

bool Base58::isBase58(std::string s)
{
	for(unsigned int i = 0; i < s.length(); i++) {
		if(BASE58_STRING.find(s[i]) == std::string::npos) {
			return false;
		}
	}

	return true;
}

std::string Base58::toBase58(const secp256k1::uint256 &x)
{
	std::string s;

	secp256k1::uint256 value = x;

	while(!value.isZero()) {
		secp256k1::uint256 digit = value.mod(58);
		int digitInt = digit.toInt32();

		s = BASE58_STRING[digitInt] + s;

		value = value.div(58);
	}

	return s;
}

void Base58::getMinMaxFromPrefix(const std::string &prefix, secp256k1::uint256 &minValueOut, secp256k1::uint256 &maxValueOut)
{
	secp256k1::uint256 minValue = toBigInt(prefix);
	secp256k1::uint256 maxValue = minValue;
	int exponent = 1;

	// 2^192
	unsigned int expWords[] = { 0, 0, 0, 0, 0, 0, 1, 0 };

	secp256k1::uint256 exp(expWords);

	// Find the smallest 192-bit number that starts with the prefix. That is, the prefix multiplied
	// by some power of 58
	secp256k1::uint256 nextValue = minValue.mul(58);

	while(nextValue.cmp(exp) < 0) {
		exponent++;
		minValue = nextValue;
		nextValue = nextValue.mul(58);
	}

	secp256k1::uint256 diff = secp256k1::uint256(58).pow(exponent - 1).sub(1);

	maxValue = minValue.add(diff);

	if(maxValue.cmp(exp) > 0) {
		maxValue = exp.sub(1);
	}

	minValueOut = minValue;
	maxValueOut = maxValue;
}
