/*
	Copyright 2025 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "v42bis.h"

namespace modem
{
namespace v42b
{

enum
{
	// Control code words (compressed mode)
	ETM = 0,         // Enter transparent mode
	FLUSH = 1,       // Flush data
	STEPUP = 2,      // Step up codeword size

	// Command codes (transparent mode)
	ECM = 0,         // Enter compression mode
	EID = 1,         // Escape character in data
	RESET = 2        // Force reinitialization
};

void Dictionary::reset()
{
	for (u32 i = 0; i < MAX_TOTAL_CODEWORDS; i++)
	{
		if (i >= CW_RESERVED && i < CW_FIRST)
		{
			parent[i] = ROOT;
			character[i] = (u8)(i - CW_RESERVED);
		}
		else
		{
			parent[i] = NONE;
			character[i] = 0;
		}
		firstChild[i] = NONE;
		nextSibling[i] = NONE;
	}
}

u16 Dictionary::getChild(u16 code, u8 c) const
{
	// A node has a child for each character at most
	int left = ALPHABET_SIZE;
	for (u16 n = firstChild[code]; n != NONE && left > 0; n = nextSibling[n], left--)
		if (character[n] == c)
			return n;
	return NONE;
}

void Dictionary::addChild(u16 childCode, u16 parentCode, u8 c)
{
	parent[childCode] = parentCode;
	character[childCode] = c;
	firstChild[childCode] = NONE;
	nextSibling[childCode] = firstChild[parentCode];
	firstChild[parentCode] = childCode;
}

void Dictionary::detach(u16 code)
{
	const u16 p = parent[code];
	if (p != NONE && p != ROOT)
	{
		u16 *link = &firstChild[p];
		int left = ALPHABET_SIZE;
		while (*link != NONE && *link != code && left-- > 0)
			link = &nextSibling[*link];
		if (*link == code)
			*link = nextSibling[code];
	}
	parent[code] = NONE;
	nextSibling[code] = NONE;
}

/* The strings, by their parent and last character. Which children a node
 * has follows from them, and is worked out again on a load. */
void Dictionary::serialize(void **data, unsigned int *total_size) const
{
	// (what a free node had before is not kept)
	u8 chars[MAX_TOTAL_CODEWORDS - CW_FIRST];
	for (u32 i = CW_FIRST; i < MAX_TOTAL_CODEWORDS; i++)
		chars[i - CW_FIRST] = parent[i] == NONE ? 0 : character[i];
	LIBRETRO_SA(&parent[CW_FIRST], MAX_TOTAL_CODEWORDS - CW_FIRST);
	LIBRETRO_SA(chars, MAX_TOTAL_CODEWORDS - CW_FIRST);
}

void Dictionary::unserialize(void **data, unsigned int *total_size, u32 limit)
{
	reset();
	LIBRETRO_USA(&parent[CW_FIRST], MAX_TOTAL_CODEWORDS - CW_FIRST);
	LIBRETRO_USA(&character[CW_FIRST], MAX_TOTAL_CODEWORDS - CW_FIRST);

	/* A string is kept if it leads to a single character in no more steps
	 * than the longest string has characters, through strings of the
	 * dictionary: then nothing that walks the tree goes outside it or round
	 * in circles. A string that does not is dropped, and every longer string
	 * made from it is dropped with it, since its walk goes the same way. */
	static u8 keep[MAX_TOTAL_CODEWORDS];
	for (u32 i = CW_FIRST; i < MAX_TOTAL_CODEWORDS; i++)
	{
		keep[i] = 0;
		if (i >= limit || parent[i] == NONE)
			continue;
		u32 n = i;
		for (int steps = 0; steps <= MAX_STRING_SIZE; steps++)
		{
			const u16 p = parent[n];
			if (p < CW_RESERVED || (p >= limit && p != ROOT))
				break;
			if (p == ROOT)
			{
				keep[i] = n < CW_FIRST;
				break;
			}
			n = p;
		}
	}
	for (u32 i = CW_FIRST; i < MAX_TOTAL_CODEWORDS; i++)
	{
		if (!keep[i])
		{
			parent[i] = NONE;
			character[i] = 0;
		}
	}
	for (u32 i = CW_FIRST; i < MAX_TOTAL_CODEWORDS; i++)
		if (parent[i] != NONE)
		{
			nextSibling[i] = firstChild[parent[i]];
			firstChild[parent[i]] = (u16)i;
		}
}

void V42Base::setParams(u32 maxCodeWords, int maxStringLength)
{
	if (maxCodeWords < MIN_TOTAL_CODEWORDS)
		maxCodeWords = MIN_TOTAL_CODEWORDS;
	else if (maxCodeWords > MAX_TOTAL_CODEWORDS)
		maxCodeWords = MAX_TOTAL_CODEWORDS;
	if (maxStringLength < DEF_STRING_SIZE)
		maxStringLength = DEF_STRING_SIZE;
	else if (maxStringLength > MAX_STRING_SIZE)
		maxStringLength = MAX_STRING_SIZE;
	this->maxCodeWords = maxCodeWords;
	this->maxStringLength = maxStringLength;
	resetBase();
}

void V42Base::resetBase()
{
	dict.reset();
	nextCodeWord = CW_FIRST;
	codeWordSize = CHAR_SIZE + 1;
	codeWordThreshold = 2 * ALPHABET_SIZE;
	lastAddedCode = -1;
	matchLen = 0;
	curCode = -1;
	transparent = true;
	modeTransition = false;
	escapeCode = 0;
}

void V42Base::recoverNode()
{
	// 7.7 Node recovery
	// 6.5 Recovering a dictionary entry to use next
	// (a tree has a leaf among the nodes that are not single characters: one
	// round is enough)
	for (u32 i = CW_FIRST; i <= maxCodeWords; i++)
	{
		// 6.5(a) and (b) counter C1 shall be incremented; if the value of C1 exceeds N2-1 then C1 shall be set to N5
		if (++nextCodeWord >= maxCodeWords)
			nextCodeWord = CW_FIRST;
		// 6.5(c) We need to reuse a leaf or find a free node
		if (!dict.isLeaf(nextCodeWord))
			continue;
		// 6.5(d) if the node is a leaf node, then it shall be detached from its parent
		if (!dict.isFree(nextCodeWord))
			dict.detach(nextCodeWord);
		break;
	}
}

int V42Base::acceptChar(u8 octet)
{
	if (curCode == -1)
	{
		curCode = octet + CW_RESERVED;
		matchLen = 1;
		modeTransition = false;
		return -1;
	}
	matchLen++;
	const u16 node = dict.getChild(curCode, octet);
	if (node != Dictionary::NONE && !modeTransition)
	{
		// 6.3(b) If the string matches a dictionary entry, and the entry is not that entry
		// created by the last invocation of the string matching procedure, then the
		// next character shall be read and appended to the string and this step
		// repeated.
		if (node != lastAddedCode) {
			curCode = node;
			return -1;
		}
	}
	// 6.4 Add the string to the dictionary
	if (node == Dictionary::NONE && matchLen <= maxStringLength)
	{
		checkDictSize();
		dict.addChild(nextCodeWord, curCode, octet);
		lastAddedCode = nextCodeWord;
		recoverNode();
	}
	else {
		lastAddedCode = -1;
	}
	int emitCode;
	if (modeTransition)
		// this code's string has already been pushed in transparent mode
		emitCode = -1;
	else
		emitCode = curCode;
	modeTransition = false;
	curCode = octet + CW_RESERVED;
	matchLen = 1;
	return emitCode;
}

void V42Base::serializeBase(void **data, unsigned int *total_size) const
{
	u16 n2 = (u16)maxCodeWords;
	u8 n7 = (u8)maxStringLength;
	u16 c1 = (u16)nextCodeWord;
	u8 c2 = (u8)codeWordSize;
	s32 lastAdded = lastAddedCode;
	s32 len = matchLen;
	s32 cur = curCode;
	u8 flags = (transparent ? 1 : 0) | (modeTransition ? 2 : 0);

	LIBRETRO_S(n2);
	LIBRETRO_S(n7);
	LIBRETRO_S(c1);
	LIBRETRO_S(c2);
	LIBRETRO_S(lastAdded);
	LIBRETRO_S(len);
	LIBRETRO_S(cur);
	LIBRETRO_S(flags);
	LIBRETRO_S(escapeCode);
	dict.serialize(data, total_size);
}

void V42Base::unserializeBase(void **data, unsigned int *total_size)
{
	u16 n2 = 0;
	u8 n7 = 0;
	u16 c1 = 0;
	u8 c2 = 0;
	s32 lastAdded = -1;
	s32 len = 0;
	s32 cur = -1;
	u8 flags = 1;
	u8 escape = 0;

	LIBRETRO_US(n2);
	LIBRETRO_US(n7);
	LIBRETRO_US(c1);
	LIBRETRO_US(c2);
	LIBRETRO_US(lastAdded);
	LIBRETRO_US(len);
	LIBRETRO_US(cur);
	LIBRETRO_US(flags);
	LIBRETRO_US(escape);
	setParams(n2, n7);
	dict.unserialize(data, total_size, maxCodeWords);

	nextCodeWord = c1 >= CW_FIRST && c1 < maxCodeWords ? c1 : CW_FIRST;
	// C2 goes from 9 bits to as many as the largest codeword has
	codeWordSize = CHAR_SIZE + 1;
	while (codeWordSize < c2 && (1u << codeWordSize) < maxCodeWords)
		codeWordSize++;
	codeWordThreshold = 1u << codeWordSize;
	lastAddedCode = lastAdded >= (s32)CW_FIRST && lastAdded < (s32)maxCodeWords ? lastAdded : -1;
	matchLen = len >= 0 && len <= MAX_STRING_SIZE ? len : 0;
	curCode = cur >= (s32)CW_RESERVED && cur < (s32)maxCodeWords && !dict.isFree(cur) ? cur : -1;
	transparent = (flags & 1) != 0;
	modeTransition = (flags & 2) != 0;
	escapeCode = escape;
}

void Compressor::pushCode(u16 code)
{
	outBitBuf |= (u32)code << outBitPos;
	outBitPos += codeWordSize;
	while (outBitPos >= 8)
	{
		push(outBitBuf & 0xff);
		outBitBuf >>= 8;
		outBitPos -= 8;
	}
}

void Compressor::write(u8 b)
{
	if (transparent)
		push(b);
	if (b == escapeCode)
	{
		if (transparent)
			push(EID);
		escapeCode += 51;
	}
	int matchedLen = this->matchLen;
	int code = acceptChar(b);
	if (code != -1)
	{
		if (!transparent)
			// 7.5 Transfer - output the last state of the string
			pushCode(code);

		// 7.8 Data compressibility test
		// Same algorithm as the dreamcast modem driver, but we could use something different
		// Accumulate the difference in bits between compressed and uncompressed size. Positive numbers
		// indicate compressible data.
		compressTest += 8 * matchedLen - (int)codeWordSize;
		compressTestCount -= matchedLen;
		if (compressTestCount <= 0)
		{
			if (transparent && compressTest > 16)
				// Schedule a switch to compressed mode
				changeMode();
			else if (!transparent && compressTest < -16)
				// Schedule a switch to transparent mode
				changeMode();
		}
	}
}

void Compressor::changeMode()
{
	if (transparent)
	{
		DEBUG_LOG(MODEM, "Compressor: switching to compressed mode");
		// 7.8.1 Transition to compressed mode
		push(escapeCode);
		push(ECM);
		transparent = false;
		compressTestCount = 256;
	}
	else
	{
		DEBUG_LOG(MODEM, "Compressor: switching to transparent mode");
		// 7.8.2 Transition to transparent mode
		flush(true);
		transparent = true;
		compressTestCount = 64;
	}
	modeTransition = true;
	compressTest = 0;
}

void Compressor::reset()
{
	resetBase();
	outBitBuf = 0;
	outBitPos = 0;
	compressTest = 0;
	compressTestCount = 64;
	out.clear();
}

void Compressor::checkDictSize()
{
	if (transparent)
		return;
	while (nextCodeWord >= codeWordThreshold && codeWordThreshold <= maxCodeWords)
	{
		DEBUG_LOG(MODEM, "Compressor: STEPUP");
		pushCode(STEPUP);
		codeWordSize++;
		codeWordThreshold *= 2;
	}
}

void Compressor::flush(bool enterTransparent)
{
	if (!transparent)
	{
		// Output the last state of the string
		if (curCode != -1 && !modeTransition)
		{
			// 7.8.2(a) ensure that the codeword representing any partially encoded data has been transferred
			pushCode(curCode);
			modeTransition = true;
		}
		if (enterTransparent) {
			// 7.8.2(c) indicate to the peer data compression function by transferring the ETM control codeword a
			// transition to transparent mode
			pushCode(ETM);
		}
		else {
			// 7.9(c) if the previous step leaves extra bits pending for transmission (octet alignment not yet achieved), then
			// transfer the FLUSH codeword
			if ((outBitPos % 8) != 0)
				pushCode(FLUSH);
		}
		// 7.8.2(d), 7.9(c) transmit sufficient 0 bits to recover octet alignment
		while (outBitPos > 0)
		{
			push(outBitBuf & 0xff);
			outBitBuf >>= 8;
			outBitPos -= 8;
		}
		outBitPos = 0;
		outBitBuf = 0;
	}
}

void Compressor::serialize(void **data, unsigned int *total_size) const
{
	s32 test = compressTest;
	s32 testCount = compressTestCount;
	u8 bitPos = (u8)outBitPos;

	serializeBase(data, total_size);
	LIBRETRO_S(test);
	LIBRETRO_S(testCount);
	LIBRETRO_S(outBitBuf);
	LIBRETRO_S(bitPos);
	out.serialize(data, total_size);
}

void Compressor::unserialize(void **data, unsigned int *total_size)
{
	s32 test = 0;
	s32 testCount = 64;
	u8 bitPos = 0;

	unserializeBase(data, total_size);
	LIBRETRO_US(test);
	LIBRETRO_US(testCount);
	LIBRETRO_US(outBitBuf);
	LIBRETRO_US(bitPos);
	out.unserialize(data, total_size);
	compressTest = test;
	compressTestCount = testCount;
	// Fewer bits than an octet are waiting between two codewords
	outBitPos = bitPos < 8 ? bitPos : 0;
	outBitBuf &= (1u << outBitPos) - 1;
}

bool Decompressor::write(u8 b, OutStream& out)
{
	if (transparent)
	{
		if (escaped)
		{
			escaped = false;
			switch (b)
			{
			case ECM:
				DEBUG_LOG(MODEM, "Decompressor: ECM");
				transparent = false;
				modeTransition = true;
				return true;
			case EID:
				DEBUG_LOG(MODEM, "Decompressor: EID");
				// the escape code needs to be processed normally
				b = escapeCode;
				escapeCode += 51;
				break;
			case RESET:
				DEBUG_LOG(MODEM, "Decompressor: RESET");
				reset();
				return true;
			default:
				// 5.8(a) receipt of a reserved command code is an error condition
				WARN_LOG(MODEM, "V.42bis: invalid command code %02x", b);
				return false;
			}
		}
		else if (b == escapeCode) {
			escaped = true;
			return true;
		}
		out.write(b);
		acceptChar(b);
		return true;
	}

	inputBitBuffer |= (u32)b << inputBitCount;
	inputBitCount += 8;
	if (inputBitCount < (int)codeWordSize)
		return true;
	u32 newCode = inputBitBuffer & ((1u << codeWordSize) - 1);
	inputBitCount -= codeWordSize;
	inputBitBuffer >>= codeWordSize;

	if (newCode < CW_RESERVED)
	{
		// Control codes
		switch (newCode)
		{
		case ETM:
			DEBUG_LOG(MODEM, "Decompressor: ETM");
			transparent = true;
			modeTransition = true;
			inputBitCount = 0;
			inputBitBuffer = 0;
			break;
		case FLUSH:
			DEBUG_LOG(MODEM, "Decompressor: FLUSH");
			flush();
			break;
		case STEPUP:
			DEBUG_LOG(MODEM, "Decompressor: STEPUP");
			// 5.8(a) receipt of a STEPUP codeword when it would cause the value of C2 to exceed N1
			// (the size of the largest codeword) is an error condition
			if ((1u << codeWordSize) >= maxCodeWords) {
				WARN_LOG(MODEM, "V.42bis: codeword size exceeds the limit");
				return false;
			}
			++codeWordSize;
			codeWordThreshold *= 2;
			break;
		}
		return true;
	}
	modeTransition = false;
	// 5.8(c) receipt of a codeword representing an empty dictionary entry is an error condition
	if (newCode >= maxCodeWords || dict.isFree(newCode)
			// and so is a string to extend that is gone
			|| (curCode != -1 && dict.isFree(curCode)))
	{
		WARN_LOG(MODEM, "V.42bis: unknown code %x", newCode);
		return false;
	}
	// Walk up the tree one character at a time
	u8 decodeBuf[MAX_STRING_SIZE + 1];
	int pos = sizeof(decodeBuf);
	u16 node = newCode;
	while (true)
	{
		decodeBuf[--pos] = dict.getChar(node);
		if (dict.isRoot(node))
			break;
		node = dict.getParent(node);
		if (pos == 0 || node >= maxCodeWords || node < CW_RESERVED)
		{
			WARN_LOG(MODEM, "V.42bis: string too long");
			return false;
		}
	}
	const u8 firstChar = decodeBuf[pos];
	const int len = (int)sizeof(decodeBuf) - pos;
	// Output the decoded string
	for (int i = pos; i < (int)sizeof(decodeBuf); i++)
	{
		out.write(decodeBuf[i]);
		if (decodeBuf[i] == escapeCode)
			escapeCode += 51;
	}
	// 6.4 Add the string to the dictionary
	if (matchLen < maxStringLength
		// 6.4(a) The string does not exceed N7 in length
		&& lastAddedCode != curCode
		&& curCode != -1
		// 6.4(b) The string is not already in the dictionary
		&& dict.getChild(curCode, firstChar) == Dictionary::NONE)
	{
		dict.addChild(nextCodeWord, curCode, firstChar);
		recoverNode();
		lastAddedCode = nextCodeWord;
	}
	else {
		lastAddedCode = -1;
	}
	curCode = newCode;
	matchLen = len;
	return true;
}

void Decompressor::flush()
{
	// 7.9(c) if step a) leaves extra bits pending for transmission (octet alignment not yet achieved),
	// then transfer sufficient 0 bits to recover octet alignment
	inputBitBuffer >>= inputBitCount & 7;
	inputBitCount &= ~7;
}

void Decompressor::reset()
{
	resetBase();
	escaped = false;
	inputBitBuffer = 0;
	inputBitCount = 0;
}

void Decompressor::serialize(void **data, unsigned int *total_size) const
{
	u8 esc = escaped;
	u8 bitCount = (u8)inputBitCount;

	serializeBase(data, total_size);
	LIBRETRO_S(esc);
	LIBRETRO_S(inputBitBuffer);
	LIBRETRO_S(bitCount);
}

void Decompressor::unserialize(void **data, unsigned int *total_size)
{
	u8 esc = 0;
	u8 bitCount = 0;

	unserializeBase(data, total_size);
	LIBRETRO_US(esc);
	LIBRETRO_US(inputBitBuffer);
	LIBRETRO_US(bitCount);
	escaped = esc != 0;
	// Fewer bits than a codeword are waiting
	inputBitCount = bitCount < codeWordSize ? bitCount : 0;
	inputBitBuffer &= (1u << inputBitCount) - 1;
}

}	// namespace v42b
}	// namespace modem
