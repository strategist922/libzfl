/*  =========================================================================
    zfl_rpc_client.c - client side RPC

    Client side API for implementing reliable remote procedure calls.

    -------------------------------------------------------------------------
    Copyright (c) 1991-2010 iMatix Corporation <www.imatix.com>
    Copyright other contributors as noted in the AUTHORS file.

    This file is part of the ZeroMQ Function Library: http://zfl.zeromq.org

    This is free software; you can redistribute it and/or modify it under the
    terms of the GNU Lesser General Public License as published by the Free
    Software Foundation; either version 3 of the License, or (at your option)
    any later version.

    This software is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABIL-
    ITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General
    Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
    =========================================================================
*/

#include <zmq.h>
#include "../include/zfl_prelude.h"
#include "../include/zfl_hash.h"
#include "../include/zfl_list.h"
#include "../include/zfl_msg.h"
#include "../include/zfl_rpc_client.h"

//  Heartbeat rate (in microseconds)
#define HEARTBEAT_INTERVAL      500000

//  Maximum time we wait for server's reply (in microseconds)
#define MAX_PROCESSING_TIME     2000000

//  Structure of our class

struct _zfl_rpc_client {
    void
        *rpc_socket,            //  used to send requests/receive replies
        *control_socket;        //  used to control RPC thread
    pthread_t
        thread;                 //  ID of API thread
};

//  Internal structure used by RPC thread

struct rpc_client {
    void
        *frontend,              //  used to communicate with application
        *backend,               //  used to communicate with RPC server
        *control;               //  used to control RPC server
    zfl_list_t
        *servers,               //  servers client is connected to
        *alive_servers,         //  list of alive server
        *lru_queue;             //  servers ready to serve (in LRU order)
    zfl_hash_t
        *registry;              //  maps server names to pointer to server struct
    zfl_msg_t
        *request;               //  pending request or NULL
    struct rpc_server
        *current_server;        //  server processing the last request or NULL
    uint64_t
        next_heartbeat,         //  time of next heartbeat
        processing_deadline;    //  utill when we wait for result
};

//  Represents server as viewed by client

struct rpc_server {
    char
        *server_id;
    int
        alive;                  //  true iff server's heart is beating
    uint64_t
        heartbeat_deadline;     //  utill when we wait for heartbeat
};

//  Used to pass arguments on RPC thread creation

struct rpc_client_thread_args {
    void
        *context;       // 0MQ context
    char
        *rpc_endpoint,
        *control_endpoint;
};

//  --------------------------------------------------------------------------
//  Return current time (in microseconds)

static uint64_t
now_us ()
{
    struct timeval tv;
    int rc = gettimeofday (&tv, NULL);
    assert (rc == 0);
    return (uint64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}


//  --------------------------------------------------------------------------
//  Allocate and initialize rpc_struct

static struct rpc_server *
new_rpc_server (char *server_id)
{
    struct rpc_server *server = zmalloc (sizeof (struct rpc_server));
    assert (server);
    server->server_id = strdup (server_id);
    server->alive = 0;
    return server;
}


//  --------------------------------------------------------------------------
//  Handle message received from a server

static void
backend_event (struct rpc_client *rpc)
{
    zfl_msg_t *msg = zfl_msg_recv (rpc->backend);
    assert (msg);

    char *server_id = zfl_msg_unwrap (msg);
    struct rpc_server *server = zfl_hash_lookup (rpc->registry, server_id);
    assert (server);
    free (server_id);

    if (zfl_msg_parts (msg) == 0) {
        //  Heartbeat signal
        if (server->alive)
            zfl_list_remove (rpc->alive_servers, server);
        else {
            zfl_list_append (rpc->lru_queue, server);
            server->alive = 1;
        }
        server->heartbeat_deadline = now_us () + HEARTBEAT_INTERVAL;
        zfl_list_append (rpc->alive_servers, server);
    }
    else
    if (server == rpc->current_server) {
        zfl_msg_send (&msg, rpc->frontend);
        assert (rpc->request);
        zfl_msg_destroy (&rpc->request);
        rpc->current_server = NULL;
    }
    zfl_msg_destroy (&msg);
}


//  --------------------------------------------------------------------------
//  Receive request from client

static void
frontend_event (struct rpc_client *rpc)
{
    zfl_msg_t *msg = zfl_msg_recv (rpc->frontend);
    assert (msg);

    assert (zfl_list_size (rpc->servers) > 0);
    assert (rpc->request == NULL);
    assert (rpc->current_server == NULL);

    //  Save the request now. It will be handled in main loop.
    rpc->request = msg;
}


//  --------------------------------------------------------------------------
//  Handle control message from application.
//  Returns TRUE when the received message contains `stop` command

static Bool
control_event (struct rpc_client *rpc)
{
    int rc;
    Bool stopped = FALSE;

    zfl_msg_t *msg = zfl_msg_recv (rpc->control);
    assert (msg);
    assert (zfl_msg_parts (msg) > 0);

    char *command = zfl_msg_pop (msg);
    if (strcmp (command, "stop") == 0) {
        assert (zfl_msg_parts (msg) == 0);
        stopped = TRUE;
    }
    else {
        assert (strcmp (command, "connect") == 0);
        assert (zfl_msg_parts (msg) == 2);
        char *server_id = zfl_msg_pop (msg);
        char *endpoint = zfl_msg_pop (msg);

        //  Insert new server, which can't already exist
        assert (zfl_hash_lookup (rpc->registry, server_id) == NULL);
        rc = zmq_connect (rpc->backend, endpoint);
        assert (rc == 0);
        struct rpc_server *server = new_rpc_server (server_id);
        rc = zfl_hash_insert (rpc->registry, server_id, server);
        assert (rc == 0);
        zfl_list_append (rpc->servers, server);

        //  Send response
        zfl_msg_t *response = zfl_msg_new ();
        zfl_msg_push (response, "ok");
        zfl_msg_send (&response, rpc->control);
        free (server_id);
        free (endpoint);
    }
    zfl_msg_destroy (&msg);
    free (command);

    return stopped;
}


//  --------------------------------------------------------------------------
//  Send heartbeat message to connected servers.

static void
heartbeat (struct rpc_client *rpc)
{
    zfl_list_t *servers = zfl_list_copy (rpc->servers);
    assert (servers);

    while (zfl_list_size (servers) > 0) {
        struct rpc_server *server = zfl_list_first (servers);
        zfl_list_remove (servers, server);

        //  Prepare and send out heartbeat message
        zfl_msg_t *msg = zfl_msg_new ();
        assert (msg);
        zfl_msg_wrap (msg, server->server_id, "");
        zfl_msg_send (&msg, rpc->backend);
    }
    zfl_list_destroy (&servers);
}


//  --------------------------------------------------------------------------
static void *
rpc_client_thread (void *arg)
{
    struct rpc_client_thread_args *client_args = arg;
    int rc;

    struct rpc_client *rpc = zmalloc (sizeof (struct rpc_client));
    assert (rpc);

    rpc->frontend = zmq_socket (client_args->context, ZMQ_REP);
    assert (rpc->frontend);
    rc = zmq_connect (rpc->frontend, client_args->rpc_endpoint);
    assert (rc == 0);

    rpc->control = zmq_socket (client_args->context, ZMQ_REP);
    assert (rpc->control);
    rc = zmq_connect (rpc->control, client_args->control_endpoint);
    assert (rc == 0);

    rpc->backend = zmq_socket (client_args->context, ZMQ_XREP);
    assert (rpc->backend);

    free (client_args->rpc_endpoint);
    free (client_args->control_endpoint);
    free (client_args);

    rpc->servers = zfl_list_new ();
    assert (rpc->servers);

    rpc->alive_servers = zfl_list_new ();
    assert (rpc->alive_servers);

    rpc->lru_queue = zfl_list_new ();
    assert (rpc->lru_queue);

    rpc->registry = zfl_hash_new ();
    assert (rpc->registry);

    rpc->request = NULL;
    rpc->current_server = NULL;
    rpc->next_heartbeat = now_us ();

    //  Controls how long we wait for message. Updated during processing.
    long poll_timeout = -1;

    //  Controls the server loop
    int stopped = 0;

    while (!stopped) {
        zmq_pollitem_t items [] = {
            //  Poll for server activity
            {rpc->backend, -1, ZMQ_POLLIN, 0},
            //  Poll for client activity
            {rpc->frontend, -1, ZMQ_POLLIN, 0},
            //  Poll for new control command
            {rpc->control, -1, ZMQ_POLLIN, 0}
        };

        rc = zmq_poll (items, 3, poll_timeout);
        assert (rc != -1);

        if (items [0].revents & ZMQ_POLLIN)
            // Either response or heartbeat signal
            backend_event (rpc);
        if (items [1].revents & ZMQ_POLLIN)
            // select server and forward the request to him
            frontend_event (rpc);
        if (items [2].revents & ZMQ_POLLIN)
            //  Either connect or stop message
            stopped = control_event (rpc);

        //  Get current time
        uint64_t now = now_us ();

        //  Time for heartbeat?
        if (now >= rpc->next_heartbeat) {
            heartbeat (rpc);
            rpc->next_heartbeat = now + HEARTBEAT_INTERVAL;
        }

        //  Check for dead servers
        while (zfl_list_size (rpc->alive_servers) > 0) {
            struct rpc_server *server = zfl_list_first (rpc->alive_servers);
            if (now < server->heartbeat_deadline)
                break;
            zfl_list_remove (rpc->alive_servers, server);
            zfl_list_remove (rpc->lru_queue, server);
            server->alive = 0;
        }

        //  Check for late response
        if (rpc->current_server)
            if (now >= rpc->processing_deadline)
                rpc->current_server = NULL;

        //  Forward pending request to the least recently used server
        if (rpc->request && !rpc->current_server) {
            if (zfl_list_size (rpc->lru_queue) > 0) {
                struct rpc_server *server = zfl_list_first (rpc->lru_queue);
                zfl_msg_t *msg = zfl_msg_dup (rpc->request);
                zfl_msg_wrap (msg, server->server_id, NULL);
                zfl_msg_send (&msg, rpc->backend);
                rpc->current_server = server;
                rpc->processing_deadline = now + MAX_PROCESSING_TIME;
                //  Move the server at the end of the LRU queue
                zfl_list_remove (rpc->lru_queue, server);
                zfl_list_append (rpc->lru_queue, server);
            }
        }

        //  How long to poll for next message?
        if (zfl_list_size (rpc->servers) == 0)
            //  If there is no server, we can wait infinitely
            poll_timeout = -1;
        else {
            //  Wait at most till next heartbeat
            poll_timeout = rpc->next_heartbeat - now;

            if (zfl_list_size (rpc->alive_servers) > 0) {
                struct rpc_server *server = zfl_list_first (rpc->alive_servers);
                if (server->heartbeat_deadline < now + poll_timeout)
                    poll_timeout = server->heartbeat_deadline - now;
            }

            if (rpc->current_server)
                if (rpc->processing_deadline < now + poll_timeout)
                    //  Wait at most till next processing deadline
                    poll_timeout = rpc->processing_deadline - now;
        }
    }

    //  Close sockets
    zmq_close (rpc->frontend);
    zmq_close (rpc->backend);
    zmq_close (rpc->control);

    return NULL;
}


//  --------------------------------------------------------------------------
//  Formats control endpoint

static char *
format_control_endpoint (char *endpoint)
{
    char *suffix = "-control";
    char *buf = malloc (strlen (endpoint) + strlen (suffix) + 1);
    assert (buf);
    strcpy (buf, endpoint);
    strcat (buf, suffix);
    return buf;
}


//  --------------------------------------------------------------------------
//  Constructor

zfl_rpc_client_t *
zfl_rpc_client_new (void *zmq_context, char *endpoint)
{
    int rc;

    zfl_rpc_client_t *self = zmalloc (sizeof (zfl_rpc_client_t));
    assert (self);

    self->rpc_socket = zmq_socket (zmq_context, ZMQ_REQ);
    assert (self->rpc_socket);
    rc = zmq_bind (self->rpc_socket, endpoint);
    assert (rc == 0);

    self->control_socket = zmq_socket (zmq_context, ZMQ_REQ);
    assert (self->control_socket);
    char *control_endpoint = format_control_endpoint (endpoint);
    rc = zmq_bind (self->control_socket, control_endpoint);
    assert (rc == 0);

    struct rpc_client_thread_args *args = zmalloc (sizeof *args);
    assert (args);
    args->context = zmq_context;
    args->rpc_endpoint = strdup (endpoint);
    args->control_endpoint = control_endpoint;

    rc = pthread_create (&self->thread, NULL, rpc_client_thread, args);
    assert (rc == 0);

    return self;
}


//  --------------------------------------------------------------------------
//  Destructor

void
zfl_rpc_client_destroy (zfl_rpc_client_t **self_p)
{
    zfl_rpc_client_t *self = *self_p;
    int rc;

    if (!self)
        return;

    zfl_msg_t *stop_request = zfl_msg_new ();
    assert (stop_request);

    zfl_msg_push (stop_request, "stop");
    zfl_msg_send (&stop_request, self->control_socket);

    rc = pthread_join (self->thread, NULL);
    assert (rc == 0);

    zmq_close (self->rpc_socket);
    zmq_close (self->control_socket);

    free (self);
    *self_p = NULL;
}


//  --------------------------------------------------------------------------
//  Connect RPC client to server
//  One RPC server can be connected to more servers

void
zfl_rpc_client_connect (zfl_rpc_client_t *self, char *server_id, char *endpoint)
{
    zfl_msg_t *msg = zfl_msg_new ();
    assert (msg);
    zfl_msg_push (msg, endpoint);
    zfl_msg_push (msg, server_id);
    zfl_msg_push (msg, "connect");
    zfl_msg_send (&msg, self->control_socket);

    //  Receive and drop response
    msg = zfl_msg_recv (self->control_socket);
    zfl_msg_destroy (&msg);
}


//  --------------------------------------------------------------------------
//  Make remote procedure call
//  Returns server's reply

zfl_msg_t *
zfl_rpc_client_send (zfl_rpc_client_t *self, zfl_msg_t **request_p)
{
    zfl_msg_send (request_p, self->rpc_socket);
    zfl_msg_t *reply = zfl_msg_recv (self->rpc_socket);
    return reply;
}
