/* Copyright (c) 2013-2016 Jeffrey Pfau
 *				 2022 Daniel Worley
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/tcp_lockstep.h>

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>

/***
 * WIP TCP SIO driver
 *
 * Currently works with most gameboy games and recently gbc seems to be working.
 *
 * Features:
 * - Pseudo-lockstep for locking networked cores
 * - Auto-discovery to determine host/client mode, only designed for max of 2 players on a network
 *
 * Known issues:
 * - Slow start to 1989 Tetris match, timing issue?
 * - No auto-detection of broadcast address, requires 192.168.1.255 broadcast to work (default on a lot of routers)
 **/

static bool GBSIOSocketInit(struct GBSIODriver* driver);
static void GBSIOSocketDeinit(struct GBSIODriver* driver);
static void GBSIOSocketWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOSocketWriteSC(struct GBSIODriver* driver, uint8_t value);
static void _GBSIOSocketProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate);
static void _finishTransfer(struct GBSIOSocket*, uint8_t);
static bool _checkBroadcasts(struct GBSIOSocket*);
static void _setSockTimeout(Socket, uint32_t);
static uint32_t _recvfrom(Socket s);

static bool _checkBroadcasts(struct GBSIOSocket* sock) {
	sock->broadcast = SocketOpenUDP(27502, NULL);
	SocketSetBlocking(sock->broadcast, true);

	_setSockTimeout(sock->broadcast, 3000);


	mLOG(GB_SIO, DEBUG, "Checking for broadcast");
	uint32_t addy = _recvfrom(sock->broadcast);
	if (addy > 0) {
		sock->serverIP.ipv4 = htonl(addy);
		return false;
	}

	return true;
}

static uint32_t _recvfrom(Socket s) {
	struct sockaddr_in from;
	int fromSize = sizeof(from);
	uint8_t buffer;

	#ifdef _WIN32

	if(recvfrom(s, &buffer, sizeof(buffer), 0, (SOCKADDR *)&from, &fromSize) > 0) {
		return from.sin_addr.s_addr;

	} else {
		return 0;
	}

	#else

	if(recvfrom(s, &buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &fromSize) > 0) {
		return from.sin_addr.s_addr;

	} else {
		return 0;
	}

	#endif
}

static void _setSockTimeout(Socket s, uint32_t timeoutVal) {
	#ifdef _WIN32
	DWORD timeout = timeoutVal;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	#else
	struct timeval tv;
	tv.tv_sec = timeoutVal / 1000;
	tv.tv_usec = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
	#endif
}

bool GBSIOSocketInit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;

	sock->event.context = sock;
    sock->event.name = "GB SIO TCPLINK";
    sock->event.callback = _GBSIOSocketProcessEvents;
    sock->event.priority = 0x80;

	return true;
}

void GBSIOSocketDeinit(struct GBSIODriver* driver) {
	struct GBSIOSocket* sock = (struct GBSIOSocket*) driver;

	SocketClose(sock->clock);
	SocketClose(sock->data);

	if (m_serverMode) {
		SocketClose(sock->server_clock);
		SocketClose(sock->server_data);
	}

	// broadcast closed during connect
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


	memset(&sock->serverIP, 0, sizeof(sock->serverIP));
	sock->serverIP.version = IPV4;
	sock->serverIP.ipv4 = 0x7F000001;

	SocketSubsystemInit();
	m_serverMode = _checkBroadcasts(sock);


	if (m_serverMode) {

		mLOG(GB_SIO, DEBUG, "Running TCPLINK server mode");
		sock->server_data = SocketOpenTCP(27500, NULL);
		sock->server_clock = SocketOpenTCP(27501, NULL);
		SocketListen(sock->server_data, 1);
		SocketListen(sock->server_clock, 1);

		mLOG(GB_SIO, DEBUG, "Sockets opened, awaiting connection...");
		mLOG(GB_SIO, DEBUG, "Server Data: %i", sock->server_data);

		sock->clock = INVALID_SOCKET;
		sock->data = INVALID_SOCKET;


		// Setup broadcast mode on broadcast socket
		uint8_t broadcastEnable = 1;
		setsockopt(sock->broadcast, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

		struct sockaddr_in broadcastAddr;
		memset(&broadcastAddr, 0, sizeof(broadcastAddr));
		broadcastAddr.sin_family = AF_INET;
		// TODO: Get the broadcast address from the current network settings
		broadcastAddr.sin_addr.s_addr = inet_addr("192.168.1.255");
		broadcastAddr.sin_port = htons(27502);

		while (sock->data == INVALID_SOCKET) {
			sendto(sock->broadcast, &broadcastEnable, sizeof(broadcastEnable), 0, (struct sockaddr *) &broadcastAddr, sizeof(broadcastAddr));

			struct timeval tv;
			int timeoutMillis = 250;
			tv.tv_sec = timeoutMillis / 1000;
			tv.tv_usec = (timeoutMillis % 1000) * 1000;
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(sock->server_data, &fds);
			select(sock->server_data + 1, &fds, NULL, NULL, &tv);

			if (FD_ISSET(sock->server_data, &fds)) {
				sock->data = SocketAccept(sock->server_data, NULL);
			}
		}


		while (sock->clock == INVALID_SOCKET) {
			sock->clock = SocketAccept(sock->server_clock, NULL);
		}
		mLOG(GB_SIO, DEBUG, "Connection established.");
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		sock->data = SocketConnectTCP(27500, &sock->serverIP);
		sock->clock = SocketConnectTCP(27501, &sock->serverIP);
	}


	mLOG(GB_SIO, DEBUG, "Data: %i", sock->data);
	mLOG(GB_SIO, DEBUG, "Clock: %i", sock->clock);

	_setSockTimeout(sock->data, 500);

	SocketSetBlocking(sock->clock, false);
	SocketSetBlocking(sock->data, true);
	SocketSetTCPPush(sock->clock, true);
	SocketSetTCPPush(sock->data, true);

	// Note: SocketClose caused a crash here, and if you leave this socket
	// open for some reason the main data sockets can no long stay in sync
	close(sock->broadcast);
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
		} else {
			_finishTransfer(node, 0XFF);
		}

		mTimingDeschedule(&node->d.p->p->timing, &node->d.p->event);
	}

	return value;
}
