/*
	amsd.c:	configuration and registration services daemon for
		Asynchronous Message Service (AMS).

	Author: Scott Burleigh, JPL

	Copyright (c) 2005, California Institute of Technology.
	ALL RIGHTS RESERVED.  U.S. Government Sponsorship
	acknowledged.
									*/
#include "amscommon.h"

typedef struct
{
	int		csRequired;
	int		csRunning;
	char		*csEndpointSpec;
	LystElt		startOfFailoverChain;
	pthread_t	csThread;
	pthread_t	csHeartbeatThread;
	Lyst		csEvents;
	struct llcv_str	csEventsCV_str;
	Llcv		csEventsCV;
	MamsInterface	tsif;
} CsState;

typedef struct
{
	int		rsRequired;
	int		rsRunning;
	char		*rsAppName;
	char		*rsAuthName;
	char		*rsUnitName;
	Venture		*venture;
	Cell		*cell;
	int		cellHeartbeats;
	int		undeclaredNodesCount;
	char		undeclaredNodes[MAX_NODE_NBR + 1];
	MamsEndpoint	*csEndpoint;
	LystElt		csEndpointElt;
	int		heartbeatsMissed;	/*	From CS.	*/
	pthread_t	rsThread;
	pthread_t	rsHeartbeatThread;
	Lyst		rsEvents;
	struct llcv_str	rsEventsCV_str;
	Llcv		rsEventsCV;
	MamsInterface	tsif;
} RsState;

static int	amsdRunning;
static char	*zeroLengthEpt = "";

static void	shutDownAmsd()
{
	amsdRunning = 0;
}

/*	*	*	Configuration server code	*	*	*/

static void	stopOtherConfigServers(CsState *csState)
{
	LystElt		elt;
	MamsEndpoint	*ep;

	for (elt = lyst_next(csState->startOfFailoverChain); elt;
			elt = lyst_next(elt))
	{
		ep = (MamsEndpoint *) lyst_data(elt);
		if (sendMamsMsg(ep, &(csState->tsif), I_am_running, 0, 0, NULL)
				< 0)
		{
			putErrmsg("Can't send I_am_running message.", NULL);
		}
	}
}

static void	*csHeartbeat(void *parm)
{
	CsState		*csState = (CsState *) parm;
	pthread_mutex_t	mutex;
	pthread_cond_t	cv;
	sigset_t	signals;
	int		cycleCount = 6;
	int		i;
	Venture		*venture;
	int		j;
	Unit		*unit;
	Cell		*cell;
	int		result;
	struct timeval	workTime;
	struct timespec	deadline;

	if (pthread_mutex_init(&mutex, NULL))
	{
		putSysErrmsg("can't start heartbeat, mutex init failed", NULL);
		return NULL;
	}

	if (pthread_cond_init(&cv, NULL))
	{
		pthread_mutex_destroy(&mutex);
		putSysErrmsg("can't start heartbeat, cond init failed", NULL);
		return NULL;
	}

	sigfillset(&signals);
	pthread_sigmask(SIG_BLOCK, &signals, NULL);
	while (1)
	{
		LOCK_MIB;
		if (cycleCount > 5)	/*	Every N5_INTERVAL sec.	*/
		{
			cycleCount = 0;
			stopOtherConfigServers(csState);
		}

		for (i = 1; i <= MaxVentureNbr; i++)
		{
			venture = mib->ventures[i];
			if (venture == NULL) continue;
			for (j = 0; j <= MaxUnitNbr; j++)
			{
				unit = venture->units[j];
				if (unit == NULL
				|| (cell = unit->cell)->mamsEndpoint.ept
						== NULL)
				{
					continue;
				}

				if (cell->heartbeatsMissed == 3)
				{
					clearMamsEndpoint
						(&(cell->mamsEndpoint));
				}
				else if (cell->heartbeatsMissed < 3)
				{
					if (sendMamsMsg (&cell->mamsEndpoint,
						&csState->tsif, heartbeat,
						0, 0, NULL) < 0)
					{
						putErrmsg("can't send \
heartbeat", NULL);
					}
				}

				cell->heartbeatsMissed++;
			}
		}

		/*	Now sleep for N3_INTERVAL seconds.		*/

		UNLOCK_MIB;
		getCurrentTime(&workTime);
		deadline.tv_sec = workTime.tv_sec + N3_INTERVAL;
		deadline.tv_nsec = workTime.tv_usec * 1000;
		pthread_mutex_lock(&mutex);
		result = pthread_cond_timedwait(&cv, &mutex, &deadline);
		pthread_mutex_unlock(&mutex);
		if (result)
		{
			errno = result;
			if (errno != ETIMEDOUT)
			{
				putSysErrmsg("heartbeat failure", NULL);
				break;
			}
		}

		cycleCount++;
	}

	putErrmsg("CS heartbeat thread ended.", NULL);
	return NULL;
}

static void	cleanUpCsState(CsState *csState)
{
	if (csState->tsif.ts)
	{
		csState->tsif.ts->shutdownFn(csState->tsif.sap);
	}

	if (csState->csEvents)
	{
		lyst_destroy(csState->csEvents);
		csState->csEvents = NULL;
	}

	llcv_close(csState->csEventsCV);
	csState->csRunning = 0;
}

static void	reloadRsRegistrations(CsState *csState)
{
	return;		/*	Maybe do this eventually.		*/
}

static void	processMsgToCs(CsState *csState, AmsEvt *evt)
{
	MamsMsg		*msg = (MamsMsg *) (evt->value);
	Venture		*venture;
	Unit		*unit;
	Cell		*cell;
	char		*cursor;
	int		bytesRemaining;
	char		*ept;
	int		eptLength;
	MamsEndpoint	endpoint;
	int		i;
	int		supplementLength;
	char		*supplement;
	char		reasonCode;
	int		cellspecLength;
	char		*cellspec;
	int		result;
	int		unitNbr;

PUTMEMO("CS got msg of type", itoa(msg->type));
PUTMEMO("...from role", itoa(msg->roleNbr));
	if (msg->type == I_am_running)
	{
		if (enqueueMamsCrash(csState->csEventsCV, "Outranked") < 0)
		{
			putErrmsg("CS can't enqueue own Crash event", NULL);
		}

		return;
	}

	/*	All other messages to CS are from registrars and
	 *	nodes, which need valid venture and unit numbers.	*/

	venture = mib->ventures[msg->ventureNbr];
	if (venture == NULL)
	{
		unit = NULL;
	}
	else
	{
		unit = venture->units[msg->unitNbr];
	}

	switch (msg->type)
	{
	case heartbeat:
		if (unit == NULL
		|| (cell = unit->cell)->mamsEndpoint.ept == NULL)
		{
			return;	/*	Ignore invalid heartbeat.	*/
		}

		/*	Legitimate heartbeat from registered RS.	*/

		cell->heartbeatsMissed = 0;
		return;

	case announce_registrar:
		cursor = msg->supplement;
		bytesRemaining = msg->supplementLength;
		ept = parseString(&cursor, &bytesRemaining, &eptLength);
		if (ept == NULL)	/*	Ignore malformed msg.	*/
		{
			return;
		}

		if (unit == NULL)
		{
			reasonCode = REJ_NO_UNIT;
			endpoint.ept = ept;
			if (((mib->pts->parseMamsEndpointFn)(&endpoint)) < 0)
			{
				if (sendMamsMsg(&endpoint, &(csState->tsif),
						rejection, msg->memo, 1,
						&reasonCode) < 0)
				{
					putErrmsg("CS can't reject registrar.",
							NULL);
				}
			}

			return;
		}

		cell = unit->cell;
		if (cell->mamsEndpoint.ept == NULL)	/*	Needed.	*/
		{
			if (constructMamsEndpoint(&(cell->mamsEndpoint),
					eptLength, ept) < 0)
			{
				putErrmsg("CS can't register new registrar.",
						itoa(msg->unitNbr));
				return;
			}
		}

		/*	(Re-announcement by current registrar is okay.)	*/

		if (strcmp(ept, cell->mamsEndpoint.ept) != 0)
		{
			/*	Already have registrar for this cell.	*/

			reasonCode = REJ_DUPLICATE;
			endpoint.ept = ept;
			if (((mib->pts->parseMamsEndpointFn)(&endpoint)) < 0)
			{
				if (sendMamsMsg(&endpoint, &(csState->tsif),
						rejection, msg->memo, 1,
						&reasonCode) < 0)
				{
					putErrmsg("CS can't reject registrar.",
							NULL);
				}
			}

			return;
		}

		/*	Accept (possibly re-announced) registration of
		 *	registrar.					*/

		cell->heartbeatsMissed = 0;
		if (sendMamsMsg(&(cell->mamsEndpoint), &(csState->tsif),
				registrar_noted, msg->memo, 0, NULL) < 0)
		{
			putErrmsg("CS can't accept registrar.", NULL);
		}

		/*	Tell all other registrars about this one.	*/

		ept = cell->mamsEndpoint.ept;
		supplementLength = 2 + strlen(ept) + 1;
		supplement = MTAKE(supplementLength);
		if (supplement == NULL)
		{
			putSysErrmsg("CS can't announce new registrar",
					itoa(msg->unitNbr));
			return;
		}

		supplement[0] = (char) ((msg->unitNbr >> 8) & 0xff);
		supplement[1] = (char) (msg->unitNbr & 0xff);
		istrcpy(supplement + 2, ept, supplementLength - 2);
		cellspec = NULL;
		for (i = 0; i <= MaxUnitNbr; i++)
		{
			if (i == msg->unitNbr)	/*	New one itself.	*/
			{
				continue;
			}

			unit = venture->units[i];
			if (unit == NULL
			|| (cell = unit->cell)->mamsEndpoint.ept == NULL)
			{
				continue;
			}

			/*	Tell this registrar about the new one.	*/

			if (sendMamsMsg(&(cell->mamsEndpoint),
					&(csState->tsif), cell_spec, 0,
					supplementLength, supplement) < 0)
			{
				putErrmsg("CS can't send cell_spec", NULL);
				break;
			}

			/*	Tell the new registrar about this one.	*/

			ept = cell->mamsEndpoint.ept;
			cellspecLength = 2 + strlen(ept) + 1;
			cellspec = MTAKE(cellspecLength);
			if (cellspec == NULL)
			{
				putSysErrmsg("CS can't orient new registrar",
						itoa(msg->unitNbr));
				return;
			}

			cellspec[0] = (char) ((unit->nbr >> 8) & 0xff);
			cellspec[1] = (char) (unit->nbr & 0xff);
			istrcpy(cellspec + 2, ept, cellspecLength - 2);
			if (sendMamsMsg(&endpoint, &(csState->tsif), cell_spec,
					0, cellspecLength, cellspec) < 0)
			{
				putErrmsg("CS can't send cell_spec.", NULL);
			}

			MRELEASE(cellspec);
		}

		if (cellspec == NULL)	/*	No other registrars.	*/
		{
			/*	Notify the new registrar, by sending
			 *	it a cell_spec announcing itself.	*/

			unit = venture->units[msg->unitNbr];
			cell = unit->cell;
			if (sendMamsMsg(&(cell->mamsEndpoint),
					&(csState->tsif), cell_spec, 0,
					supplementLength, supplement) < 0)
			{
				putErrmsg("CS can't send cell_spec", NULL);
				break;
			}
		}

		MRELEASE(supplement);
		return;

	case registrar_query:
		cursor = msg->supplement;
		bytesRemaining = msg->supplementLength;
		ept = parseString(&cursor, &bytesRemaining, &eptLength);
		if (ept == NULL)	/*	Ignore malformed msg.	*/
		{
			return;
		}

		endpoint.ept = ept;
		if (((mib->pts->parseMamsEndpointFn)(&endpoint)) < 0)
		{
			return;		/*	Can't respond.		*/
		}

		if (unit == NULL)
		{
			unitNbr = 0;
			ept = zeroLengthEpt;
		}
		else
		{
			unitNbr = unit->nbr;
			ept = unit->cell->mamsEndpoint.ept;
			if (ept == NULL)	/*	No registrar.	*/
			{
				ept = zeroLengthEpt;
			}
		}

		if (ept == zeroLengthEpt)
		{
			if (sendMamsMsg(&endpoint, &(csState->tsif),
				registrar_unknown, msg->memo, 0, NULL) < 0)
			{
				putErrmsg("CS can't send registrar_unknown",
						NULL);
			}

			return;
		}

		supplementLength = 2 + strlen(ept) + 1;
		supplement = MTAKE(supplementLength);
		if (supplement == NULL)
		{
			putSysErrmsg("CS can't report on registrar",
					itoa(unitNbr));
			return;
		}

		supplement[0] = (char) ((unitNbr >> 8) & 0xff);
		supplement[1] = (char) (unitNbr & 0xff);
		istrcpy(supplement + 2, ept, supplementLength - 2);
		result = sendMamsMsg(&endpoint, &(csState->tsif), cell_spec,
			       	msg->memo, supplementLength, supplement);
		MRELEASE(supplement);
		if (result < 0)
		{
			putErrmsg("CS can't send cell_spec", NULL);
		}

		return;

	default:		/*	Inapplicable message; ignore.	*/
		return;
	}
}

static void	*csMain(void *parm)
{
	CsState		*csState = (CsState *) parm;
	sigset_t	signals;
	LystElt		elt;
	AmsEvt		*evt;

	sigfillset(&signals);
	pthread_sigmask(SIG_BLOCK, &signals, NULL);
	csState->csRunning = 1;
PUTS("Configuration server is running.");
	while (1)
	{
		if (llcv_wait(csState->csEventsCV, llcv_lyst_not_empty,
					LLCV_BLOCKING) < 0)
		{
			putSysErrmsg("CS thread failed getting event", NULL);
			break;
		}

		llcv_lock(csState->csEventsCV);
		elt = lyst_first(csState->csEvents);
		if (elt == NULL)
		{
			llcv_unlock(csState->csEventsCV);
			continue;
		}

		evt = (AmsEvt *) lyst_data_set(elt, NULL);
		lyst_delete(elt);
		llcv_unlock(csState->csEventsCV);
		switch (evt->type)
		{
		case MAMS_MSG_EVT:
			LOCK_MIB;
			processMsgToCs(csState, evt);
			UNLOCK_MIB;
			recycleEvent(evt);
			continue;

		case CRASH_EVT:
			putErrmsg("CS thread terminated", evt->value);
			recycleEvent(evt);
			break;	/*	Out of switch.			*/

		default:	/*	Inapplicable event; ignore.	*/
			recycleEvent(evt);
			continue;
		}

		break;		/*	Out of loop.			*/
	}

	/*	Operation of the configuration server is terminated.	*/

	writeErrmsgMemos();
	pthread_cancel(csState->csHeartbeatThread);
	pthread_join(csState->csHeartbeatThread, NULL);
	cleanUpCsState(csState);
	return NULL;
}

static int	startConfigServer(CsState *csState)
{
	MamsInterface	*tsif;
	LystElt		elt;
	MamsEndpoint	*ep;

	/*	Load the necessary state data structures.		*/

	csState->startOfFailoverChain = NULL;
	csState->csEvents = lyst_create_using(amsMemory);
	if (csState->csEvents == NULL)
	{
		putSysErrmsg(NoMemoryMemo, NULL);
		return -1;
	}

	lyst_delete_set(csState->csEvents, destroyEvent, NULL);
	csState->csEventsCV = llcv_open(csState->csEvents,
			&(csState->csEventsCV_str));
	if (csState->csEventsCV == NULL)
	{
		putSysErrmsg("amsd can't open CS events llcv", NULL);
		return -1;
	}

	/*	Initialize the MAMS transport service interface.	*/

	tsif = &(csState->tsif);
	tsif->ts = mib->pts;
	tsif->endpointSpec = csState->csEndpointSpec;
	tsif->eventsQueue = csState->csEventsCV;
	if (tsif->ts->mamsInitFn(tsif) < 0)
	{
		putErrmsg("amsd can't initialize CS MAMS interface", NULL);
		return -1;
	}

	/*	Make sure the PTS endpoint opened by the initialize
	 *	function is one of the ones that the continuum knows
	 *	about.							*/

	LOCK_MIB;
	for (elt = lyst_first(mib->csEndpoints); elt; elt = lyst_next(elt))
	{
		ep = (MamsEndpoint *) lyst_data(elt);
		if (strcmp(ep->ept, tsif->ept) == 0)
		{
			csState->startOfFailoverChain = elt;
			break;
		}
	}

	UNLOCK_MIB;
	if (csState->startOfFailoverChain == NULL)
	{
		putErrmsg("Endpoint spec doesn't match any catalogued \
CS endpoint", csState->csEndpointSpec);
		return -1;
	}

	/*	Start the MAMS transport service receiver thread.	*/

	if (pthread_create(&(tsif->receiver), NULL, mib->pts->mamsReceiverFn,
				tsif))
	{
		putSysErrmsg("amsd can't spawn CS tsif thread", NULL);
		return -1;
	}

	/*	Reload best current information about registrars'
	 *	locations.						*/

	reloadRsRegistrations(csState);

	/*	Start the configuration server heartbeat thread.	*/

	if (pthread_create(&csState->csHeartbeatThread, NULL, csHeartbeat,
				csState))
	{
		putSysErrmsg("can't spawn CS heartbeat thread", NULL);
		return -1;
	}

	/*	Start the configuration server main thread.		*/

	if (pthread_create(&(csState->csThread), NULL, csMain, csState))
	{
		putSysErrmsg("can't spawn configuration server thread", NULL);
		return -1;
	}

	/*	Configuration server is now running.			*/

	return 0;
}

static void	stopConfigServer(CsState *csState)
{
	if (csState->csRunning)
	{
		if (enqueueMamsCrash(csState->csEventsCV, "Stopped") < 0)
		{
			putErrmsg(NoMemoryMemo, NULL);
			cleanUpCsState(csState);
		}
		else
		{
			pthread_join(csState->csThread, NULL);
		}
	}
}

/*	*	*	Registrar code	*	*	*	*	*/

static int	sendMsgToCS(RsState *rsState, AmsEvt *evt)
{
	MamsMsg		*msg = (MamsMsg *) (evt->value);
	MamsEndpoint	*ep;
	int		result;

	if (rsState->csEndpoint)
	{
		result = sendMamsMsg(rsState->csEndpoint, &(rsState->tsif),
				msg->type, msg->memo, msg->supplementLength,
				msg->supplement);
	}
	else	/*	Not currently in contact with config server.	*/
	{
		if (lyst_length(mib->csEndpoints) == 0)
		{
			putErrmsg("Configuration server endpoints list empty.", 
					NULL);
			return -1;
		}

		if (rsState->csEndpointElt == NULL)
		{
			rsState->csEndpointElt = lyst_first(mib->csEndpoints);
		}
		else	/*	Let's try next csEndpoint in list.	*/
		{
			rsState->csEndpointElt =
					lyst_next(rsState->csEndpointElt);
			if (rsState->csEndpointElt == NULL)
			{
				rsState->csEndpointElt =
						lyst_first(mib->csEndpoints);
			}
		}

		ep = (MamsEndpoint *) lyst_data(rsState->csEndpointElt);
		result = sendMamsMsg(ep, &(rsState->tsif), msg->type, msg->memo,
				msg->supplementLength, msg->supplement);
	}

	if (msg->supplement)
	{
		MRELEASE(msg->supplement);
	}

	if (result < 0)
	{
		putErrmsg("RS failed sending message to CS", NULL);
	}

	return result;
}

static int	enqueueMsgToCS(RsState *rsState, MamsPduType msgType,
			signed int memo, unsigned short supplementLength,
			char *supplement)
{
	MamsMsg	msg;
	AmsEvt	*evt;

	memset((char *) &msg, 0, sizeof msg);
	msg.type = msgType;
	msg.memo = memo;
	msg.supplementLength = supplementLength;
	msg.supplement = supplement;
	evt = (AmsEvt *) MTAKE(1 + sizeof(MamsMsg));
	if (evt == NULL)
	{
		putSysErrmsg(NoMemoryMemo, NULL);
		return -1;
	}

	memcpy(evt->value, (char *) &msg, sizeof msg);
	evt->type = MSG_TO_SEND_EVT;
	if (enqueueMamsEvent(rsState->rsEventsCV, evt, NULL, 0))
	{
		MRELEASE(evt);
		putSysErrmsg(NoMemoryMemo, NULL);
		return -1;
	}

	return 0;
}

static int	forwardMsg(RsState *rsState, MamsPduType msgType,
			int roleNbr, int unitNbr, int nodeNbr,
			unsigned short supplementLength, char *supplement)
{
	signed int	nodeId;
	int		i;
	Cell		*cell;
	Node		*node;
	int		result = 0;

	nodeId = computeNodeId(roleNbr, unitNbr, nodeNbr);
	cell = rsState->cell;
	for (i = 1; i <= MaxNodeNbr; i++)
	{
		if (i == nodeNbr && unitNbr == rsState->cell->unit->nbr)
		{
			continue;	/*	Don't echo to source.	*/
		}

		node = cell->nodes[i];
		if (node->role == NULL)
		{
			continue;	/*	No such node.		*/
		}

		result = sendMamsMsg(&(node->mamsEndpoint), &(rsState->tsif),
				msgType, nodeId, supplementLength, supplement);
		if (result < 0)
		{
			break;
		}
	}

	return 0;
}

static int	propagateMsg(RsState *rsState, MamsPduType msgType,
			int roleNbr, int unitNbr, int nodeNbr,
			unsigned short supplementLength, char *supplement)
{
	signed int	nodeId;
	int		i;
	Unit		*unit;
	Cell		*cell;
	int		result;

	if (forwardMsg(rsState, msgType, roleNbr, unitNbr, nodeNbr,
			supplementLength, supplement))
	{
		return -1;
	}

	nodeId = computeNodeId(roleNbr, unitNbr, nodeNbr);
	for (i = 0; i <= MaxUnitNbr; i++)
	{
		if (i == rsState->cell->unit->nbr)	/*	Self.	*/
		{
			continue;
		}

		unit = rsState->venture->units[i];
		if (unit == NULL
		|| (cell = unit->cell)->mamsEndpoint.ept == NULL)
		{
			continue;
		}

		result = sendMamsMsg(&(cell->mamsEndpoint), &(rsState->tsif),
				msgType, nodeId, supplementLength, supplement);
		if (result < 0)
		{
			break;
		}
	}

	return 0;
}

static int	resyncCell(RsState *rsState)
{
	int		nodeCount = 0;
	int		i;
	Node		*node;
	unsigned char	nodeLyst[MAX_NODE_NBR + 1];
	int		nodeLystLength;

	/*	Construct list of currently registered nodes.		*/

	for (i = 1; i <= MaxNodeNbr; i++)
	{
		node = rsState->cell->nodes[i];
		if (node->role == NULL)
		{
			continue;	/*	No such node.		*/
		}

		nodeCount++;
		nodeLyst[nodeCount] = i;
	}

	nodeLyst[0] = nodeCount;
	nodeLystLength = nodeCount + 1;

	/*	Tell everybody in venture about state of own cell.	*/

	if (propagateMsg(rsState, cell_status, 0, rsState->cell->unit->nbr, 0,
			nodeLystLength, (char *) nodeLyst))
	{
		putErrmsg("RS can't propagate cell_status.", NULL);
	}

	return 0;
}

static void	processHeartbeatCycle(RsState *rsState, int *cycleCount,
			int *beatsSinceResync)
{
	int	i;
	Node	*node;

	/*	Send heartbeats to nodes as necessary.			*/

	if (*cycleCount > 1)	/*	Every 20 seconds.		*/
	{
		*cycleCount = 0;

		/*	Registrar's census clock starts upon initial
		 *	contact with configuration server.		*/

		if (rsState->csEndpoint != NULL || rsState->cellHeartbeats > 0)
		{
			rsState->cellHeartbeats++;
		}

		/*	Send heartbeats to all nodes in own cell.	*/

		for (i = 1; i <= MaxNodeNbr; i++)
		{
			node = rsState->cell->nodes[i];
			if (node->role == NULL)
			{
				continue;
			}

			if (node->heartbeatsMissed == 3)
			{
				if (sendMamsMsg(&(node->mamsEndpoint),
					&(rsState->tsif), you_are_dead,
					0, 0, NULL))
				{
					putErrmsg("RS can't send imputed \
termination to dead node.", NULL);
				}

				if (propagateMsg(rsState, I_am_stopping,
					node->role->nbr,
					rsState->cell->unit->nbr, i, 0, NULL))
				{
					putErrmsg("RS can't send imputed \
termination to peer nodes.", NULL);
				}

				forgetNode(node);
			}
			else if (node->heartbeatsMissed < 3)
			{
				if (sendMamsMsg(&(node->mamsEndpoint),
					&(rsState->tsif), heartbeat,
					0, 0, NULL) < 0)
				{
					putErrmsg("RS can't send heartbeat.",
							NULL);
				}
			}

			node->heartbeatsMissed++;
		}
	}

	/*	Resync as necessary.				*/

	if (rsState->cell->resyncPeriod > 0)
	{
		(*beatsSinceResync)++;
		if (*beatsSinceResync == rsState->cell->resyncPeriod)
		{
			resyncCell(rsState);
			*beatsSinceResync = 0;
		}
	}
}

static void	*rsHeartbeat(void *parm)
{
	RsState		*rsState = (RsState *) parm;
	int		cycleCount = 0;
	pthread_mutex_t	mutex;
	pthread_cond_t	cv;
	sigset_t	signals;
	int		supplementLen;
	char		*ept;
	int		beatsSinceResync = -1;
	struct timeval	workTime;
	struct timespec	deadline;
	int		result;

	if (pthread_mutex_init(&mutex, NULL))
	{
		putSysErrmsg("can't start heartbeat, mutex init failed", NULL);
		return NULL;
	}

	if (pthread_cond_init(&cv, NULL))
	{
		pthread_mutex_destroy(&mutex);
		putSysErrmsg("can't start heartbeat, cond init failed", NULL);
		return NULL;
	}

	sigfillset(&signals);
	pthread_sigmask(SIG_BLOCK, &signals, NULL);
	while (1)		/*	Every 10 seconds.		*/
	{
		LOCK_MIB;

		/*	Send heartbeat to configuration server.		*/

		if (rsState->heartbeatsMissed == 3)
		{
			rsState->csEndpoint = NULL;
		}

		if (rsState->csEndpoint)	/*	Send heartbeat.	*/
		{
			enqueueMsgToCS(rsState, heartbeat, 0, 0, NULL);
		}
		else	/*	Try to reconnect to config server.	*/
		{
			supplementLen = strlen(rsState->tsif.ept) + 1;
			ept = MTAKE(supplementLen);
			if (ept == NULL)
			{
				UNLOCK_MIB;
				putErrmsg(NoMemoryMemo, NULL);
				return NULL;
			}

			istrcpy(ept, rsState->tsif.ept, supplementLen);
			enqueueMsgToCS(rsState, announce_registrar, 0,
					supplementLen, ept);
		}

		rsState->heartbeatsMissed++;

		/*	Send heartbeats to all nodes in cell; resync.	*/

		processHeartbeatCycle(rsState, &cycleCount, &beatsSinceResync);
		UNLOCK_MIB;

		/*	Sleep for N3_INTERVAL seconds and repeat.	*/

		getCurrentTime(&workTime);
		deadline.tv_sec = workTime.tv_sec + N3_INTERVAL;
		deadline.tv_nsec = workTime.tv_usec * 1000;
		pthread_mutex_lock(&mutex);
		result = pthread_cond_timedwait(&cv, &mutex, &deadline);
		pthread_mutex_unlock(&mutex);
		if (result)
		{
			errno = result;
			if (errno != ETIMEDOUT)
			{
				putSysErrmsg("heartbeat thread failure", NULL);
				break;
			}
		}

		cycleCount++;
	}

	putErrmsg("RS heartbeat thread ended", NULL);
	return NULL;
}

static void	cleanUpRsState(RsState *rsState)
{
	if (rsState->tsif.ts)
	{
		rsState->tsif.ts->shutdownFn(rsState->tsif.sap);
	}

	if (rsState->rsEvents)
	{
		lyst_destroy(rsState->rsEvents);
		rsState->rsEvents = NULL;
	}

	llcv_close(rsState->rsEventsCV);
	rsState->rsRunning = 0;
}

static int	skipDeliveryVector(int *bytesRemaining, char **cursor)
{
	unsigned char	u1;
	int		len;

	if (*bytesRemaining < 1)
	{
		return -1;
	}

	u1 = **cursor;
	(*cursor)++;
	(*bytesRemaining)--;
	if (parseString(cursor, bytesRemaining, &len) < 0)
	{
		return -1;
	}

	return 0;
}

static int	skipDeliveryVectorList(int *bytesRemaining, char **cursor)
{
	int	vectorCount;

	if (*bytesRemaining < 1)
	{
		return -1;
	}

	vectorCount = (unsigned char) **cursor;
	(*cursor)++;
	(*bytesRemaining)--;
	while (vectorCount > 0)
	{
		if (skipDeliveryVector(bytesRemaining, cursor))
		{
			return -1;
		}

		vectorCount--;
	}

	return 0;
}

static int	skipDeclaration(int *bytesRemaining, char **cursor)
{
	int	listLength;

	/*	First skip over the subscriptions list.			*/

	if (*bytesRemaining < 2)
	{
		return -1;
	}

	listLength = (((unsigned char) **cursor) << 8) & 0xff00;
	(*cursor)++;
	listLength += (unsigned char) **cursor;
	(*cursor)++;
	(*bytesRemaining) -= 2;
	while (listLength > 0)
	{
		if (*bytesRemaining < SUBSCRIBE_LEN)
		{
			return -1;
		}

		(*cursor) += SUBSCRIBE_LEN;
		(*bytesRemaining) -= SUBSCRIBE_LEN;
		listLength--;
	}

	/*	Now skip over the invitations list.			*/

	if (*bytesRemaining < 2)
	{
		return -1;
	}

	listLength = (((unsigned char) **cursor) << 8) & 0xff00;
	(*cursor)++;
	listLength += (unsigned char) **cursor;
	(*cursor)++;
	(*bytesRemaining) -= 2;
	while (listLength > 0)
	{
		if (*bytesRemaining < INVITE_LEN)
		{
			return -1;
		}

		(*cursor) += INVITE_LEN;
		(*bytesRemaining) -= INVITE_LEN;
		listLength--;
	}

	return 0;
}

static void	processMsgToRs(RsState *rsState, AmsEvt *evt)
{
	MamsMsg		*msg = (MamsMsg *) (evt->value);
	Venture		*venture;
	Unit		*unit;
	Cell		*cell;
	Node		*node;
	int		unitNbr;
	int		i;
	int		nodeNbr;
	int		roleNbr;
	AppRole		*role;
	int		nodeCount;
	char		*ept;
	int		eptLength;
	MamsEndpoint	endpoint;
	int		supplementLength;
	char		*supplement;
	char		reasonCode;
	char		*reasonString;
	int		result;
	char		*cursor;
	int		bytesRemaining;

PUTMEMO("RS got msg of type", itoa(msg->type));
PUTMEMO("...from role", itoa(msg->roleNbr));
	venture = mib->ventures[msg->ventureNbr];
	if (venture == NULL)
	{
		unit = NULL;
	}
	else
	{
		unit = venture->units[msg->unitNbr];
	}

	switch (msg->type)
	{
	case heartbeat:
		if (msg->ventureNbr == 0)	/*	From CS.	*/
		{
			rsState->heartbeatsMissed = 0;
			return;
		}

		/*	Heartbeat from a node.				*/

		if (unit == NULL || msg->memo < 1 || msg->memo > MaxNodeNbr
		|| (node = unit->cell->nodes[msg->memo]) == NULL
		|| node->role == NULL)
		{
			return;	/*	Ignore invalid heartbeat.	*/
		}

		/*	Legitimate heartbeat from registered node.	*/

		node->heartbeatsMissed = 0;
		return;

	case rejection:
		if (rsState->csEndpoint != NULL)
		{
			return;	/*	Ignore spurious rejection.	*/
		}

		/*	Rejected on attempt to announce self to the
		 *	configuration server.				*/

		reasonCode = *(msg->supplement);
		switch (reasonCode)
		{
		case REJ_DUPLICATE:
			reasonString = "Duplicate";
			break;

		case REJ_NO_UNIT:
			reasonString = "No such unit";
			break;

		default:
			reasonString = "Reason unknown";
		}

		if (enqueueMamsCrash(rsState->rsEventsCV, reasonString) < 0)
		{
			putErrmsg(NoMemoryMemo, NULL);
		}

		return;

	case registrar_noted:
		rsState->heartbeatsMissed = 0;
		rsState->csEndpoint =
			(MamsEndpoint *) lyst_data(rsState->csEndpointElt);
		rsState->csEndpointElt = NULL;
		return;

	case cell_spec:
		if (msg->supplementLength < 3)
		{
			putErrmsg("Cell spec lacks endpoint name.", NULL);
			return;
		}
		
		unitNbr = (((unsigned char) (msg->supplement[0])) << 8) +
				((unsigned char) (msg->supplement[1]));
		if (unitNbr == rsState->cell->unit->nbr)
		{
			return;		/*	Notification of self.	*/
		}

		cursor = msg->supplement + 2;
		bytesRemaining = msg->supplementLength - 2;
		ept = parseString(&cursor, &bytesRemaining, &eptLength);
		if (ept == NULL)
		{
			putErrmsg("Cell spec endpoint name invalid.", NULL);
			return;
		}

		/*	Cell spec for remote registrar.			*/

		unit = rsState->venture->units[unitNbr];
		if (unit == NULL)
		{
			return;		/*	Ignore invalid sender.	*/
		}

		cell = unit->cell;
		if (cell->mamsEndpoint.ept != NULL)
		{
			if (strcmp(ept, cell->mamsEndpoint.ept) == 0)
			{
				/*	Redundant information.		*/

				return;
			}

			putErrmsg("Got revised registrar spec; accepting it.",
					itoa(unitNbr));
			clearMamsEndpoint(&(cell->mamsEndpoint));
		}

		if (constructMamsEndpoint(&(cell->mamsEndpoint), eptLength,
					ept) < 0)
		{
			clearMamsEndpoint(&(cell->mamsEndpoint));
			putErrmsg("Can't load spec for cell.", NULL);
		}

		return;

	case node_registration:

		/*	Parse node's MAMS endpoint in case it's
		 *	needed for an echo message.			*/

		cursor = msg->supplement;
		bytesRemaining = msg->supplementLength;
		ept = parseString(&cursor, &bytesRemaining, &eptLength);
		if (ept == NULL)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		if (skipDeliveryVectorList(&bytesRemaining, &cursor) < 0)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		endpoint.ept = ept;
		if (((mib->pts->parseMamsEndpointFn)(&endpoint)) < 0)
		{
			return;		/*	Can't respond.		*/
		}

		if (rsState->cellHeartbeats < 4)
		{
			reasonCode = REJ_NO_CENSUS;
			if (sendMamsMsg(&endpoint, &(rsState->tsif), rejection,
					msg->memo, 1, &reasonCode) < 0)
			{
				putErrmsg("RS can't reject MAMS msg", NULL);
			}

			return;
		}

		nodeNbr = nodeCount = 0;
		for (i = 1; i <= MaxNodeNbr; i++)
		{
			node = rsState->cell->nodes[i];
			if (node->role == NULL)
			{
				/*	This is an unused node #.	*/

				if (nodeNbr == 0)
				{
					nodeNbr = i;
				}
			}
			else
			{
				nodeCount++;
			}
		}

		if (nodeNbr == 0)	/*	Cell already full.	*/
		{
			reasonCode = REJ_CELL_FULL;
			if (sendMamsMsg(&endpoint, &(rsState->tsif), rejection,
					msg->memo, 1, &reasonCode) < 0)
			{
				putErrmsg("RS can't reject MAMS msg.", NULL);
			}

			return;
		}

		node = rsState->cell->nodes[nodeNbr];
		roleNbr = msg->roleNbr;
		role = rsState->venture->roles[roleNbr];
		if (rememberNode(node, role, eptLength, ept))
		{
			putErrmsg("RS can't register new node.", NULL);
			return;
		}

		supplementLength = 1;
		supplement = MTAKE(supplementLength);
		if (supplement == NULL)
		{
			forgetNode(node);
			putSysErrmsg(NoMemoryMemo, NULL);
			return;
		}

		*supplement = nodeNbr;
		result = sendMamsMsg(&endpoint, &(rsState->tsif), you_are_in,
				msg->memo, supplementLength, supplement);
		MRELEASE(supplement);
		if (result < 0)
		{
			forgetNode(node);
			putErrmsg("RS can't accept node registration.", NULL);
			return;
		}

		if (propagateMsg(rsState, I_am_starting, roleNbr,
				rsState->cell->unit->nbr, nodeNbr,
				msg->supplementLength, msg->supplement))
		{
			putErrmsg("RS can't advertise new node.", NULL);
		}

		return;

	case I_am_stopping:
		if (parseNodeId(msg->memo, &roleNbr, &unitNbr, &nodeNbr) < 0)
		{
			putErrmsg("RS ditching I_am_stoppng", NULL);
			return;
		}

		if (unitNbr == rsState->cell->unit->nbr)
		{
			/*	Message from node in own cell.		*/

			node = rsState->cell->nodes[nodeNbr];
			forgetNode(node);
			if (propagateMsg(rsState, I_am_stopping, roleNbr,
					unitNbr, nodeNbr, msg->supplementLength,
					msg->supplement))
			{
				putErrmsg("RS can't propagate I_am_stopping",
						NULL);
			}
		}
		else	/*	Message from registrar of another cell.	*/
		{
			if (forwardMsg(rsState, I_am_stopping, roleNbr, unitNbr,
					nodeNbr, msg->supplementLength,
					msg->supplement))
			{
				putErrmsg("RS can't forward I_am_stopping",
						NULL);
			}
		}

		return;

	case reconnect:
		if (msg->supplementLength < 4)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		nodeNbr = (unsigned char) *(msg->supplement + 2);
		cursor = msg->supplement + 4;
		bytesRemaining = msg->supplementLength - 4;
		ept = parseString(&cursor, &bytesRemaining, &eptLength);
		if (ept == NULL)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		endpoint.ept = ept;
		if (((mib->pts->parseMamsEndpointFn)(&endpoint)) < 0)
		{
			return;		/*	Can't respond.		*/
		}

		if (rsState->cellHeartbeats > 3)
		{
			if (sendMamsMsg(&endpoint, &(rsState->tsif),
					you_are_dead, 0, 0, NULL) < 0)
			{
				putErrmsg("RS can't ditch reconnect", NULL);
			}

			return;
		}

		/*	Registrar is still new enough not to know
		 *	the complete composition of the cell.		*/

		if (skipDeliveryVectorList(&bytesRemaining, &cursor) < 0)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		if (skipDeclaration(&bytesRemaining, &cursor) < 0)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		if (bytesRemaining == 0)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		nodeCount = (unsigned char) *cursor;
		cursor++;
		bytesRemaining--;
		if (bytesRemaining != nodeCount)
		{
			return;		/*	Ignore malformed msg.	*/
		}

		/*	Message correctly formed, reconnection okay.	*/

		if (rsState->undeclaredNodesCount > 0
		&& rsState->undeclaredNodes[nodeNbr] == 0)
		{
			/*	This node was not in the census
			 *	declared by the first reconnected node.	*/

			if (sendMamsMsg(&endpoint, &rsState->tsif,
					you_are_dead, 0, 0, NULL) < 0)
			{
				putErrmsg("RS can't ditch reconnect", NULL);
			}

			return;
		}

		node = rsState->cell->nodes[nodeNbr];
		if (node->role)
		{
			/*	Another node identifying itself by
			 *	the same node number has already
			 *	reconnected.				*/

			if (sendMamsMsg(&endpoint, &(rsState->tsif),
					you_are_dead, 0, 0, NULL) < 0)
			{
				putErrmsg("RS can't ditch reconnect", NULL);
			}

			return;
		}

		roleNbr = msg->roleNbr;
		role = rsState->venture->roles[roleNbr];
		result = rememberNode(node, role, eptLength, ept);
		if (result < 0)
		{
			putErrmsg("RS can't reconnect node", NULL);
			return;
		}

		if (rsState->undeclaredNodesCount == 0)
		{
			/*	This is the first reconnect sent to
			 *	the resurrected registrar; load list
			 *	of nodes to expect reconnects from.	*/

			rsState->undeclaredNodesCount = nodeCount;
			while (bytesRemaining > 0)
			{
				i = *cursor;
				if (i > 0 && i <= MaxNodeNbr)
				{
					rsState->undeclaredNodes[i] = 1;
				}

				bytesRemaining--;
				cursor++;
			}
		}

		/*	Scratch node off the undeclared nodes list.	*/

		if (rsState->undeclaredNodes[nodeNbr] == 1)
		{
			rsState->undeclaredNodes[nodeNbr] = 0;
			rsState->undeclaredNodesCount--;
		}

		/*	Let node go about its business.			*/

		if (sendMamsMsg(&endpoint, &(rsState->tsif), reconnected,
				msg->memo, 0, NULL) < 0)
		{
			putErrmsg("RS can't acknowledge MAMS msg", NULL);
		}

		return;

	case subscribe:
	case unsubscribe:
	case invite:
	case disinvite:
	case node_status:
		if (parseNodeId(msg->memo, &roleNbr, &unitNbr, &nodeNbr) < 0)
		{
			putErrmsg("RS ditching MAMS propagation", NULL);
			return;
		}

		if (unitNbr == rsState->cell->unit->nbr)
		{
			/*	Message from node in own cell.		*/

			if (propagateMsg(rsState, msg->type, roleNbr, unitNbr,
					nodeNbr, msg->supplementLength,
					msg->supplement))
			{
				putErrmsg("RS can't propagate message",
						NULL);
			}
		}
		else	/*	Message from registrar of another cell.	*/
		{
			if (rsState->cell->resyncPeriod > 0)
			{
				if (forwardMsg(rsState, msg->type, roleNbr,
					unitNbr, nodeNbr, msg->supplementLength,
					msg->supplement))
				{
					putErrmsg("RS can't forward message",
							NULL);
				}
			}
		}

		return;

	case cell_status:	/*	From registrar of some cell.	*/
		if (rsState->cell->resyncPeriod > 0)
		{
			if (forwardMsg(rsState, cell_status, 0, msg->unitNbr,
					0, msg->supplementLength,
					msg->supplement))
			{
				putErrmsg("RS can't forward message", NULL);
			}
		}

		return;

	default:		/*	Inapplicable message; ignore.	*/
		return;
	}
}

static void	*rsMain(void *parm)
{
	RsState		*rsState = (RsState *) parm;
	sigset_t	signals;
	LystElt		elt;
	AmsEvt		*evt;
	int		result;

	sigfillset(&signals);
	pthread_sigmask(SIG_BLOCK, &signals, NULL);
	rsState->rsRunning = 1;
PUTS("Registrar is running.");
	while (1)
	{
		if (llcv_wait(rsState->rsEventsCV, llcv_lyst_not_empty,
					LLCV_BLOCKING) < 0)
		{
			putSysErrmsg("RS thread failed getting event", NULL);
			break;
		}

		llcv_lock(rsState->rsEventsCV);
		elt = lyst_first(rsState->rsEvents);
		if (elt == NULL)
		{
			llcv_unlock(rsState->rsEventsCV);
			continue;
		}

		evt = (AmsEvt *) lyst_data_set(elt, NULL);
		lyst_delete(elt);
		llcv_unlock(rsState->rsEventsCV);
		switch (evt->type)
		{
		case MAMS_MSG_EVT:
			LOCK_MIB;
			processMsgToRs(rsState, evt);
			UNLOCK_MIB;
			recycleEvent(evt);
			continue;

		case MSG_TO_SEND_EVT:
			LOCK_MIB;
			result = sendMsgToCS(rsState, evt);
			UNLOCK_MIB;
			if (result < 0)
			{
				putErrmsg("Registrar CS contact failed.", NULL);
			}

			recycleEvent(evt);
			continue;

		case CRASH_EVT:
			putErrmsg("RS thread terminated", evt->value);
			recycleEvent(evt);
			break;	/*	Out of switch.			*/

		default:	/*	Inapplicable event; ignore.	*/
			recycleEvent(evt);
			continue;
		}

		break;		/*	Out of loop.			*/
	}

	/*	Operation of the registrar is terminated.		*/

	writeErrmsgMemos();
	pthread_cancel(rsState->rsHeartbeatThread);
	pthread_join(rsState->rsHeartbeatThread, NULL);
	cleanUpRsState(rsState);
	return NULL;
}

static int	startRegistrar(RsState *rsState)
{
	int		i;
	Venture		*venture = NULL;
	Unit		*unit = NULL;
	char		ventureName[MAX_APP_NAME + 2 + MAX_AUTH_NAME + 1];
	MamsInterface	*tsif;

	if (rsState->rsAppName == NULL || strlen(rsState->rsAppName) == 0
	|| rsState->rsAuthName == NULL || strlen(rsState->rsAuthName) == 0
	|| rsState->rsUnitName == NULL)
	{
		putErrmsg(BadParmsMemo, NULL);
		errno = EINVAL;
		return -1;
	}

	for (i = 1; i <= MaxVentureNbr; i++)
	{
		venture = mib->ventures[i];
		if (venture == NULL)	/*	Number not in use.	*/
		{
			continue;
		}

		if (strcmp(venture->app->name, rsState->rsAppName) == 0
		&& strcmp(venture->authorityName, rsState->rsAuthName) == 0)
		{
			break;
		}
	}

	if (i > MaxVentureNbr)
	{
		isprintf(ventureName, sizeof ventureName, "%s(%s)",
				rsState->rsAppName, rsState->rsAuthName);
		putErrmsg("Can't start registrar: no such message space.",
				ventureName);
		errno = EINVAL;
		return -1;
	}

	rsState->venture = venture;
	for (i = 0; i <= MaxUnitNbr; i++)
	{
		unit = venture->units[i];
		if (unit == NULL)	/*	Number not in use.	*/
		{
			continue;
		}

		if (strcmp(unit->name, rsState->rsUnitName) == 0)
		{
			break;
		}
	}

	if (i > MaxUnitNbr)
	{
		putErrmsg("Can't start registrar: no such unit.",
				rsState->rsUnitName);
		errno = EINVAL;
		return -1;
	}

	rsState->cell = unit->cell;

	/*	Load the necessary state data structures.		*/

	rsState->rsEvents = lyst_create_using(amsMemory);
	if (rsState->rsEvents == NULL)
	{
		putSysErrmsg(NoMemoryMemo, NULL);
		return -1;
	}

	lyst_delete_set(rsState->rsEvents, destroyEvent, NULL);
	rsState->rsEventsCV = llcv_open(rsState->rsEvents,
			&(rsState->rsEventsCV_str));
	if (rsState->rsEventsCV == NULL)
	{
		putSysErrmsg("amsd can't open RS events llcv", NULL);
		return -1;
	}

	/*	Initialize the MAMS transport service interface.	*/

	tsif = &(rsState->tsif);
	tsif->ts = mib->pts;
	tsif->ventureNbr = rsState->venture->nbr;
	tsif->unitNbr = rsState->cell->unit->nbr;
	tsif->endpointSpec = NULL;
	tsif->eventsQueue = rsState->rsEventsCV;
	if (tsif->ts->mamsInitFn(tsif) < 0)
	{
		putErrmsg("amsd can't initialize RS MAMS interface", NULL);
		return -1;
	}

	/*	Start the MAMS transport service receiver thread.	*/

	if (pthread_create(&(tsif->receiver), NULL, mib->pts->mamsReceiverFn,
				tsif))
	{
		putSysErrmsg("amsd can't spawn RS tsif thread", NULL);
		return -1;
	}

	/*	Start the registrar heartbeat thread.			*/

	if (pthread_create(&rsState->rsHeartbeatThread, NULL, rsHeartbeat,
				rsState))
	{
		putSysErrmsg("can't spawn RS heartbeat thread", NULL);
		return -1;
	}

	/*	Start the registrar main thread.			*/

	if (pthread_create(&(rsState->rsThread), NULL, rsMain, rsState))
	{
		putSysErrmsg("can't spawn registrar thread", NULL);
		return -1;
	}

	/*	Registrar is now running.				*/

	return 0;
}

static void	stopRegistrar(RsState *rsState)
{
	if (rsState->rsRunning)
	{
		if (enqueueMamsCrash(rsState->rsEventsCV, "Stopped") < 0)
		{
			putErrmsg(NoMemoryMemo, NULL);
			cleanUpRsState(rsState);
		}
		else
		{
			pthread_join(rsState->rsThread, NULL);
		}
	}
}

/*	*	*	AMSD code	*	*	*	*	*/

static int	run_amsd(char *mibSource, char *mName, char *memory,
			unsigned int mSize, char *csEndpointSpec,
			char *rsAppName, char *rsAuthName, char *rsUnitName)
{
	int		result;
	char		ownHostName[MAXHOSTNAMELEN + 1];
	char		eps[MAXHOSTNAMELEN + 5 + 1];
	CsState		csState;
	RsState		rsState;

	/*	Apply defaults as necessary.				*/

	if (strcmp(mibSource, "@") == 0)
	{
		mibSource = NULL;
	}

	if (strcmp(mName, "@") == 0)
	{
		mName = NULL;
	}

	if (strcmp(csEndpointSpec, "@") == 0)
	{
		getNameOfHost(ownHostName, sizeof ownHostName);
		isprintf(eps, sizeof eps, "%s:2357", ownHostName);
		csEndpointSpec = eps;
	}

	if (strcmp(csEndpointSpec, ".") == 0)
	{
		csEndpointSpec = NULL;
	}

	/*	Initialize dynamic memory management.			*/

	if (initMemoryMgt(mName, memory, mSize) < 0)
	{
		return -1;
	}

	/*	Load Management Information Base as necessary.		*/

	if (mib == NULL)
	{
		result = loadMib(mibSource);
		if (result < 0 || mib == NULL)
		{
			putErrmsg("amsd can't load MIB", mibSource);
			return -1;
		}
	}

	memset((char *) &csState, 0, sizeof csState);
	csState.csEndpointSpec = csEndpointSpec;
	if (csEndpointSpec)
	{
		csState.csRequired = 1;
	}

	memset((char *) &rsState, 0, sizeof rsState);
	rsState.rsAppName = rsAppName;
	rsState.rsAuthName = rsAuthName;
	rsState.rsUnitName = rsUnitName;
	if (rsAppName)
	{
		rsState.rsRequired = 1;
	}

	sm_TaskVarAdd(&amsdRunning);
	amsdRunning = 1;
	signal(SIGINT, shutDownAmsd);
	while (1)
	{
		if (amsdRunning == 0)
		{
			stopConfigServer(&csState);
			stopRegistrar(&rsState);
			return 0;
		}

		if (csState.csRequired == 1 && csState.csRunning == 0)
		{
			writeMemo("Starting configuration server.");
			if (startConfigServer(&csState) < 0)
			{
				cleanUpCsState(&csState);
				putErrmsg("amsd can't start CS", NULL);
			}
		}

		if (rsState.rsRequired == 1 && rsState.rsRunning == 0)
		{
			writeMemo("Starting registration server.");
			if (startRegistrar(&rsState) < 0)
			{
				cleanUpRsState(&rsState);
				putErrmsg("amsd can't start RS", NULL);
			}
		}

		snooze(N5_INTERVAL);
	}
}

#if defined (VXWORKS) || defined (RTEMS)
int	amsd(int a1, int a2, int a3, int a4, int a5,
		int a6, int a7, int a8, int a9, int a10)
{
	char		*mibSource = (char *) a1;
	char		*mName = (char *) a2;
	char		*memory = (char *) a3;
	unsigned int	mSize = a4;
	char		*csEndpointSpec = (char *) a5;
	char		*rsAppName = (char *) a6;
	char		*rsAuthName = (char *) a7;
	char		*rsUnitName = (char *) a8;
	int		result;
#else
int	main(int argc, char *argv[])
{
	char		*mibSource;
	char		*mName;
	char		*memory;
	unsigned int	mSize;
	char		*csEndpointSpec;
	char		*rsAppName = NULL;
	char		*rsAuthName = NULL;
	char		*rsUnitName = NULL;
	int		result;

	if (argc != 6 && argc != 9)
	{
		PUTS("Usage:  amsd { @ | <MIB source name> }");
		PUTS("             { @ | <memory manager name> }");
		PUTS("             { 0 | <memory address> }");
		PUTS("             { 0 | <memory size> }");
		PUTS("             { . | @ | <config. server endpoint spec> }");
		PUTS("             [<registrar application name>");
		PUTS("              <registrar authority name>");
		PUTS("              <registrar unit name>]");
		return 0;
	}

	mibSource = argv[1];
	mName = argv[2];
	if (strcmp(argv[3], "0") == 0)
	{
		memory = NULL;
	}
	else
	{
		memory = (char *) strtoul(argv[3], (char **) NULL, 0);
	}

	mSize = strtoul(argv[4], (char **) NULL, 0);
	csEndpointSpec = argv[5];
	if (argc > 6)
	{
		rsAppName = argv[6];
		rsAuthName = argv[7];
		rsUnitName = argv[8];
	}
#endif
	result = run_amsd(mibSource, mName, memory, mSize, csEndpointSpec,
			rsAppName, rsAuthName, rsUnitName);
	writeErrmsgMemos();
	return result;
}
