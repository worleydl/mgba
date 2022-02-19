/* Copyright (c) 2013-2016 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/tcp_lockstep.h>

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>

#define LOCKSTEP_INCREMENT 512

static bool GBSIOSocketInit(struct GBSIODriver* driver);
static void GBSIOSocketDeinit(struct GBSIODriver* driver);
static void GBSIOSocketWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOSocketWriteSC(struct GBSIODriver* driver, uint8_t value);
static void _GBSIOSocketProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate);

bool GBSIOSocketInit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;

	sock->event.context = sock;
	sock->event.name = "GB SIO TCPLINK";
	sock->event.callback = _GBSIOSocketProcessEvents;
	sock->event.priority = 0x80;

	mTimingSchedule(&driver->p->p->timing, &sock->event, 0);
}

void GBSIOSocketDeinit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;
	mTimingDeschedule(&driver->p->p->timing, &sock->event);
}

void GBSIOSocketCreate(struct GBSIOSocket* sock) {
	sock->d.init = GBSIOSocketInit;
	sock->d.deinit = GBSIOSocketDeinit;
	sock->d.writeSB = GBSIOSocketWriteSB;
	sock->d.writeSC = GBSIOSocketWriteSC;
	sock->transferActive = 0;
}

void GBSIOSocketConnect(struct GBSIOSocket* sock, bool server) {
	sock->pendingSB = 0xFF;
	sock->server = server;

	struct Address serverIP = {
		.version = IPV4,
		.ipv4 = 0x7F000001
	};

	SocketSubsystemInit();
	if (server) {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK server mode");
		sock->server_data = SocketOpenTCP(27500, NULL);
		sock->server_clock = SocketOpenTCP(27501, NULL);
		SocketListen(sock->server_data, 1);
		SocketListen(sock->server_clock, 1);


		mLOG(GB_SIO, DEBUG, "Sockets opened, awaiting connection...");
		mLOG(GB_SIO, DEBUG, "Data: %i Clock: %i", sock->server_data, sock->server_clock);

		sock->data = -1;
		sock->clock = -1;

		while (sock->data == -1) {
			//mLOG(GB_SIO, DEBUG, "Awaiting...");
			sock->data = SocketAccept(sock->server_data, NULL);
			sock->clock = SocketAccept(sock->server_clock, NULL);
		}
		mLOG(GB_SIO, DEBUG, "Connection established.");
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		sock->data = SocketConnectTCP(27500, &serverIP);
		sock->clock = SocketConnectTCP(27501, &serverIP);

		SocketSetBlocking(sock->data, false);
		SocketSetBlocking(sock->clock, false);
		SocketSetTCPPush(sock->data, true);
	}
}

static void _GBSIOSocketProcessEvents(struct mTiming* timing, void* user, uint32_t cyclesLate) {
	struct GBSIOSocket* node = user;

	if (!node->server) {
		uint8_t buffer[32];
		Socket r = node->clock;
		SocketPoll(1, &r, 0, 0, 500);
		if (SocketRecv(node->clock, buffer, sizeof("HELO")) == sizeof("HELO")) {
			node->transferActive = TRANSFER_STARTING;
		}
	}

	switch (node->transferActive) {
		case TRANSFER_IDLE:
			mTimingSchedule(timing, &node->event, LOCKSTEP_INCREMENT);
			break;
		case TRANSFER_STARTING:
			mLOG(GB_SIO, DEBUG, "TCPLINK STARTING");
			node->transferActive = TRANSFER_FINISHED;
			mTimingSchedule(timing, &node->event, 8);

			if (node->server) {
				// Send our data
				SocketSend(node->data, &node->pendingSB, sizeof(node->pendingSB));
				// Overwrite pending buffer to be updated
				SocketRecv(node->data, &node->pendingSB, sizeof(node->pendingSB));
			} else {
				// TODO: Doublechecking references here on size
				uint8_t copy;
				SocketRecv(node->data, &copy, sizeof(copy));
				SocketSend(node->data, &node->pendingSB, sizeof(node->pendingSB));
				node->pendingSB = copy;
			}
			break;
		case TRANSFER_FINISHED:
			// Copy over SB data to live SIO register
			mLOG(GB_SIO, DEBUG, "TCPLINK Finished");
			node->d.p->pendingSB = node->pendingSB;
			if (GBRegisterSCIsEnable(node->d.p->p->memory.io[GB_REG_SC])) {
				node->d.p->remainingBits = 8;
				mTimingDeschedule(timing, &node->event);
			}
			// Fallthru
		default:
			node->transferActive = TRANSFER_IDLE;
			mTimingSchedule(timing, &node->event, 0);
	}
}

static void GBSIOSocketWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;
	node->pendingSB = value;
}

static uint8_t GBSIOSocketWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;

	// Only server gets to be master in GB land
	if ((value & 0x81) == 0x81 && node->server) {
		SocketSend(node->clock, "HELO", sizeof("HELO"));
		node->transferActive = TRANSFER_STARTING;
		node->transferCycles = GBSIOCyclesPerTransfer[(value >> 1) & 1];
		mTimingDeschedule(&driver->p->p->timing, &driver->p->event);
		mTimingDeschedule(&driver->p->p->timing, &node->event);
		mTimingSchedule(&driver->p->p->timing, &node->event, 0);
	}

	return value;
}
