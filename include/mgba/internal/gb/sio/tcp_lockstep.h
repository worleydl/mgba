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

struct GBSIOSocket {
	struct GBSIODriver d;
	struct mTimingEvent event;

	int32_t transferCycles;
	enum mLockstepPhase transferActive;


	Socket data;
	Socket clock;

	Socket server_data;
	Socket server_clock;

	uint8_t pendingSB;
	bool wantClock;
	bool receivedClock;
};

void GBSIOSocketConnect(struct GBSIOSocket*, bool server);
void GBSIOSocketCreate(struct GBSIOSocket*);

static bool m_serverMode = false;


CXX_GUARD_END

#endif
