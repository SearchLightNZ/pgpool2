/* -*-pgsql-c-*- */
/*
 * $Header$
 *
 * pgpool: a language independent connection pool server for PostgreSQL
 * written by Tatsuo Ishii
 *
 * Copyright (c) 2003-2014	PgPool Global Development Group
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
#include "utils/elog.h"

#include "pool.h"
#include "pool_config.h"

#include "libpq-fe.h"

#include "watchdog/wd_ipc_commands.h"

#define WAIT_RETRY_COUNT (pool_config->recovery_timeout / 3)

#define FIRST_STAGE 0
#define SECOND_STAGE 1

static void exec_checkpoint(PGconn *conn);
static void exec_recovery(PGconn *conn, BackendInfo *master_backend, BackendInfo *recovery_backend, char stage);
static void exec_remote_start(PGconn *conn, BackendInfo *backend);
static PGconn *connect_backend_libpq(BackendInfo *backend);
static void check_postmaster_started(BackendInfo *backend);

static char recovery_command[1024];

extern volatile sig_atomic_t pcp_worker_wakeup_request;

/*
 * Start online recovery.
 * "recovery_node" is the node to be recovered.
 * Master or primary node is chosen in this function.
 */
void start_recovery(int recovery_node)
{
	int node_id;
	BackendInfo *backend;
	BackendInfo *recovery_backend;
	PGconn *conn;
	int failback_wait_count;
#define FAILBACK_WAIT_MAX_RETRY 5		/* 5 seconds should be enough for failback operation */

	ereport(LOG,
		(errmsg("starting recovering node %d", recovery_node)));

	if ( (recovery_node < 0) || (recovery_node >= pool_config->backend_desc->num_backends) )
		ereport(ERROR,
				(errmsg("node recovery failed, node id: %d is not valid", recovery_node)));

	if (*(my_backend_status[(recovery_node)]) == CON_UNUSED)
		ereport(ERROR,
				(errmsg("node recovery failed, node id: %d is unused", recovery_node)));

	if (VALID_BACKEND(recovery_node))
		ereport(ERROR,
				(errmsg("node recovery failed, node id: %d is alive", recovery_node)));

	/* select master/primary node */
	node_id = MASTER_SLAVE ? PRIMARY_NODE_ID : REAL_MASTER_NODE_ID;
	backend = &pool_config->backend_desc->backend_info[node_id];

	/* get node info to be recovered */
	recovery_backend = &pool_config->backend_desc->backend_info[recovery_node];

	conn = connect_backend_libpq(backend);
	if (conn == NULL)
		ereport(ERROR,
				(errmsg("node recovery failed, unable to connect to master node: %d ", node_id)));

	PG_TRY();
	{
		/* 1st stage */
		if (REPLICATION)
		{
			exec_checkpoint(conn);
			ereport(LOG,
				(errmsg("node recovery, CHECKPOINT in the 1st stage done")));
		}

		exec_recovery(conn, backend, recovery_backend, FIRST_STAGE);

		ereport(LOG,
			(errmsg("node recovery, 1st stage is done")));

		if (REPLICATION)
		{
			ereport(LOG,
				(errmsg("node recovery, starting 2nd stage")));

			/* 2nd stage */
			*InRecovery = RECOVERY_ONLINE;
			if (pool_config->use_watchdog)
			{
				/* announce start recovery */
				if (COMMAND_OK != wd_start_recovery())
					ereport(ERROR,
							(errmsg("node recovery failed, failed to send start recovery packet")));
			}

			if (wait_connection_closed() != 0)
				ereport(ERROR,
						(errmsg("node recovery failed, waiting connection closed in the other pgpools timeout")));

			ereport(LOG,
				(errmsg("node recovery, all connections from clients have been closed")));

			exec_checkpoint(conn);

			ereport(LOG,
				(errmsg("node recovery"),
					 errdetail("CHECKPOINT in the 2nd stage done")));

			exec_recovery(conn, backend, recovery_backend, SECOND_STAGE);
		}

		exec_remote_start(conn, recovery_backend);

		check_postmaster_started(recovery_backend);

		ereport(LOG,
			(errmsg("node recovery, node: %d restarted", recovery_node)));

		/*
		 * reset failover completion flag.  this is necessary since
		 * previous failover/failback will set the flag to 1.
		 */
		pcp_worker_wakeup_request = 0;

		/* send failback request to pgpool parent */
		send_failback_request(recovery_node,false, false);

		/* wait for failback */
		failback_wait_count = 0;
		while (!pcp_worker_wakeup_request)
		{
			struct timeval t = {1, 0};
			/* polling SIGUSR2 signal every 1 sec */
			select(0, NULL, NULL, NULL, &t);
			failback_wait_count++;
			if (failback_wait_count >= FAILBACK_WAIT_MAX_RETRY)
			{
				ereport(LOG,
					(errmsg("node recovery"),
						errdetail("waiting for wake up request is timeout(%d seconds)",
							   FAILBACK_WAIT_MAX_RETRY)));

				break;
			}
		}
		pcp_worker_wakeup_request = 0;
	}
	PG_CATCH();
	{
		PQfinish(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	PQfinish(conn);

	ereport(LOG,
			(errmsg("recovery done")));
}

/*
 * Notice all children finishing recovery.
 */
void finish_recovery(void)
{
	/* announce end recovery */
	if (pool_config->use_watchdog && *InRecovery != RECOVERY_INIT)
	{
		wd_end_recovery();
	}

	*InRecovery = RECOVERY_INIT;
	pool_signal_parent(SIGUSR2);
}

/*
 * Execute CHECKPOINT
 */
static void exec_checkpoint(PGconn *conn)
{
	PGresult *result;
	ereport(DEBUG1,
		(errmsg("recovery execute checkpoint, start checkpoint")));

	result = PQexec(conn, "CHECKPOINT");
	if(PQresultStatus(result) !=  PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("executing recovery, execute CHECKPOINT failed")));
	PQclear(result);

	ereport(DEBUG1,
		(errmsg("recovery execute checkpoint, finish checkpoint")));
}

/*
 * Call pgpool_recovery() function.
 */
static void exec_recovery(PGconn *conn, BackendInfo *master_backend, BackendInfo *recovery_backend, char stage)
{
	PGresult *result;
	char *hostname;
	char *script;

	if (strlen(recovery_backend->backend_hostname) == 0 || *(recovery_backend->backend_hostname) == '/')
		hostname = "localhost";
	else
		hostname = recovery_backend->backend_hostname;

	script = (stage == FIRST_STAGE) ?
		pool_config->recovery_1st_stage_command : pool_config->recovery_2nd_stage_command;

	if (script == NULL || strlen(script) == 0)
	{
		/* do not execute script */
		return;
	}

	/*
	 * Execute recovery command
	 */
	snprintf(recovery_command,
			 sizeof(recovery_command),
			 "SELECT pgpool_recovery('%s', '%s', '%s', '%d')",
			 script,
			 hostname,
			 recovery_backend->backend_data_directory,
			 master_backend->backend_port);

	ereport(LOG,
		(errmsg("executing recovery"),
			 errdetail("starting recovery command: \"%s\"", recovery_command)));

	ereport(LOG,
		(errmsg("executing recovery"),
			 errdetail("disabling statement_timeout")));

	result = PQexec(conn, "SET statement_timeout To 0");
	if(PQresultStatus(result) !=  PGRES_COMMAND_OK)
		ereport(ERROR,
				(errmsg("executing recovery, SET STATEMENT_TIMEOUT failed at \"%s\"",
						(stage == FIRST_STAGE) ? "1st stage" : "2nd stage")));
		
	PQclear(result);

	ereport(DEBUG1,
		(errmsg("executing recovery, start recovery")));

	result = PQexec(conn, recovery_command);
	if(PQresultStatus(result) !=  PGRES_TUPLES_OK)
		ereport(ERROR,
				(errmsg("executing recovery, execution of command failed at \"%s\"",
						(stage == FIRST_STAGE) ? "1st stage" : "2nd stage"),
				 errdetail("command:\"%s\"",script)));
	
	PQclear(result);

	ereport(DEBUG1,
		(errmsg("executing recovery, finish recovery")));
}

/*
 * Call pgpool_remote_start() function.
 */
static void exec_remote_start(PGconn *conn, BackendInfo *backend)
{
	PGresult *result;
	char *hostname;

	if (strlen(backend->backend_hostname) == 0 || *(backend->backend_hostname) == '/')
		hostname = "localhost";
	else
		hostname = backend->backend_hostname;

	snprintf(recovery_command, sizeof(recovery_command),
			 "SELECT pgpool_remote_start('%s', '%s')",
			 hostname,
			 backend->backend_data_directory);

	ereport(DEBUG1,
		(errmsg("executing remote start"),
			 errdetail("start pgpool_remote_start")));

	result = PQexec(conn, recovery_command);
	if(PQresultStatus(result) !=  PGRES_TUPLES_OK)
		ereport(ERROR,
			(errmsg("executing remote start failed with error: \"%s\"",PQresultErrorMessage(result))));

	PQclear(result);

	ereport(DEBUG1,
		(errmsg("executing remote start"),
			 errdetail("finish pgpool_remote_start")));
}

/*
 * Check postmaster is started.
 */
static void check_postmaster_started(BackendInfo *backend)
{
	int i = 0;
	char port_str[16];
	PGconn *conn;
	char *dbname;

	snprintf(port_str, sizeof(port_str),"%d", backend->backend_port);

	/*
	 * First we try with "postgres" database.
	 */
	dbname = "postgres";

	do {
		ConnStatusType r;

		ereport(LOG,
			(errmsg("checking if postmaster is started"),
				errdetail("trying to connect to postmaster on hostname:%s database:%s user:%s (retry %d times)",
					   backend->backend_hostname, dbname, pool_config->recovery_user, i)));

		conn = PQsetdbLogin(backend->backend_hostname,
							port_str,
							NULL,
							NULL,
							dbname,
							pool_config->recovery_user,
							pool_config->recovery_password);

		r = PQstatus(conn);
		PQfinish(conn);
		if (r == CONNECTION_OK)
			return;

		ereport(LOG,
			(errmsg("checking if postmaster is started"),
				errdetail("failed to connect to postmaster on hostname:%s database:%s user:%s",
					   backend->backend_hostname, dbname, pool_config->recovery_user)));
		
		sleep(3);
	} while (i++ < 3);	/* XXX Hard coded retry (9 seconds) */

	/*
	 * Retry with "template1" database.
	 */
	dbname = "template1";
	i = 0;

	do {
		ConnStatusType r;

		ereport(LOG,
			(errmsg("checking if postmaster is started"),
				errdetail("trying to connect to postmaster on hostname:%s database:%s user:%s (retry %d times)",
					   backend->backend_hostname, dbname, pool_config->recovery_user, i)));

		conn = PQsetdbLogin(backend->backend_hostname,
							port_str,
							NULL,
							NULL,
							dbname,
							pool_config->recovery_user,
							pool_config->recovery_password);

		r = PQstatus(conn);
		PQfinish(conn);
		if (r == CONNECTION_OK)
			return;

		ereport(LOG,
			(errmsg("checking if postmaster is started"),
				 errdetail("failed to connect to postmaster on hostname:%s database:%s user:%s",
						   backend->backend_hostname, dbname, pool_config->recovery_user)));

		if (WAIT_RETRY_COUNT != 0)
			sleep(3);
	} while (i++ < WAIT_RETRY_COUNT);

	ereport(ERROR,
		(errmsg("recovery is checking if postmaster is started"),
			 errdetail("postmaster on hostname:\"%s\" database:\"%s\" user:\"%s\" failed to start in %d second",
					   backend->backend_hostname, dbname, pool_config->recovery_user, pool_config->recovery_timeout)));
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
 */
int wait_connection_closed(void)
{
	int i = 0;

	do {

		if (Req_info->conn_counter == 0)
			return 0;

		if (WAIT_RETRY_COUNT != 0)
			sleep(3);
	} while (i++ < WAIT_RETRY_COUNT);
	ereport(LOG,
			(errmsg("wait_connection_closed: existing connections did not close in %d sec.", pool_config->recovery_timeout)));
	return 1;
}
