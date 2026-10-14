/*
	Copyright 2022 flyinghead

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
#include <string>

namespace card_reader {

void initdInit();
void derbyInit();
void clubkInit();
void term();

void barcodeInit();
void barcodeTerm();
bool barcodeAvailable();
std::string barcodeGetCard();
void barcodeSetCard(const std::string& card);

void insertCard(int playerNum);
bool readerAvailable();

// What the readers have in a save state since V24 (card_reader.cpp)
void serialize_v24(void **data, unsigned int *total_size);
bool unserialize_v24(void **data, unsigned int *total_size);
void state_before_v24();

}

// The cards out of the readers in a save state from V30 on (card_reader.cpp)
void naomi_cards_serialize_v30(void **data, unsigned int *total_size);
bool naomi_cards_unserialize_v30(void **data, unsigned int *total_size);
void naomi_cards_state_before_v30(void);
