// Twitch bridge: local TCP listener polled once per frame from D_DoomLoop.
//
// Disabled unless -twitchport <port> is given. Listens on 127.0.0.1 only.
// Accepts newline-terminated lines of the form:   <eventname> [a1] [a2] [a3]
// and turns them into "netevent <eventname> a1 a2 a3". Raw console commands are
// never accepted: the event name is restricted to [A-Za-z0-9_] and args to ints.

#include <string.h>
#include <stdlib.h>
#include <string>
#include "c_dispatch.h"
#include "m_argv.h"
#include "printf.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
static void SetNonBlocking(sock_t s) { u_long m = 1; ioctlsocket(s, FIONBIO, &m); }
static void CloseSock(sock_t s) { closesocket(s); }
static bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
static void SetNonBlocking(sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static void CloseSock(sock_t s) { close(s); }
static bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

static sock_t ListenSock = SOCK_INVALID;
static sock_t ClientSock = SOCK_INVALID;
static std::string LineBuf;
static bool Enabled = false;

static const int MAX_LINE = 128;
static const int MAX_CMDS_PER_FRAME = 16;

void TwitchBridge_Init()
{
	const char *portstr = Args->CheckValue("-twitchport");
	if (!portstr) return;

	int port = atoi(portstr);
	if (port <= 0 || port > 65535) { Printf("Twitch bridge: bad port\n"); return; }

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
#endif

	ListenSock = socket(AF_INET, SOCK_STREAM, 0);
	if (ListenSock == SOCK_INVALID) { Printf("Twitch bridge: socket() failed\n"); return; }

	int yes = 1;
	setsockopt(ListenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only

	if (bind(ListenSock, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(ListenSock, 1) != 0)
	{
		Printf("Twitch bridge: could not listen on 127.0.0.1:%d\n", port);
		CloseSock(ListenSock);
		ListenSock = SOCK_INVALID;
		return;
	}
	SetNonBlocking(ListenSock);
	Enabled = true;
	Printf("Twitch bridge: listening on 127.0.0.1:%d\n", port);
}

// Returns true and fills 'out' if the line is a valid event.
static bool BuildCommand(const std::string &line, std::string &out)
{
	size_t i = 0, n = line.size();
	while (i < n && line[i] == ' ') i++;

	size_t nameStart = i;
	while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
	if (i == nameStart) return false;
	out = "netevent " + line.substr(nameStart, i - nameStart);

	int args = 0;
	while (i < n)
	{
		while (i < n && line[i] == ' ') i++;
		if (i >= n) break;
		size_t s = i;
		if (line[i] == '-') i++;
		size_t d = i;
		while (i < n && isdigit((unsigned char)line[i])) i++;
		if (i == d || i - s > 10 || args >= 3) return false;   // not an int / too many args
		out += " " + line.substr(s, i - s);
		args++;
	}
	return true;
}

static void DropClient()
{
	if (ClientSock != SOCK_INVALID) CloseSock(ClientSock);
	ClientSock = SOCK_INVALID;
	LineBuf.clear();
}

void TwitchBridge_Poll()
{
	if (!Enabled) return;

	if (ClientSock == SOCK_INVALID)
	{
		sock_t c = accept(ListenSock, nullptr, nullptr);
		if (c == SOCK_INVALID) return;
		SetNonBlocking(c);
		ClientSock = c;
		Printf("Twitch bridge: client connected\n");
	}

	char buf[512];
	int cmds = 0;
	for (;;)
	{
		int r = (int)recv(ClientSock, buf, sizeof(buf), 0);
		if (r > 0)
		{
			for (int k = 0; k < r; k++)
			{
				char ch = buf[k];
				if (ch == '\n' || ch == '\r')
				{
					if (!LineBuf.empty())
					{
						std::string cmd;
						if (cmds < MAX_CMDS_PER_FRAME && BuildCommand(LineBuf, cmd))
						{
							AddCommandString(cmd.c_str());
							cmds++;
						}
						LineBuf.clear();
					}
				}
				else if ((int)LineBuf.size() < MAX_LINE) LineBuf.push_back(ch);
				else LineBuf.clear();   // overlong line: discard
			}
		}
		else if (r == 0) { Printf("Twitch bridge: client disconnected\n"); DropClient(); return; }
		else
		{
			if (!WouldBlock()) DropClient();
			return;
		}
	}
}
