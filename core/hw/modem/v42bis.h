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
#pragma once
#include "types.h"
#include "internal.h"

/* V.42 bis data compression. The dictionary is a tree kept in arrays,
 * so that it has a fixed size and can be in a save state. */
namespace modem
{
namespace v42b
{
// N3 Character size (bits)
static const u32 CHAR_SIZE = 8;
// N4 Number of characters in the alphabet
static const u32 ALPHABET_SIZE = 1 << CHAR_SIZE;
// N6 Number of control codewords
static const u32 CW_RESERVED = 3;
// N5 Index number of first dictionary entry used to store a string
static const u32 CW_FIRST = ALPHABET_SIZE + CW_RESERVED;
// 5.1 Minimum and default value for P1 (Total number of codewords)
static const u32 MIN_TOTAL_CODEWORDS = 512;
/* The most this modem accepts for P1. The other end may ask for more (the
 * standard allows 65535); it is then answered with this, which the
 * negotiation lets it do. */
static const u32 MAX_TOTAL_CODEWORDS = 2048;
// 5.1 Valid range for P2 (Maximum string length)
static const int DEF_STRING_SIZE = 6;
static const int MAX_STRING_SIZE = 250;

class Dictionary
{
public:
	static const u16 NONE = 0xffff;		// no node; a parent: the node is free
	static const u16 ROOT = 0xfffe;		// a parent: the node is a single character

	void reset();

	bool isFree(u16 code) const {
		return parent[code] == NONE;
	}
	bool isRoot(u16 code) const {
		return parent[code] == ROOT;
	}
	bool isLeaf(u16 code) const {
		return firstChild[code] == NONE;
	}
	u16 getParent(u16 code) const {
		return parent[code];
	}
	u8 getChar(u16 code) const {
		return character[code];
	}
	// The child of @code for @c, NONE if it has none
	u16 getChild(u16 code, u8 c) const;
	void addChild(u16 childCode, u16 parentCode, u8 c);
	// Takes a leaf off its parent: the node is free again
	void detach(u16 code);

	void serialize(void **data, unsigned int *total_size) const;
	// @limit: P1, the number of codewords in use
	void unserialize(void **data, unsigned int *total_size, u32 limit);

private:
	u16 parent[MAX_TOTAL_CODEWORDS];
	u16 firstChild[MAX_TOTAL_CODEWORDS];
	u16 nextSibling[MAX_TOTAL_CODEWORDS];
	u8 character[MAX_TOTAL_CODEWORDS];
};

class V42Base
{
public:
	// @maxCodeWords and @maxStringLength are the negotiated P1 and P2
	void setParams(u32 maxCodeWords, int maxStringLength);

protected:
	V42Base() {
		setParams(MIN_TOTAL_CODEWORDS, DEF_STRING_SIZE);
	}

	void resetBase();
	void recoverNode();
	int acceptChar(u8 octet);
	virtual void checkDictSize() {}

	void serializeBase(void **data, unsigned int *total_size) const;
	void unserializeBase(void **data, unsigned int *total_size);

	u32 maxCodeWords;			// V.42 bis param N2
	int maxStringLength;		// V.42 bis param N7
	u32 nextCodeWord;			// V.42 bis param C1
	u32 codeWordSize;			// V.42 bis param C2
	u32 codeWordThreshold;		// V.42 bis param C3
	Dictionary dict;
	int lastAddedCode;
	int matchLen;
	int curCode;
	bool transparent;
	// This flag indicates that the current codeword has already been transfered
	// and that string matching should restart (but dictionary update should proceed as normal)
	// Used for mode transitions, and flush in compressed mode
	bool modeTransition;
	u8 escapeCode;
};

class Compressor : public V42Base
{
public:
	Compressor() {
		reset();
	}

	void write(u8 v);
	void changeMode();
	void reset();

	void flush() {
		flush(false);
	}
	int read() {
		return out.pop();
	}
	int available() const {
		return out.size();
	}

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	void checkDictSize() override;
	void flush(bool enterTransparent);
	void pushCode(u16 code);
	void push(u8 b) {
		out.push(b);
	}

	int compressTest;
	int compressTestCount;
	u32 outBitBuf;
	int outBitPos;
	ByteFifo<512> out;
};

class Decompressor : public V42Base
{
public:
	Decompressor() {
		reset();
	}

	/* What is decoded goes to @out as it comes. False if the data is in
	 * error (V.42 bis 5.8): the connection is to be released. */
	bool write(u8 v, OutStream& out);
	void reset();

	void serialize(void **data, unsigned int *total_size) const;
	void unserialize(void **data, unsigned int *total_size);

private:
	void flush();

	bool escaped;
	u32 inputBitBuffer;
	int inputBitCount;
};

}	// namespace v42b
}	// namespace modem
