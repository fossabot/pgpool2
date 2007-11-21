/* -*-pgsql-c-*- */
/*
 * $Header$
 * 
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2007	PgPool Global Development Group
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
 * recovery.c: online recovery process
 *
 */

#include "config.h"

#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "libpq-fe.h"

#define WAIT_RETRY_COUNT 30

static int exec_checkpoint(PGconn *conn);
static int exec_recovery(PGconn *conn, BackendInfo *backend, char first_stage);
static int exec_remote_start(PGconn *conn, BackendInfo *backend);
static PGconn *connect_backend_libpq(BackendInfo *backend);
static int wait_connection_closed(void);
static int check_postmaster_started(BackendInfo *backend);

static char recovery_command[1024];

int start_recovery(int recovery_node)
{
	BackendInfo *backend;
	BackendInfo *recovery_backend;
	PGconn *conn;

	if (VALID_BACKEND(recovery_node))
	{
		pool_error("start_recovery: backend node %d is alive", recovery_node);
		return 1;
	}

	Req_info->kind = NODE_RECOVERY_REQUEST;

	backend = &pool_config->backend_desc->backend_info[MASTER_NODE_ID];
	recovery_backend = &pool_config->backend_desc->backend_info[recovery_node];

	conn = connect_backend_libpq(backend);
	if (conn == NULL)
	{
		PQfinish(conn);
		pool_error("start_recover: could not connect master node.");
		return 1;
	}

	/* 1st stage */
	if (exec_checkpoint(conn) != 0)
	{
		PQfinish(conn);
		pool_error("start_recovery: CHECKPOINT failed");
		return 1;
	}
	if (exec_recovery(conn, recovery_backend, 1) != 0)
	{
		PQfinish(conn);
		return 1;
	}

	/* 2nd stage */
	*InRecovery = 1;
	if (wait_connection_closed() != 0)
	{
		PQfinish(conn);
		pool_error("start_recovery: timeover for waiting connection closed");
		return 1;
	}

	if (exec_checkpoint(conn) != 0)
	{
		PQfinish(conn);
		pool_error("start_recovery: CHECKPOINT failed");
		return 1;
	}
	if (exec_recovery(conn, recovery_backend, 0) != 0)
	{
		PQfinish(conn);
		return 1;
	}
	if (exec_remote_start(conn, recovery_backend) != 0)
	{
		PQfinish(conn);
		pool_error("start_recovery: remote start failed");
		return 1;
	}

	if (check_postmaster_started(recovery_backend))
	{
		PQfinish(conn);
		pool_error("start_recovery: check start failed");
		return 1;
	}

	send_failback_request(recovery_node);
	pause(); /* wait for failback */

	PQfinish(conn);

	return 0;
}

/*
 * Notice all children finishing recovery.
 */
void finish_recovery(void)
{
	*InRecovery = 0;
	kill(getppid(), SIGUSR2);
}

/*
 * Execute CHECKPOINT
 */
static int exec_checkpoint(PGconn *conn)
{
	PGresult *result;
	int r;

	pool_debug("exec_checkpoint: start checkpoint");
	result = PQexec(conn, "CHECKPOINT");
	r = (PQresultStatus(result) !=  PGRES_COMMAND_OK);
	PQclear(result);
	pool_debug("exec_checkpoint: finish checkpoint");
	return r;
}

/*
 * Call pgpool_recovery() function.
 */
static int exec_recovery(PGconn *conn, BackendInfo *backend, char first_stage)
{
	PGresult *result;
	char *hostname;
	char *script;
	int r;

	if (strlen(backend->backend_hostname) == 0)
		hostname = "localhost";
	else
		hostname = backend->backend_hostname;

	script = first_stage ? pool_config->recovery_1st_stage_command : pool_config->recovery_2nd_stage_command;

	if (script == NULL || strlen(script) == 0)
	{
		/* do not execute script */
		return 0;
	}

	snprintf(recovery_command,
			 sizeof(recovery_command),
			 "SELECT pgpool_recovery('%s', '%s', '%s')",
			 script,
			 hostname,
			 backend->backend_data_directory);

	pool_debug("exec_recovery: start recovery");
	result = PQexec(conn, recovery_command);
	r = (PQresultStatus(result) !=  PGRES_TUPLES_OK);
	if (r != 0)
		pool_error("exec_recovery: pgpool_recovery failed: %s", PQresultErrorMessage(result));
	PQclear(result);
	pool_debug("exec_recovery: finish recovery");
	return r;
}

/*
 * Call pgpool_remote_start() function.
 */
static int exec_remote_start(PGconn *conn, BackendInfo *backend)
{
	PGresult *result;
	char *hostname;
	int r;

	if (strlen(backend->backend_hostname) == 0)
		hostname = "localhost";
	else
		hostname = backend->backend_hostname;

	snprintf(recovery_command, sizeof(recovery_command),
			 "SELECT pgpool_remote_start('%s', '%s')",
			 hostname,
			 backend->backend_data_directory);

	pool_debug("exec_remote_start: start pgpool_remote_start");
	result = PQexec(conn, recovery_command);
	r = (PQresultStatus(result) !=  PGRES_TUPLES_OK);
	if (r != 0)
		pool_error("exec_remote_start: pgpool_remote_start failed: %s", PQresultErrorMessage(result));
	PQclear(result);
	pool_debug("exec_remote_start: finish pgpool_remote_start");
	return r;
}

/*
 * Check postmaster is started.
 */
static int check_postmaster_started(BackendInfo *backend)
{
	int i;
	char port_str[16];
	PGconn *conn;

	snprintf(port_str, sizeof(port_str),
			 "%d", backend->backend_port);
	for (i = 0; i < WAIT_RETRY_COUNT; i++)
	{
		ConnStatusType r;
		conn = PQsetdbLogin(backend->backend_hostname,
							port_str,
							NULL,
							NULL,
							"template1",
							pool_config->recovery_user,
							pool_config->recovery_password);
		r = PQstatus(conn);
		PQfinish(conn);
		if (r == CONNECTION_OK)
			return 0;

		sleep(3);
	}
	return 1;
}

static PGconn *connect_backend_libpq(BackendInfo *backend)
{
	char port_str[16];
	PGconn *conn;

	snprintf(port_str, sizeof(port_str),
			 "%d", backend->backend_port);
	conn = PQsetdbLogin(backend->backend_hostname,
						port_str,
						NULL,
						NULL,
						"template1",
						pool_config->recovery_user,
						pool_config->recovery_password);

	if (PQstatus(conn) != CONNECTION_OK)
	{
		PQfinish(conn);
		return NULL;
	}
	return conn;
}

/*
 * Wait all connections are closed.
 * If connection remained, sleep 3 sec and retry to wait.
 */
static int wait_connection_closed(void)
{
	int i;

	for (i = 0; i < WAIT_RETRY_COUNT; i++)
	{
		if (Req_info->conn_counter == 0)
			return 0;
		sleep(3);
	}

	return 1;
}
