/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/tcp_lockstep.h>

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>

#define LOCKSTEP_INCREMENT 512

static bool GBSIOLockstepNodeInit(struct GBSIODriver* driver);
static void GBSIOLockstepNodeDeinit(struct GBSIODriver* driver);
static void GBSIOLockstepNodeWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOLockstepNodeWriteSC(struct GBSIODriver* driver, uint8_t value);
static void _GBSIOLockstepNodeProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate);

void GBSIOLockstepInit(struct GBSIOLockstep* lockstep, bool server) {
	lockstep->player = NULL;
	lockstep->pendingSB = 0xFF;
	lockstep->masterClaimed = false;
	lockstep->server = server;

	struct Address serverIP = {
		.version = IPV4,
		.ipv4 = 0x7F000001
	};

	SocketSubsystemInit();
	if (server) {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK server mode");
		lockstep->server_data = SocketOpenTCP(27500, NULL);
		lockstep->server_clock = SocketOpenTCP(27501, NULL);
		SocketListen(lockstep->server_data, 1);
		SocketListen(lockstep->server_clock, 1);


		mLOG(GB_SIO, DEBUG, "Sockets opened, awaiting connection...");
		mLOG(GB_SIO, DEBUG, "Data: %i Clock: %i", lockstep->server_data, lockstep->server_clock);

		lockstep->data = -1;
		lockstep->clock = -1;

		while (lockstep->data == -1) {
			//mLOG(GB_SIO, DEBUG, "Awaiting...");
			lockstep->data = SocketAccept(lockstep->server_data, NULL);
			lockstep->clock = SocketAccept(lockstep->server_clock, NULL);
		}
		mLOG(GB_SIO, DEBUG, "Connection established.");
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		lockstep->data = SocketConnectTCP(27500, &serverIP);
		lockstep->clock = SocketConnectTCP(27501, &serverIP);
	}
}

void GBSIOLockstepNodeCreate(struct GBSIOLockstepNode* node) {
	node->d.init = GBSIOLockstepNodeInit;
	node->d.deinit = GBSIOLockstepNodeDeinit;
	node->d.writeSB = GBSIOLockstepNodeWriteSB;
	node->d.writeSC = GBSIOLockstepNodeWriteSC;
}

bool GBSIOLockstepNodeInit(struct GBSIODriver* driver) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;
	mLOG(GB_SIO, DEBUG, "Lockstep %i: Node init", node->id);
	node->event.context = node;
	node->event.name = "GB TCP SIO Lockstep";
	node->event.callback = _GBSIOLockstepNodeProcessEvents;
	node->event.priority = 0x80;

	node->nextEvent = 0;
	node->eventDiff = 0;
	mTimingSchedule(&driver->p->p->timing, &node->event, 0);
#ifndef NDEBUG
	node->phase = node->p->d.transferActive;
	node->transferId = node->p->d.transferId;
#endif
	return true;
}

void GBSIOLockstepNodeDeinit(struct GBSIODriver* driver) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;
	node->p->d.unload(&node->p->d, node->id);
	mTimingDeschedule(&driver->p->p->timing, &node->event);
	SocketSubsystemDeinit();
}

static void _GBSIOLockstepNodeProcessEvents(struct mTiming* timing, void* user, uint32_t cyclesLate) {
	struct GBSIOLockstepNode* node = user;

	if (!node->p->server) {
		Socket r = node->p->clock;
		// Clock data only means to initiate a transfer, get ready to send over our pending bits!
		if (SocketPoll(1, &r, 0, 0, 32)) {
			node->p->d.transferActive = TRANSFER_STARTING;
			uint8_t buffer[32];
			// Flush the buffer
			while (SocketRecv(node->p->clock, buffer, sizeof(buffer)) == sizeof(buffer));
		}
	}

	switch (node->p->d.transferActive) {
		case TRANSFER_IDLE:
			mTimingSchedule(timing, &node->event, LOCKSTEP_INCREMENT);
		case TRANSFER_STARTING:
			node->p->d.transferActive = TRANSFER_FINISHED;
			mTimingSchedule(timing, &node->event, 8);

			if (node->p->server) {
				// Send our data
				SocketSend(node->p->data, &node->p->pendingSB, sizeof(node->p->pendingSB));
				// Overwrite pending buffer to be updated
				SocketRecv(node->p->data, &node->p->pendingSB, sizeof(node->p->pendingSB));
			} else {
				// TODO: Doublechecking references here on size
				uint8_t copy;
				SocketRecv(node->p->data, &copy, sizeof(copy));
				SocketSend(node->p->data, &node->p->pendingSB, sizeof(node->p->pendingSB));
				node->p->pendingSB = copy;
			}
			break;
		case TRANSFER_FINISHED:
			// Copy over SB data to live SIO register
			node->d.p->pendingSB = node->p->pendingSB;
			if (GBRegisterSCIsEnable(node->d.p->p->memory.io[GB_REG_SC])) {
				node->d.p->remainingBits = 8;
				mTimingDeschedule(timing, &node->event);
			}
			// Fallthru
		default:
			mTimingSchedule(timing, &node->event, 0);
	}
}

static void GBSIOLockstepNodeWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;
	node->p->pendingSB = value;
}

static uint8_t GBSIOLockstepNodeWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOLockstepNode* node = (struct GBSIOLockstepNode*) driver;

	// Only server gets to be master in GB land
	if ((value & 0x81) == 0x81 && node->p->server) {
		SocketSend(node->p->clock, "HELO", sizeof("HELO"));
		node->p->d.transferActive = TRANSFER_STARTING;
		node->p->d.transferCycles = GBSIOCyclesPerTransfer[(value >> 1) & 1];
		mTimingDeschedule(&driver->p->p->timing, &driver->p->event);
		mTimingDeschedule(&driver->p->p->timing, &node->event);
		mTimingSchedule(&driver->p->p->timing, &node->event, 0);
	}
	return value;
}
