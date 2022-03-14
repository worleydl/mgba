/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_TCP_SIO_LOCKSTEP_H
#define GB_TCP_SIO_LOCKSTEP_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/core/lockstep.h>
#include <mgba/core/timing.h>
#include <mgba/internal/gb/sio.h>
#include <mgba-util/socket.h>
#include <mgba/gb/interface.h>
#include <mgba-util/threading.h>

enum SerialXfer {
	CLOCK_RESPONSE = 0,
	CLOCK_REQUEST = 1
};

enum TransferState {
	XFER_IDLE = 0,
	XFER_STARTED,
	XFER_FINISHED,
};


struct GBSIOSocket {
	struct GBSIODriver d;

	struct Address serverIP;

	bool processing;
	enum mLockstepPhase transferActive;
	struct mTimingEvent event;

	Socket broadcast;

	Socket clock;
	Socket server_clock;

	Socket data;
	Socket server_data;

	uint8_t pendingSB;
	uint8_t incomingSB;
	uint16_t waitCycles;

	int8_t clockResponse[2];
	int8_t clockRequest[2];
};

void GBSIOSocketConnect(struct GBSIOSocket*, bool server);
void GBSIOSocketCreate(struct GBSIOSocket*);
void GBSIOSocketSync(struct GBSIOSocket*);
bool GBSIOSocketBusy(struct GBSIOSocket*);

static bool m_serverMode = false;


CXX_GUARD_END

#endif
