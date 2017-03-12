/*
 * rf433-ook - Arduino 433MHz OOK Receiver/Transmitter
 * Copyright 2017  Simon Arlott
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Interrupt handler protocol decode derived from:
 * RemoteSwitch library v2.0.0 made by Randy Simons http://randysimons.nl
 *
 * Copyright 2010 Randy Simons. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list
 *       of conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY RANDY SIMONS ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RANDY SIMONS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those of the
 * authors and should not be interpreted as representing official policies, either expressed
 * or implied, of Randy Simons.
 */

#include <stdint.h>

#include "Receiver.hpp"

Receiver receiver;

Receiver::Receiver() {

}

Receiver::~Receiver() {

}

void Receiver::attach(int pin) {
	attachInterrupt(digitalPinToInterrupt(pin), interruptHandler, CHANGE);
}

static void addBit(char *code, uint_fast8_t &codeLength, int_fast8_t &currentBit, uint_fast8_t &value, uint_fast8_t bit) {
	value |= bit << currentBit;

	if (currentBit == 0) {
		code[codeLength++] = (value < 10) ? (char)('0' + value) : (char)('A' + (value - 10));

		currentBit = 3;
		value = 0;
	} else {
		currentBit--;
	}
}

void Receiver::interruptHandler() {
	static unsigned long last = 0;
	static bool sync = false;
	static unsigned long minZeroPeriod, maxZeroPeriod;
	static unsigned long minOnePeriod, maxOnePeriod;
	static unsigned long minSyncPeriod, maxSyncPeriod;
	static char code[Code::MAX_LENGTH + 1];
	static uint_fast8_t codeLength;
	static unsigned long start, stop;
	static bool preSyncStandalone = true;
	static unsigned long preSyncPeriod, postSyncPeriod;
	static unsigned long zeroBitPeriod, oneBitPeriod, allBitPeriod;
	static unsigned int zeroBitCount, oneBitCount, allBitCount;
	static int_fast8_t currentBit;
	static uint_fast8_t value;
	unsigned long now = micros();
	unsigned long duration = now - last;

retry:
	if (!sync) {
		if (duration >= SYNC_CYCLES * MIN_PERIOD_US) {
			unsigned long period = duration / SYNC_CYCLES;

			start = last;
			codeLength = 0;
			preSyncPeriod = period;
			zeroBitPeriod = oneBitPeriod = allBitPeriod = 0;
			zeroBitCount = oneBitCount = allBitCount = 0;
			currentBit = 3;
			value = 0;
			sync = true;

			// 1 period = 0-bit
			minZeroPeriod = period * 4 / 10;
			maxZeroPeriod = period * 16 / 10;

			// 3 period = 1-bit
			minOnePeriod = period * 23 / 10;
			maxOnePeriod = period * 37 / 10;

			minSyncPeriod = period * (SYNC_CYCLES - 6);
			maxSyncPeriod = period * (SYNC_CYCLES + 4);
		}
	} else {
		bool postSyncPresent = false;

		if (duration >= minSyncPeriod && duration <= maxSyncPeriod) {
			postSyncPeriod = duration / SYNC_CYCLES;
			postSyncPresent = true;
		} else {
			if (codeLength == sizeof(code) - 1) {
				// Code too long
			} else if (duration >= minZeroPeriod && duration <= maxZeroPeriod) {
				addBit(code, codeLength, currentBit, value, 0);
				zeroBitPeriod += duration;
				zeroBitCount++;
				allBitPeriod += duration;
				allBitCount++;
				goto done;
			} else if (duration >= minOnePeriod && duration <= maxOnePeriod) {
				addBit(code, codeLength, currentBit, value, 1);
				oneBitPeriod += duration / 3;
				oneBitCount++;
				allBitPeriod += duration / 3;
				allBitCount++;
				goto done;
			} else {
				// Invalid duration
			}
		}

		if (codeLength >= Code::MIN_LENGTH) {
			stop = now;

			if (zeroBitCount > 0) {
				zeroBitPeriod /= zeroBitCount;
			}

			if (oneBitCount > 0) {
				oneBitPeriod /= oneBitCount;
			}

			if (allBitCount > 0) {
				allBitPeriod /= allBitCount;
			}

			code[codeLength] = 0;
			addCode(Code(code, 3 - currentBit, value, stop - start,
				preSyncStandalone, postSyncPresent, preSyncPeriod, postSyncPeriod,
				zeroBitPeriod, oneBitPeriod, allBitPeriod));
		} else {
			// Code too short
		}

		// Restart, reusing the current sync duration
		sync = false;
		preSyncStandalone = !postSyncPresent;
		goto retry;
	}

done:
	last = now;
}

void Receiver::addCode(const Code &code) {
	receiver.codes[receiver.codeIndex] = code;
	receiver.codeIndex = (receiver.codeIndex + 1) % MAX_CODES;
}

void Receiver::printCode() {
	noInterrupts();

	for (uint_fast16_t n = 0; n < MAX_CODES; n++) {
		// This won't work if `codeIndex + n` wraps before `% MAX_CODES` is applied
		uint_fast8_t i = ((uint_fast16_t)codeIndex + n) % MAX_CODES;

		if (!codes[i].empty()) {
			Code code = codes[i];
			codes[i].clear();
			interrupts();

			SerialUSB.println(code);
			return;
		}
	}

	interrupts();
}
