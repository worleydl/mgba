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
static void _finishTransfer(struct GBSIOSocket*, uint8_t);

bool GBSIOSocketInit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;

	sock->event.context = sock;
    sock->event.name = "GB SIO TCPLINK";
    sock->event.callback = _GBSIOSocketProcessEvents;
    sock->event.priority = 0x80;

	sock->state = XFER_IDLE;

	return true;
}

void GBSIOSocketDeinit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;
}

void GBSIOSocketCreate(struct GBSIOSocket* sock) {
	sock->d.init = GBSIOSocketInit;
	sock->d.deinit = GBSIOSocketDeinit;
	sock->d.writeSB = GBSIOSocketWriteSB;
	sock->d.writeSC = GBSIOSocketWriteSC;
	sock->transferActive = 0;
	sock->processing = false;

	sock->clockRequest[0] = CLOCK_REQUEST;
	sock->clockResponse[0] = CLOCK_RESPONSE;
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
		mLOG(GB_SIO, DEBUG, "Data: %i", sock->server_data);

		sock->clock = -1;
		sock->data = -1;

		while (sock->data == -1) {
			sock->data = SocketAccept(sock->server_data, NULL);
		}

		while (sock->clock == -1) {
			sock->clock = SocketAccept(sock->server_clock, NULL);
		}
		mLOG(GB_SIO, DEBUG, "Connection established.");
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		sock->data = SocketConnectTCP(27500, &serverIP);
		sock->clock = SocketConnectTCP(27501, &serverIP);
	}


	// TODO: Move this to socket.h if it works
	DWORD timeout = 500;
	setsockopt(sock->data, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

	SocketSetBlocking(sock->clock, false);
	SocketSetBlocking(sock->data, true);
	SocketSetTCPPush(sock->clock, true);
	SocketSetTCPPush(sock->data, true);

}

static void _GBSIOSocketProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate) {
	struct GBSIOSocket* sock = driver;
	sock->d.p->p->memory.io[GB_REG_SB] = sock->d.p->pendingSB;
	sock->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCClearEnable(sock->d.p->p->memory.io[GB_REG_SC]);
	sock->d.p->p->memory.io[GB_REG_IF] |= (1 << GB_IRQ_SIO);
	GBUpdateIRQs(sock->d.p->p);
	sock->d.p->pendingSB = 0xFF;
	sock->processing = false;
}

static void _finishTransfer(struct GBSIOSocket* sock, uint8_t update) {
	// Copy over SB data to live SIO register
	sock->d.p->pendingSB = update;

	if (GBRegisterSCIsEnable(sock->d.p->p->memory.io[GB_REG_SC])) {
		mTimingDeschedule(&sock->d.p->p->timing, &sock->event);
		mTimingSchedule(&sock->d.p->p->timing, &sock->event, (sock->d.p->period * (2 - sock->d.p->p->doubleSpeed)) * 8);
		sock->processing = true;
		sock->waitCycles = 1; // Experiment to wait for gameboy to process data before allowing more comms
	}
}


void GBSIOSocketSync(struct GBSIOSocket* node) {
	if (node->processing) {
		return;
	}

	uint8_t buffer[2];
	if (SocketRecv(node->clock, &buffer, sizeof(buffer)) == sizeof(buffer)){
		if (buffer[0] == CLOCK_REQUEST) {
			node->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCFillEnable(node->d.p->p->memory.io[GB_REG_SC]);
			node->clockResponse[1] = node->pendingSB;
			SocketSend(node->data, &node->clockResponse, sizeof(node->clockResponse));
		}

		_finishTransfer(node, buffer[1]);
	}
}

static void GBSIOSocketWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;
	node->pendingSB = value;
}

static uint8_t GBSIOSocketWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOSocket* node = (struct GBSIOSocket*) driver;
	node->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCFillEnable(node->d.p->p->memory.io[GB_REG_SC]);
	// We want to send some data
	if ((value & 0x81) == 0x81) {
		node->clockRequest[1] = node->pendingSB;

		// Shots fired
		SocketSend(node->clock, &node->clockRequest, sizeof(node->clockRequest));
		uint8_t buffer[2];

		// Bad things will happen if the timeout is exceeded, but if the devices stay within
		// things should work.  This effectively locks the emulation until devices sync
		// their SB buffers and execution can resume on both.
		if (SocketRecv(node->data, &buffer, sizeof(buffer)) == sizeof(buffer)){
			_finishTransfer(node, buffer[1]);
			mTimingDeschedule(&node->d.p->p->timing, &node->d.p->event);
		}
	}

	return value;
}
