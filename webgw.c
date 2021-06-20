#include <syslog.h>
#include <err.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <sys/select.h>

#include "extern.h"
#include "config.h"

void sigpipe()
{
	syslog(LOG_ERR, "sigpipe");
}

int main(int argc, char *argv[])
{
	static struct webgw ctx;

	openlog(argv[0], LOG_NDELAY | LOG_CONS | LOG_PID, LOG_DAEMON);

	signal(SIGPIPE, sigpipe);

#if 0
	if (daemon(0, 0) < 0) {
		syslog(LOG_ERR, "failed to daemonize: %m");
		err(1, "failed to daemonize");
	}
#endif

	init(&ctx, LISTEN_ADDR, LISTEN_PORT);

/*
	Practically no need for chroot because we already set pledge list
	and it does not include rpath etc

	if (chroot("/var/empty") < 0 || chdir("/") != 0)
		warn("chroot");
	setresuid(getuid(), getuid(), getuid());
*/

#define PLEDGE_LIST "stdio inet dns  wpath cpath proc rpath"
	if (pledge(PLEDGE_LIST, NULL) < 0) {
		syslog(LOG_ERR, "failed to pledge '%s': %m", PLEDGE_LIST);
		return 1;
	}

	for (;;)
		server_dispatch_events(&ctx);

	return 0;
}
