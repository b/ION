/*
	tcpcli.c:	BP TCP-based convergence-layer input
			daemon, designed to serve as an input
			duct.

	Author: Scott Burleigh, JPL

	Copyright (c) 2004, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
	
									*/
#include "tcpcla.h"

static void	interruptThread()
{
	isignal(SIGTERM, interruptThread);
}

/*	*	*	Keepalive thread function	*	*	*/

typedef struct
{
	int		ductSocket;
	pthread_mutex_t	*mutex;
	struct sockaddr	socketName;
	int 		keepalivePeriod;
	int 		*cliRunning;
} KeepaliveThreadParms;

static void	terminateKeepaliveThread(KeepaliveThreadParms *parms)
{
	writeErrmsgMemos();
	writeMemo("[i] tcpcli keepalive thread stopping.");
	pthread_mutex_lock(parms->mutex);
	if (parms->ductSocket != -1)
	{
		close(parms->ductSocket);
		parms->ductSocket = -1;
	}
	pthread_mutex_unlock(parms->mutex);
}

static void *sendKeepalives(void *parm)
{
	KeepaliveThreadParms 	*parms = (KeepaliveThreadParms *) parm;
	int 			count = 0;
	int 			bytesSent;
	unsigned char		*buffer;

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putErrmsg("No memory for TCP buffer in tcpcli.", NULL);
		terminateKeepaliveThread(parms);
		return NULL;
	}
	
	while (*(parms->cliRunning))
	{
		snooze(1);
		count++;
		if (count < parms->keepalivePeriod)
		{
			continue;
		}

		/*	Time to send a keepalive.  Note that the
		 *	interval between keepalive attempts will be
		 *	KEEPALIVE_PERIOD plus (if the remote induct
		 *	is not reachable) the length of time taken
		 *	by TCP to determine that the connection
		 *	attempt will not succeed (e.g., 3 seconds).	*/

		count = 0;
		
		pthread_mutex_lock(parms->mutex);
		if(parms->ductSocket < 0 || !(*(parms->cliRunning)))	
		{
			*(parms->cliRunning) = 0;
			pthread_mutex_unlock(parms->mutex);
			continue;
		}
		bytesSent = sendBundleByTCPCL(&parms->socketName,
				&parms->ductSocket, 0, 0, buffer, &parms->keepalivePeriod);
		pthread_mutex_unlock(parms->mutex);
		if (bytesSent < 0)
		{
			break;
		}
		sm_TaskYield();
	}
	MRELEASE(buffer);
	terminateKeepaliveThread(parms);
	return NULL;
}

/*	*	*	Receiver thread functions	*	*	*/

typedef struct
{
	char		senderEidBuffer[SDRSTRING_BUFSZ];
	char		*senderEid;
	VInduct		*vduct;
	LystElt		elt;
	struct sockaddr	cloSocketName;
	pthread_mutex_t	*mutex;
	pthread_t	mainThread;
	int		bundleSocket;
	pthread_t	thread;
	int		*cliRunning;
} ReceiverThreadParms;

static void	terminateReceiverThread(ReceiverThreadParms *parms)
{
	writeErrmsgMemos();
	writeMemo("[i] tcpcli receiver thread stopping.");
	pthread_mutex_lock(parms->mutex);
	if (parms->bundleSocket != -1)
	{
		close(parms->bundleSocket);
		parms->bundleSocket = -1;
	}

	lyst_delete(parms->elt);
	pthread_mutex_unlock(parms->mutex);
}

static void	*receiveBundles(void *parm)
{
	/*	Main loop for bundle reception thread on one
	 *	connection, terminating when connection is lost.	*/

	ReceiverThreadParms	*parms = (ReceiverThreadParms *) parm;
	KeepaliveThreadParms	*kparms;
	AcqWorkArea		*work;
	char			*buffer;
	pthread_t		kthread = NULL;

	buffer = MTAKE(TCPCLA_BUFSZ);
	if (buffer == NULL)
	{
		putSysErrmsg("tcpcli can't get TCP buffer", NULL);
		terminateReceiverThread(parms);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		return NULL;
	}

	work = bpGetAcqArea(parms->vduct);
	if (work == NULL)
	{
		putSysErrmsg("tcpcli can't get acquisition work area", NULL);
		MRELEASE(buffer);
		terminateReceiverThread(parms);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		return NULL;
	}
	
	/*	Set up parameters for the keepalive thread	*/
	kparms = (KeepaliveThreadParms *)
			MTAKE(sizeof(KeepaliveThreadParms));
	if (kparms == NULL)
	{
		putErrmsg("tcpcli can't allocate for new keepalive thread",
				NULL);
		MRELEASE(buffer);
		bpReleaseAcqArea(work);
		terminateReceiverThread(parms);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		return NULL;
	}

	pthread_mutex_lock(parms->mutex);

	/*	Making sure there is no race condition when keep alive
	 *	values are set.						*/

	if(sendContactHeader(&parms->bundleSocket, (unsigned char *)buffer) < 0)
	{
		putSysErrmsg("tcpcli couldn't send contact header", NULL);
		MRELEASE(buffer);
		MRELEASE(kparms);
		close(parms->bundleSocket);
		parms->bundleSocket = -1;
		pthread_mutex_unlock(parms->mutex);

		bpReleaseAcqArea(work);
		terminateReceiverThread(parms);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		return NULL;
	}

	pthread_mutex_unlock(parms->mutex);
	if(receiveContactHeader(&parms->bundleSocket, (unsigned char *)buffer, &kparms->keepalivePeriod) < 0)
	{
		putSysErrmsg("tcpcli couldn't receive contact header", NULL);
		MRELEASE(buffer);
		MRELEASE(kparms);
		pthread_mutex_lock(parms->mutex);
		close(parms->bundleSocket);
		parms->bundleSocket = -1;
		pthread_mutex_unlock(parms->mutex);
		bpReleaseAcqArea(work);
		terminateReceiverThread(parms);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		return NULL;
	}


	/*
	 * The keep alive thread is created only if the negotiated
	 * keep alive is greater than 0.
	 */
	if( kparms->keepalivePeriod > 0 )
	{
		kparms->ductSocket = parms->bundleSocket;
		kparms->socketName = parms->cloSocketName;
		kparms->mutex = parms->mutex;
		kparms->cliRunning = parms->cliRunning;
	 
		/*
		 * Creating a thread to send out keep alives, which
		 * makes the TCPCL bi directional
		 */
		if (pthread_create(&kthread, NULL, sendKeepalives, kparms))
		{
			putSysErrmsg("tcpcli can't create new thread for \
keepalives", NULL);
			*(parms->cliRunning) = 0;
		}
	}

	/*	Now start receiving bundles.				*/

	iblock(SIGTERM);
	while (*(parms->cliRunning))
	{
		if (bpBeginAcq(work, 0, parms->senderEid) < 0)
		{
			putErrmsg("Can't begin acquisition of bundle.", NULL);
			*(parms->cliRunning) = 0;
			continue;
		}

		switch (receiveBundleByTcpCL(parms->bundleSocket, work, buffer))
		{
		case -1:
			putErrmsg("Can't acquire bundle.", NULL);

			/*	Intentional fall-through to next case.	*/

		case 0:				/*	Normal stop.	*/
			*(parms->cliRunning) = 0;
			continue;

		default:
			break;			/*	Out of switch.	*/
		}

		if (bpEndAcq(work) < 0)
		{
			putErrmsg("Can't end acquisition of bundle.", NULL);
			*(parms->cliRunning) = 0;
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	/*	End of receiver thread; release resources.		*/

	if (kthread)
	{
		pthread_kill(kthread, SIGTERM);
		pthread_join(kthread, NULL);
	}

	bpReleaseAcqArea(work);
	MRELEASE(buffer);
	terminateReceiverThread(parms);
	MRELEASE(kparms);
	MRELEASE(parms->cliRunning);
	MRELEASE(parms);
	return NULL;
}

/*	*	*	Access thread functions	*	*	*	*/

typedef struct
{
	VInduct			*vduct;
	struct sockaddr		socketName;
	struct sockaddr_in	*inetName;
	int			ductSocket;
	pthread_t		mainThread;
	int			running;
} AccessThreadParms;

static void	*spawnReceivers(void *parm)
{
	/*	Main loop for acceptance of connections and
	 *	creation of receivers to service those connections.	*/

	AccessThreadParms	*atp = (AccessThreadParms *) parm;
	pthread_mutex_t		mutex;
	Lyst			threads;
	int			newSocket;
	struct sockaddr		cloSocketName;
	socklen_t		nameLength;
	ReceiverThreadParms	*parms;
	LystElt			elt;
	struct sockaddr_in	*fromAddr;
	unsigned int		hostNbr;
	char			hostName[MAXHOSTNAMELEN + 1];
	pthread_t		thread;

	pthread_mutex_init(&mutex, NULL);
	threads = lyst_create_using(getIonMemoryMgr());
	if (threads == NULL)
	{
		putSysErrmsg("tcpcli can't create threads list", NULL);
		pthread_mutex_destroy(&mutex);
		return NULL;
	}

	/*	Can now begin accepting connections from remote
	 *	contacts.  On failure, take down the whole CLI.		*/

	iblock(SIGTERM);
	while (1)
	{
		nameLength = sizeof(struct sockaddr);
		newSocket = accept(atp->ductSocket, &cloSocketName,
				&nameLength);
		if (newSocket < 0)
		{
			putSysErrmsg("tcpcli accept() failed", NULL);
			pthread_kill(atp->mainThread, SIGTERM);
			continue;
		}

		if (atp->running == 0)
		{
			break;	/*	Main thread has shut down.	*/
		}

		parms = (ReceiverThreadParms *)
				MTAKE(sizeof(ReceiverThreadParms));
		if (parms == NULL)
		{
			putErrmsg("tcpcli can't allocate for new thread", NULL);
			pthread_kill(atp->mainThread, SIGTERM);
			continue;
		}

		parms->vduct = atp->vduct;
		pthread_mutex_lock(&mutex);
		parms->elt = lyst_insert_last(threads, parms);
		pthread_mutex_unlock(&mutex);
		if (parms->elt == NULL)
		{
			putErrmsg("tcpcli can't allocate lyst element for new thread", NULL);
			MRELEASE(parms);
			pthread_kill(atp->mainThread, SIGTERM);
			continue;
		}
		parms->mutex = &mutex;
		parms->mainThread = atp->mainThread;
		parms->bundleSocket = newSocket;
		fromAddr = (struct sockaddr_in *) &cloSocketName;
		memcpy((char *) &hostNbr,
				(char *) &(fromAddr->sin_addr.s_addr), 4);
		hostNbr = ntohl(hostNbr);
		if (getInternetHostName(hostNbr, hostName))
		{
			parms->senderEid = parms->senderEidBuffer;
			getSenderEid(&(parms->senderEid), hostName);
		}
		else
		{
			parms->senderEid = NULL;
		}

		parms->cloSocketName = cloSocketName;
		parms->cliRunning = MTAKE(sizeof(int));
		if (parms->cliRunning == NULL)
		{
			putSysErrmsg("tcpcli can't create new thread", NULL);
			MRELEASE(parms);
			pthread_kill(atp->mainThread, SIGTERM);
			continue;
		}

		*(parms->cliRunning) = 1;
		if (pthread_create(&(parms->thread), NULL, receiveBundles,
					parms))
		{
			putSysErrmsg("tcpcli can't create new thread", NULL);
			MRELEASE(parms->cliRunning);
			MRELEASE(parms);
			pthread_kill(atp->mainThread, SIGTERM);
		}

		/*	Make sure other tasks have a chance to run.	*/

		sm_TaskYield();
	}

	close(atp->ductSocket);
	writeErrmsgMemos();

	/*	Shut down all current CLI threads cleanly.		*/

	while (1)
	{
		pthread_mutex_lock(&mutex);
		elt = lyst_first(threads);
		if (elt == NULL)	/*	All threads shut down.	*/
		{
			pthread_mutex_unlock(&mutex);
			break;
		}

		/*	Trigger termination of thread.			*/

		parms = (ReceiverThreadParms *) lyst_data(elt);
		thread = parms->thread;
		if(sendShutDownMessage(&parms->bundleSocket, SHUT_DN_NO, -1) < 0)
		{
			putErrmsg("Sending Shutdown message failed!!",NULL);
		}
		close(parms->bundleSocket);
		parms->bundleSocket = -1;
		pthread_mutex_unlock(&mutex);
		MRELEASE(parms->cliRunning);
		MRELEASE(parms);
		pthread_kill(thread, SIGTERM);
		pthread_join(thread, NULL);
	}

	lyst_destroy(threads);
	writeErrmsgMemos();
	writeMemo("[i] tcpcli access thread has ended.");
	pthread_mutex_destroy(&mutex);
	return NULL;
}

/*	*	*	Main thread functions	*	*	*	*/

#if defined (VXWORKS) || defined (RTEMS)
int	tcpcli(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char	*ductName = (char *) a1;
#else
int	main(int argc, char *argv[])
{
	char	*ductName = (argc > 1 ? argv[1] : NULL);
#endif
	VInduct			*vduct;
	PsmAddress		vductElt;
	Sdr			sdr;
	Induct			duct;
	ClProtocol		protocol;
	char			*hostName;
	unsigned short		portNbr;
	unsigned int		hostNbr;
	AccessThreadParms	atp;
	socklen_t		nameLength;
	char			*tcpDelayString;
	pthread_t		accessThread;
	int			fd;

	if (ductName == NULL)
	{
		PUTS("Usage: tcpcli <local host name>[:<port number>]");
		return 0;
	}

	if (bpAttach() < 0)
	{
		putErrmsg("tcpcli can't attach to BP.", NULL);
		return 1;
	}

	findInduct("tcp", ductName, &vduct, &vductElt);
	if (vductElt == 0)
	{
		putErrmsg("No such tcp duct.", ductName);
		return 1;
	}

	if (vduct->cliPid > 0 && vduct->cliPid != sm_TaskIdSelf())
	{
		putErrmsg("CLI task is already started for this duct.",
				itoa(vduct->cliPid));
		return 1;
	}

	/*	All command-line arguments are now validated.		*/

	sdr = getIonsdr();
	sdr_read(sdr, (char *) &duct, sdr_list_data(sdr, vduct->inductElt),
			sizeof(Induct));
	sdr_read(sdr, (char *) &protocol, duct.protocol, sizeof(ClProtocol));
	if (protocol.nominalRate <= 0)
	{
		vduct->acqThrottle.nominalRate = DEFAULT_TCP_RATE;
	}
	else
	{
		vduct->acqThrottle.nominalRate = protocol.nominalRate;
	}

	hostName = ductName;
	parseSocketSpec(ductName, &portNbr, &hostNbr);
	if (portNbr == 0)
	{
		portNbr = BpTcpDefaultPortNbr;
	}

	portNbr = htons(portNbr);
	if (hostNbr == 0)
	{
		putErrmsg("Can't get IP address for host.", hostName);
		return 1;
	}

	hostNbr = htonl(hostNbr);
	atp.vduct = vduct;
	memset((char *) &(atp.socketName), 0, sizeof(struct sockaddr));
	atp.inetName = (struct sockaddr_in *) &(atp.socketName);
	atp.inetName->sin_family = AF_INET;
	atp.inetName->sin_port = portNbr;
	memcpy((char *) &(atp.inetName->sin_addr.s_addr), (char *) &hostNbr, 4);
	atp.ductSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	
	/* set desired keep alive period to 15 and since there is no negotiated
	 * keep alive period it is 0	*/

	tcpDesiredKeepAlivePeriod = KEEPALIVE_PERIOD;

	if (atp.ductSocket < 0)
	{
		putSysErrmsg("Can't open TCP socket", NULL);
		return 1;
	}

	nameLength = sizeof(struct sockaddr);
	if (reUseAddress(atp.ductSocket)
	|| bind(atp.ductSocket, &(atp.socketName), nameLength) < 0
	|| listen(atp.ductSocket, 5) < 0
	|| getsockname(atp.ductSocket, &(atp.socketName), &nameLength) < 0)
	{
		close(atp.ductSocket);
		putSysErrmsg("Can't initialize socket", NULL);
		return 1;
	}

	tcpDelayString = getenv("TCP_DELAY_NSEC_PER_BYTE");
	if (tcpDelayString == NULL)
	{
		tcpDelayEnabled = 0;
	}
	else
	{
		tcpDelayEnabled = 1;
		tcpDelayNsecPerByte = strtol(tcpDelayString, NULL, 0);
		if (tcpDelayNsecPerByte < 0
		|| tcpDelayNsecPerByte > 16384)
		{
			tcpDelayNsecPerByte = 0;
		}
	}

	/*	Initialize sender endpoint ID lookup.			*/

	ipnInit();
	dtn2Init();

	/*	Set up signal handling: SIGTERM is shutdown signal.	*/

	isignal(SIGTERM, interruptThread);

	/*	Start the access thread.				*/

	atp.running = 1;
	atp.mainThread = pthread_self();
	if (pthread_create(&accessThread, NULL, spawnReceivers, &atp))
	{
		close(atp.ductSocket);
		putSysErrmsg("tcpcli can't create access thread", NULL);
		return 1;
	}

	/*	Now sleep until interrupted by SIGTERM, at which point
	 *	it's time to stop the induct.				*/

	writeMemo("[i] tcpcli is running.");
	snooze(2000000000);

	/*	Time to shut down.					*/

	atp.running = 0;

	/*	Wake up the access thread by connecting to it.		*/

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd >= 0)
	{
		connect(fd, &(atp.socketName), sizeof(struct sockaddr));

		/*	Immediately discard the connected socket.	*/

		close(fd);
	}

	pthread_join(accessThread, NULL);
	writeErrmsgMemos();
	writeMemo("[i] tcpcli duct has ended.");
	bp_detach();
	return 0;
}
