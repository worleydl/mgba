/* Copyright (c) 2013-2016 Jeffrey Pfau
 *				 2022 Daniel Worley
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gb/sio/udp_sio.h>

#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/io.h>
#include <mgba/internal/sm83/sm83.h>

/***
 * TCP showed promise locally but it's not feasible on real devices with the overhead associated.
 *
 * This is an attempt with UDP
 */
static bool GBSIOUDPInit(struct GBSIODriver* driver);
static void GBSIOUDPDeinit(struct GBSIODriver* driver);
static void GBSIOUDPWriteSB(struct GBSIODriver* driver, uint8_t value);
static uint8_t GBSIOUDPWriteSC(struct GBSIODriver* driver, uint8_t value);
static void _GBSIOUDPProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate);
static void _GBSIOUDPSync(struct mTiming* timing, void* driver, uint32_t cyclesLate);
static void _finishTransfer(struct GBSIOUDP*, uint8_t, int);
static bool _checkBroadcasts(struct GBSIOUDP*);
static void _setSockTimeout(Socket, uint32_t);

static int _recvfrom(Socket s, uint8_t* buffer, int bufferSize, struct sockaddr_in* addy, int addySize);
static int _recvfrom(Socket s, uint8_t* buffer, int bufferSize, struct sockaddr_in* addy, int addySize) {
	#ifdef _WIN32
		return recvfrom(s, buffer, bufferSize, 0, (SOCKADDR*) addy, &addySize);
	#else
		return recvfrom(s, buffer, bufferSize, 0, (struct sockaddr*) addy, &addySize);
	#endif
}


static int _broadcast(Socket s, uint8_t* buffer, int bufferSize, struct sockaddr_in* addy, int addySize);
static int _broadcast(Socket s, uint8_t* buffer, int bufferSize, struct sockaddr_in* addy, int addySize) {
	#ifdef _WIN32
		return sendto(s, buffer, bufferSize, 0, (SOCKADDR*) addy, addySize);
	#else
		return sendto(s, buffer, bufferSize, 0, (struct sockaddr*) addy, addySize);
	#endif
}



static void _flush(Socket s);
static void _flush(Socket s) {
	uint8_t buffer[8];
	SocketSetBlocking(s, false);
	while (SocketRecv(s, &buffer, sizeof(buffer)) > 0) {}
}

static bool _checkBroadcasts(struct GBSIOUDP* sock) {
	sock->broadcast = SocketOpenUDP(27502, NULL);
	SocketSetBlocking(sock->broadcast, true);
	_setSockTimeout(sock->broadcast, 3000);


	mLOG(GB_SIO, DEBUG, "Checking for broadcast");
	uint8_t buffer;
	struct sockaddr_in from;
	int fromSize = sizeof(from);
	if (_recvfrom(sock->broadcast, &buffer, sizeof(buffer), &from, fromSize) > 0) {
		sock->serveraddr.sin_addr.s_addr = from.sin_addr.s_addr;
		return true;
	}

	return false;
}

static void _sendto(struct GBSIOUDP* s, uint8_t* data, int dataSize) {
	int addrSize = sizeof(s->clientaddr);

	#ifdef _WIN32
	if (m_serverMode) {
		sendto(s->data, data, dataSize, 0, (SOCKADDR*)&s->clientaddr, addrSize);
	} else {
		sendto(s->data, data, dataSize, 0, (SOCKADDR*)&s->serveraddr, addrSize);
	}
	#else
	if (m_serverMode) {
		sendto(s->data, data, dataSize, 0, (struct sockaddr*)&s->clientaddr, addrSize);
	} else {
		sendto(s->data, data, dataSize, 0, (struct sockaddr*)&s->serveraddr, addrSize);
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

bool GBSIOUDPInit(struct GBSIODriver* driver) {
	struct GBSIOUDP* sock = (struct GBSIOUDP*) driver;

	sock->event.context = sock;
    sock->event.name = "GB SIO TCPLINK";
    sock->event.callback = _GBSIOUDPProcessEvents;
    sock->event.priority = 0x80;

	sock->syncEvent.context = sock;
	sock->syncEvent.name = "GB SIO TCPLINK Sync Event";
	sock->syncEvent.callback = _GBSIOUDPSync;
	sock->syncEvent.priority = 0x80;


	mTimingSchedule(&driver->p->p->timing, &sock->syncEvent, 0);

	return true;
}

void GBSIOUDPDeinit(struct GBSIODriver* driver) {
	struct GBSIOUDP* sock = (struct GBSIOUDP*) driver;

	SocketClose(sock->data);

	// broadcast closed during connect

	mTimingDeschedule(&sock->d.p->p->timing, &sock->event);
	mTimingDeschedule(&sock->d.p->p->timing, &sock->syncEvent);
}

void GBSIOUDPCreate(struct GBSIOUDP* sock) {
	sock->d.init = GBSIOUDPInit;
	sock->d.deinit = GBSIOUDPDeinit;
	sock->d.writeSB = GBSIOUDPWriteSB;
	sock->d.writeSC = GBSIOUDPWriteSC;
	sock->transferActive = 0;
	sock->processing = false;

	sock->clockRequest[0] = CLOCK_REQUEST;
	sock->clockResponse[0] = CLOCK_RESPONSE;
}

void GBSIOUDPConnect(struct GBSIOUDP* sock, bool server) {

	sock->pendingSB = 0xFF;
	sock->needSync = false;


	memset(&sock->clientaddr, 0, sizeof(sock->clientaddr));
	memset(&sock->serveraddr, 0, sizeof(sock->serveraddr));
	sock->serveraddr.sin_family = AF_INET;
	sock->serveraddr.sin_port = htons(27500);

	SocketSubsystemInit();
	m_serverMode = !_checkBroadcasts(sock);


	uint8_t joinSig = 0;
	if (m_serverMode) {

		mLOG(GB_SIO, DEBUG, "Running TCPLINK server mode");
		sock->data = SocketOpenUDP(27500, NULL);
		SocketSetBlocking(sock->data, false);

		// Setup broadcast mode on broadcast socket
		uint8_t broadcastEnable = 1;
		setsockopt(sock->broadcast, SOL_SOCKET, SO_BROADCAST, (void *) &broadcastEnable, sizeof(broadcastEnable));

		struct sockaddr_in broadcastAddr;
		memset(&broadcastAddr, 0, sizeof(broadcastAddr));
		broadcastAddr.sin_family = AF_INET;
		// TODO: Get the broadcast address from the current network settings
		broadcastAddr.sin_addr.s_addr = inet_addr("192.168.1.255");
		broadcastAddr.sin_port = htons(27502);

		// Broadcast so client can pick up address and join
		int clientaddrSize = sizeof(sock->clientaddr);
		while (_recvfrom(sock->data, &joinSig, sizeof(joinSig), &sock->clientaddr, clientaddrSize) <= 0) {
			_broadcast(sock->broadcast, &broadcastEnable, sizeof(broadcastEnable), &broadcastAddr, sizeof(broadcastAddr));
			sleep(0.25);
		}
	} else {
		mLOG(GB_SIO, DEBUG, "Running TCPLINK client mode");
		sock->data = SocketOpenUDP(0, NULL);

		// Fire off signal to let host know we're ready to go
		_sendto(sock, &joinSig, sizeof(joinSig));
	}

	_flush(sock->data);
	mLOG(GB_SIO, DEBUG, "Data: %i", sock->data);

	close(sock->broadcast);
}

static void _GBSIOUDPProcessEvents(struct mTiming* timing, void* driver, uint32_t cyclesLate) {
	struct GBSIOUDP* sock = driver;
	sock->d.p->p->memory.io[GB_REG_SB] = sock->d.p->pendingSB;
	sock->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCClearEnable(sock->d.p->p->memory.io[GB_REG_SC]);
	sock->d.p->p->memory.io[GB_REG_IF] |= (1 << GB_IRQ_SIO);
	GBUpdateIRQs(sock->d.p->p);
	sock->d.p->pendingSB = 0xFF;
	sock->processing = false;
}

static void _finishTransfer(struct GBSIOUDP* sock, uint8_t update, int cycles) {
	// Copy over SB data to live SIO register
	sock->d.p->pendingSB = update;

	if (GBRegisterSCIsEnable(sock->d.p->p->memory.io[GB_REG_SC])) {
		mTimingDeschedule(&sock->d.p->p->timing, &sock->event);
		mTimingSchedule(&sock->d.p->p->timing, &sock->event, ((sock->d.p->period * (2 - sock->d.p->p->doubleSpeed)) * 8) - cycles);
		sock->processing = true;
	}
}


static void _GBSIOUDPSync(struct mTiming* timing, void* driver, uint32_t cyclesLate) {
	struct GBSIOUDP* node = driver;

	if (node->processing) {
		mTimingSchedule(&node->d.p->p->timing, &node->syncEvent, 24);
		return;
	}

	uint8_t buffer[2];
	if (SocketRecv(node->data, &buffer, sizeof(buffer)) == sizeof(buffer)){
		if (buffer[0] == CLOCK_REQUEST) {
			node->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCFillEnable(node->d.p->p->memory.io[GB_REG_SC]);
			node->clockResponse[1] = node->pendingSB;
			_sendto(node, node->clockResponse, sizeof(node->clockResponse));
			_finishTransfer(node, buffer[1], 0);
		} else {
			struct SM83Core* cpu = node->d.p->p->cpu;
			_finishTransfer(node, buffer[1], cpu->cycles - node->lastClock);
		}
	}

	mTimingSchedule(&node->d.p->p->timing, &node->syncEvent, 24);
}

static void GBSIOUDPWriteSB(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOUDP* node = (struct GBSIOUDP*) driver;
	node->pendingSB = value;
}

static uint8_t GBSIOUDPWriteSC(struct GBSIODriver* driver, uint8_t value) {
	struct GBSIOUDP* node = (struct GBSIOUDP*) driver;
	node->d.p->p->memory.io[GB_REG_SC] = GBRegisterSCFillEnable(node->d.p->p->memory.io[GB_REG_SC]);
	// We want to send some data
	if ((value & 0x81) == 0x81) {
		node->clockRequest[1] = node->pendingSB;

		struct SM83Core* cpu = driver->p->p->cpu;
		node->lastClock = cpu->cycles;
		_sendto(node, node->clockRequest, sizeof(node->clockRequest));

		mTimingDeschedule(&driver->p->p->timing, &node->d.p->event);
	}

	return value;
}
