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

static void _flush(struct GBSIOSocket* sock);
void _flush(struct GBSIOSocket* sock) {
	uint8_t buffer[32];
	while (SocketRecv(sock->clock, buffer, sizeof(buffer)) == sizeof(buffer));
	while (SocketRecv(sock->data, buffer, sizeof(buffer)) == sizeof(buffer));
}

bool GBSIOSocketInit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;

	sock->event.context = sock;
	sock->event.name = "GB SIO TCPLINK";
	sock->event.callback = _GBSIOSocketProcessEvents;
	sock->event.priority = 0x80;

	mTimingSchedule(&driver->p->p->timing, &sock->event, 0);
	return true;
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
	m_serverMode = server;

	struct Address serverIP = {
		.version = IPV4,
		.ipv4 = 0x7F000001
	};

	SocketSubsystemInit();
	if (m_serverMode) {
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
			sock->data = SocketAccept(sock->server_data, NULL);
			sock->clock = SocketAccept(sock->server_clock, NULL);
		}
		mLOG(GB_SIO, DEBUG, "Connection established.");
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		sock->data = SocketConnectTCP(27500, &serverIP);
		sock->clock = SocketConnectTCP(27501, &serverIP);
	}

	SocketSetBlocking(sock->data, false);
	SocketSetBlocking(sock->clock, false);
	SocketSetTCPPush(sock->data, true);

}

static void _GBSIOSocketProcessEvents(struct mTiming* timing, void* user, uint32_t cyclesLate) {
	struct GBSIOSocket* node = user;

	// Check for clock, if seen go into transfer mode to receive updates
	if (!m_serverMode && node->transferActive == TRANSFER_IDLE) {
		uint8_t buffer[32];
		Socket r = node->clock;

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

			if (m_serverMode) {
				// Send our data
				SocketSend(node->data, &node->pendingSB, sizeof(uint8_t));

				Socket r = node->data;
				SocketRecv(node->data, &node->pendingSB, sizeof(uint8_t));
				mLOG(GB_SIO, DEBUG, "Server synced SB: %i", node->pendingSB);
			} else {
				// TODO: Doublechecking references here on size
				uint8_t copy = 0;
				Socket r = node->data;
				SocketRecv(node->data, &copy, sizeof(uint8_t));
				SocketSend(node->data, &node->pendingSB, sizeof(uint8_t));
				node->pendingSB = copy;
				mLOG(GB_SIO, DEBUG, "Client synced SB: %i", node->pendingSB);
			}

			mTimingSchedule(timing, &node->event, 0);
			break;
		case TRANSFER_FINISHED:
			// Copy over SB data to live SIO register
			mLOG(GB_SIO, DEBUG, "TCPLINK Finished");
			node->d.p->pendingSB = node->pendingSB;
			if (GBRegisterSCIsEnable(node->d.p->p->memory.io[GB_REG_SC])) {
				node->d.p->remainingBits = 8;
			}

			// Fallthru
		default:
			node->transferActive = TRANSFER_IDLE;
			mTimingSchedule(timing, &node->event, LOCKSTEP_INCREMENT);
	}
}

static void GBSIOSocketWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;
	node->pendingSB = value;
}

static uint8_t GBSIOSocketWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;

	// Only server gets to be master in GB land
	if ((value & 0x81) == 0x81 && m_serverMode) {
		SocketSend(node->clock, "HELO", sizeof("HELO"));
		node->transferActive = TRANSFER_STARTING;
		node->transferCycles = GBSIOCyclesPerTransfer[(value >> 1) & 1];
		mTimingDeschedule(&driver->p->p->timing, &driver->p->event);
		mTimingDeschedule(&driver->p->p->timing, &node->event);
		mTimingSchedule(&driver->p->p->timing, &node->event, 0);
	}

	return value;
}
