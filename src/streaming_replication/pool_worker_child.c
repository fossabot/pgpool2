/* -*-pgsql-c-*- */
/*
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2019	PgPool Global Development Group
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * child.c: worker child process main
 *
 */
#include "config.h"


#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <signal.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "pool.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/elog.h"

#include "context/pool_process_context.h"
#include "context/pool_session_context.h"
#include "pool_config.h"
#include "utils/pool_ip.h"
#include "auth/md5.h"
#include "auth/pool_hba.h"
#include "utils/pool_stream.h"

char		remote_ps_data[NI_MAXHOST]; /* used for set_ps_display */
static POOL_CONNECTION_POOL_SLOT * slots[MAX_NUM_BACKENDS];
static volatile sig_atomic_t reload_config_request = 0;
static volatile sig_atomic_t restart_request = 0;

static void establish_persistent_connection(void);
static void discard_persistent_connection(void);
static void check_replication_time_lag(void);
static void CheckReplicationTimeLagErrorCb(void *arg);
static unsigned long long int text_to_lsn(char *text);
static RETSIGTYPE my_signal_handler(int sig);
static RETSIGTYPE reload_config_handler(int sig);
static void reload_config(void);

#define CHECK_REQUEST \
	do { \
		if (reload_config_request) \
		{ \
			reload_config(); \
			reload_config_request = 0; \
		} else if (restart_request) \
		{ \
		  ereport(LOG,(errmsg("worker process received restart request"))); \
		  exit(1); \
		} \
    } while (0)


#define PG10_SERVER_VERSION	100000	/* PostgreSQL 10 server version num */
#define PG91_SERVER_VERSION	90100	/* PostgreSQL 9.1 server version num */

/*
* worker child main loop
*/
void
do_worker_child(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext WorkerMemoryContext;

	ereport(DEBUG1,
			(errmsg("I am %d", getpid())));

	/* Identify myself via ps */
	init_ps_display("", "", "", "");
	set_ps_display("worker process", false);

	/* set up signal handlers */
	signal(SIGALRM, SIG_DFL);
	signal(SIGTERM, my_signal_handler);
	signal(SIGINT, my_signal_handler);
	signal(SIGHUP, reload_config_handler);
	signal(SIGQUIT, my_signal_handler);
	signal(SIGCHLD, SIG_IGN);
	signal(SIGUSR1, my_signal_handler);
	signal(SIGUSR2, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	/* Create per loop iteration memory context */
	WorkerMemoryContext = AllocSetContextCreate(TopMemoryContext,
												"Worker_main_loop",
												ALLOCSET_DEFAULT_MINSIZE,
												ALLOCSET_DEFAULT_INITSIZE,
												ALLOCSET_DEFAULT_MAXSIZE);

	MemoryContextSwitchTo(TopMemoryContext);

	/* Initialize my backend status */
	pool_initialize_private_backend_status();

	/* Initialize per process context */
	pool_init_process_context();

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		pool_signal(SIGALRM, SIG_IGN);
		error_context_stack = NULL;
		EmitErrorReport();
		MemoryContextSwitchTo(TopMemoryContext);
		FlushErrorState();
	}
	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;


	for (;;)
	{
		MemoryContextSwitchTo(WorkerMemoryContext);
		MemoryContextResetAndDeleteChildren(WorkerMemoryContext);

		CHECK_REQUEST;

		if (pool_config->sr_check_period <= 0)
		{
			sleep(30);
		}

		/*
		 * If streaming replication mode, do time lag checking
		 */

		if (pool_config->sr_check_period > 0 && STREAM)
		{
			establish_persistent_connection();
			PG_TRY();
			{
				POOL_NODE_STATUS *node_status;
				int			i;

				/* Do replication time lag checking */
				check_replication_time_lag();

				/* Check node status */
				node_status = verify_backend_node_status(slots);
				for (i = 0; i < NUM_BACKENDS; i++)
				{
					ereport(DEBUG1,
							(errmsg("node status[%d]: %d", i, node_status[i])));

					if (node_status[i] == POOL_NODE_STATUS_INVALID)
					{
						int			n;

						ereport(LOG,
								(errmsg("pgpool_worker_child: invalid node found %d", i)));
						if (pool_config->detach_false_primary)
						{
							n = i;
							degenerate_backend_set(&n, 1, REQ_DETAIL_SWITCHOVER | REQ_DETAIL_CONFIRMED);
						}
					}
				}
			}
			PG_CATCH();
			{
				discard_persistent_connection();
				sleep(pool_config->sr_check_period);
				PG_RE_THROW();
			}
			PG_END_TRY();

			/* Discard persistent connections */
			discard_persistent_connection();
		}
		sleep(pool_config->sr_check_period);
	}
	exit(0);
}

/*
 * Establish persistent connection to backend
 */
static void
establish_persistent_connection(void)
{
	int			i;
	BackendInfo *bkinfo;

	char	   *password = get_pgpool_config_user_password(pool_config->sr_check_user,
														   pool_config->sr_check_password);

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (!VALID_BACKEND(i))
			continue;

		if (slots[i] == NULL)
		{
			bkinfo = pool_get_node_info(i);
			slots[i] = make_persistent_db_connection_noerror(i, bkinfo->backend_hostname,
															 bkinfo->backend_port,
															 pool_config->sr_check_database,
															 pool_config->sr_check_user,
															 password ? password : "", true);
		}
	}

	if (password)
		pfree(password);
}

/*
 * Discard persistent connection to backend
 */
static void
discard_persistent_connection(void)
{
	int			i;

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (slots[i])
		{
			discard_persistent_db_connection(slots[i]);
			slots[i] = NULL;
		}
	}
}

/*
 * Check replication time lag
 */
static void
check_replication_time_lag(void)
{
	/* backend server version cache */
	static int	server_version[MAX_NUM_BACKENDS];

	int			i;
	POOL_SELECT_RESULT *res;
	POOL_SELECT_RESULT *res_rep;	/* query results of pg_stat_replication */
	unsigned long long int lsn[MAX_NUM_BACKENDS];
	char	   *query;
	char	   *stat_rep_query;
	BackendInfo *bkinfo;
	unsigned long long int lag;
	ErrorContextCallback callback;

	/* clear replication state */
	for (i = 0; i < NUM_BACKENDS; i++)
	{
		bkinfo = pool_get_node_info(i);

		*bkinfo->replication_state = '\0';
		*bkinfo->replication_sync_state = '\0';
	}

	if (NUM_BACKENDS <= 1)
	{
		/* If there's only one node, there's no point to do checking */
		return;
	}

	if (REAL_PRIMARY_NODE_ID < 0)
	{
		/* No need to check if there's no primary */
		return;
	}

	/*
	 * Register a error context callback to throw proper context message
	 */
	callback.callback = CheckReplicationTimeLagErrorCb;
	callback.arg = NULL;
	callback.previous = error_context_stack;
	error_context_stack = &callback;
	stat_rep_query = NULL;

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (!VALID_BACKEND(i))
			continue;

		if (!slots[i])
		{
			ereport(ERROR,
					(errmsg("Failed to check replication time lag"),
					 errdetail("No persistent db connection for the node %d", i),
					 errhint("check sr_check_user and sr_check_password")));

		}

		if (server_version[i] == 0)
		{
			query = "SELECT current_setting('server_version_num')";

			/*
			 * Get backend server version. If the query fails, keep previous
			 * info.
			 */
			if (get_query_result(slots, i, query, &res) == 0)
			{
				server_version[i] = atoi(res->data[0]);
				ereport(DEBUG1,
						(errmsg("backend %d server version: %d", i, server_version[i])));
			}

		}

		if (PRIMARY_NODE_ID == i)
		{
			if (server_version[i] >= PG10_SERVER_VERSION)
				query = "SELECT pg_current_wal_lsn()";
			else
				query = "SELECT pg_current_xlog_location()";

			if (server_version[i] == PG91_SERVER_VERSION)
				stat_rep_query = "SELECT application_name, state, '' AS sync_state FROM pg_stat_replication";
			else if (server_version[i] > PG91_SERVER_VERSION)
				stat_rep_query = "SELECT application_name, state, sync_state FROM pg_stat_replication";
		}
		else
		{
			if (server_version[i] >= PG10_SERVER_VERSION)
				query = "SELECT pg_last_wal_replay_lsn()";
			else
				query = "SELECT pg_last_xlog_replay_location()";
		}

		if (get_query_result(slots, i, query, &res) == 0 && res->nullflags[0] != -1)
		{
			lsn[i] = text_to_lsn(res->data[0]);
			free_select_result(res);
		}
		else
		{
			lsn[i] = 0;
		}
	}

	/*
	 * Call pg_stat_replication and fill the replication status.
	 */
	if (slots[PRIMARY_NODE_ID] && stat_rep_query != NULL)
	{
		int		status;

		status = get_query_result(slots, PRIMARY_NODE_ID, stat_rep_query, &res_rep);

		if (status != 0)
		{
			ereport(LOG,
					(errmsg("get_query_result falied: status: %d", status)));
		}

		for (i = 0; i < NUM_BACKENDS; i++)
		{
			bkinfo = pool_get_node_info(i);

			*bkinfo->replication_state = '\0';
			*bkinfo->replication_sync_state = '\0';

			if (i == PRIMARY_NODE_ID)
				continue;

			if (status == 0)
			{
				int		j;
				char	*s;

				for (j = 0; j < res_rep->numrows; j++)
				{
					if (strcmp(res_rep->data[j*3], bkinfo->backend_application_name) == 0)
					{
						/*
						 * If sr_check_user has enough privilege, it should return
						 * some string. If not, NULL pointer will be returned for
						 * res_rep->data[1] and [2]. So we need to prepare for the
						 * latter case.
						 */
						s = res_rep->data[j*3+1]? res_rep->data[j*3+1] : "";
						strlcpy(bkinfo->replication_state, s, NAMEDATALEN);
						s = res_rep->data[j*3+2]? res_rep->data[j*3+2] : "";
						strlcpy(bkinfo->replication_sync_state, s, NAMEDATALEN);
					}
				}
			}
		}
		if (status == 0)
			free_select_result(res_rep);
	}

	for (i = 0; i < NUM_BACKENDS; i++)
	{
		if (!VALID_BACKEND(i))
			continue;

		/* Set standby delay value */
		bkinfo = pool_get_node_info(i);
		lag = (lsn[PRIMARY_NODE_ID] > lsn[i]) ? lsn[PRIMARY_NODE_ID] - lsn[i] : 0;

		if (PRIMARY_NODE_ID == i)
		{
			bkinfo->standby_delay = 0;
		}
		else
		{
			bkinfo->standby_delay = lag;

			/* Log delay if necessary */
			if ((pool_config->log_standby_delay == LSD_ALWAYS && lag > 0) ||
				(pool_config->delay_threshold &&
				 pool_config->log_standby_delay == LSD_OVER_THRESHOLD &&
				 lag > pool_config->delay_threshold))
			{
				ereport(LOG,
						(errmsg("Replication of node:%d is behind %llu bytes from the primary server (node:%d)",
								i, lsn[PRIMARY_NODE_ID] - lsn[i], PRIMARY_NODE_ID)));
			}
		}
	}

	error_context_stack = callback.previous;
}

static void
CheckReplicationTimeLagErrorCb(void *arg)
{
	errcontext("while checking replication time lag");
}

/*
 * Convert logid/recoff style text to 64bit log location (LSN)
 */
static unsigned long long int
text_to_lsn(char *text)
{
/*
 * WAL segment size in bytes.  XXX We should fetch this from
 * PostgreSQL, rather than having fixed value.
 */
#define WALSEGMENTSIZE 16 * 1024 * 1024

	unsigned int xlogid;
	unsigned int xrecoff;
	unsigned long long int lsn;

	if (sscanf(text, "%X/%X", &xlogid, &xrecoff) !=2)
	{
		ereport(ERROR,
				(errmsg("invalid LSN format"),
				 errdetail("wrong log location format: %s", text)));

	}
	lsn = xlogid * ((unsigned long long int) 0xffffffff - WALSEGMENTSIZE) + xrecoff;
#ifdef DEBUG
	ereport(LOG,
			(errmsg("lsn: %X %X %llX", xlogid, xrecoff, lsn)));
#endif
	return lsn;
}

static RETSIGTYPE my_signal_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);

	switch (sig)
	{
		case SIGTERM:
		case SIGINT:
		case SIGQUIT:
			exit(0);
			break;

			/* Failback or new node added */
		case SIGUSR1:
			restart_request = 1;
			break;

		default:
			exit(1);
			break;
	}

	POOL_SETMASK(&UnBlockSig);

	errno = save_errno;
}

static RETSIGTYPE reload_config_handler(int sig)
{
	int			save_errno = errno;

	POOL_SETMASK(&BlockSig);
	reload_config_request = 1;
	POOL_SETMASK(&UnBlockSig);
	errno = save_errno;
}

static void
reload_config(void)
{
	ereport(LOG,
			(errmsg("reloading config file")));
	MemoryContext oldContext = MemoryContextSwitchTo(TopMemoryContext);

	pool_get_config(get_config_file_name(), CFGCXT_RELOAD);
	MemoryContextSwitchTo(oldContext);
	if (pool_config->enable_pool_hba)
		load_hba(get_hba_file_name());
	reload_config_request = 0;
}

/*
 * Execute query against specified backend using an established connection to
 * backend.  Return -1 on failure or 0 otherwise.  Caller must prepare memory
 * for POOL_SELECT_RESULT and pass it as "res". It is guaranteed that no
 * exception occurs within this function.
 */
int
get_query_result(POOL_CONNECTION_POOL_SLOT * *slots, int backend_id, char *query, POOL_SELECT_RESULT * *res)
{
	int			sts = -1;
	MemoryContext oldContext = CurrentMemoryContext;

	PG_TRY();
	{
		do_query(slots[backend_id]->con, query, res, PROTO_MAJOR_V3);
	}
	PG_CATCH();
	{
		/* ignore the error message */
		res = NULL;
		MemoryContextSwitchTo(oldContext);
		FlushErrorState();
		ereport(LOG,
				(errmsg("get_query_result: do_query failed")));
	}
	PG_END_TRY();

	if (!res)
	{
		ereport(LOG,
				(errmsg("get_query_result: no result returned"),
				 errdetail("node id (%d)", backend_id)));
		return sts;
	}

	if ((*res)->numrows <= 0)
	{
		free_select_result(*res);
		ereport(LOG,
				(errmsg("get_query_result: no rows returned"),
				 errdetail("node id (%d)", backend_id)));
		return sts;
	}

	sts = 0;
	return sts;
}
