/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <Python.h>
#include "qpid/dispatch/python_embedded.h"
#include "policy.h"
#include <stdio.h>
#include <string.h>
#include "dispatch_private.h"
#include "connection_manager_private.h"
#include "qpid/dispatch/container.h"
#include "qpid/dispatch/server.h"
#include "qpid/dispatch/message.h"
#include <proton/engine.h>
#include <proton/message.h>
#include <proton/condition.h>
#include <proton/connection.h>
#include <proton/transport.h>
#include <proton/error.h>
#include <proton/event.h>
#include "qpid/dispatch/ctools.h"
#include "qpid/dispatch/hash.h"
#include "qpid/dispatch/threading.h"
#include "qpid/dispatch/iterator.h"
#include "qpid/dispatch/log.h"


/**
 * Private Function Prototypes
 */
void qd_policy_private_deny_amqp_connection(pn_connection_t *conn, const char *cond_name, const char *cond_descr);
void qd_policy_deny_amqp_session(pn_session_t *ssn, qd_connection_t *qd_conn);
void _qd_policy_deny_amqp_link(pn_link_t *link, qd_connection_t *qd_conn, char * s_or_r);
void _qd_policy_deny_amqp_sender_link(pn_link_t *pn_link, qd_connection_t *qd_conn);
void _qd_policy_deny_amqp_receiver_link(pn_link_t *pn_link, qd_connection_t *qd_conn);

//
// TODO: when policy dev is more complete lower the log level
//
#define POLICY_LOG_LEVEL QD_LOG_CRITICAL

//
// The current statistics maintained globally through multiple
// reconfiguration of policy settings.
//
static int n_connections = 0;
static int n_denied = 0;
static int n_processed = 0;

//
// error conditions signaled to effect denial
//
static char* RESOURCE_LIMIT_EXCEEDED     = "amqp:resource-limit-exceeded";
//static char* UNAUTHORIZED_ACCESS         = "amqp:unauthorized-access";
//static char* CONNECTION_FORCED           = "amqp:connection:forced";

//
// error descriptions signaled to effect denial
//
static char* CONNECTION_DISALLOWED         = "connection disallowed by local policy";
static char* SESSION_DISALLOWED            = "session disallowed by local policy";
static char* LINK_DISALLOWED               = "link disallowed by local policy";

//
// Policy configuration/statistics management interface
//
struct qd_policy_t {
    qd_dispatch_t        *qd;
    qd_log_source_t      *log_source;
    void                 *py_policy_manager;
                          // configured settings
    int                   max_connection_limit;
    char                 *policyFolder;
    bool                  enableAccessRules;
                          // live statistics
    int                   connections_processed;
    int                   connections_denied;
    int                   connections_current;
};

/** Create the policy structure
 * @param[in] qd pointer the the qd
 **/
qd_policy_t *qd_policy(qd_dispatch_t *qd)
{
    qd_policy_t *policy = NEW(qd_policy_t);

    policy->qd                   = qd;
    policy->log_source           = qd_log_source("POLICY");
    policy->max_connection_limit = 0;
    policy->policyFolder         = 0;
    policy->enableAccessRules    = false;
    policy->connections_processed= 0;
    policy->connections_denied   = 0;
    policy->connections_current  = 0;

    qd_log(policy->log_source, QD_LOG_TRACE, "Policy Initialized");
    return policy;
}


/** Free the policy structure
 * @param[in] policy pointer to the policy
 **/
void qd_policy_free(qd_policy_t *policy)
{
    if (policy->policyFolder)
        free(policy->policyFolder);
    free(policy);
}

//
//
#define CHECK() if (qd_error_code()) goto error

qd_error_t qd_entity_configure_policy(qd_policy_t *policy, qd_entity_t *entity)
{
    policy->max_connection_limit = qd_entity_opt_long(entity, "maximumConnections", 0); CHECK();
    if (policy->max_connection_limit < 0)
        return qd_error(QD_ERROR_CONFIG, "maximumConnections must be >= 0");
    policy->policyFolder =
        qd_entity_opt_string(entity, "policyFolder", 0); CHECK();
    policy->enableAccessRules = qd_entity_opt_bool(entity, "enableAccessRules", false); CHECK();
    qd_log(policy->log_source, QD_LOG_INFO, "Configured maximumConnections: %d", policy->max_connection_limit);
    return QD_ERROR_NONE;

error:
    if (policy->policyFolder)
        free(policy->policyFolder);
    qd_policy_free(policy);
    return qd_error_code();
}


//
//
qd_error_t qd_register_policy_manager(qd_policy_t *policy, void *policy_manager)
{
    policy->py_policy_manager = policy_manager;
    return QD_ERROR_NONE;
}


long qd_policy_c_counts_alloc()
{
    qd_policy_denial_counts_t * dc = NEW(qd_policy_denial_counts_t);
    assert(dc);
    memset(dc, 0, sizeof(qd_policy_denial_counts_t));
    return (long)dc;
}


void qd_policy_c_counts_free(long ccounts)
{
    void *dc = (void *)ccounts;
    assert(dc);
    free(dc);
}


qd_error_t qd_policy_c_counts_refresh(long ccounts, qd_entity_t *entity)
{
    qd_policy_denial_counts_t *dc = (qd_policy_denial_counts_t*)ccounts;
    if (!qd_entity_set_long(entity, "sessionDenied", dc->sessionDenied) &&
        !qd_entity_set_long(entity, "senderDenied", dc->senderDenied) &&
        !qd_entity_set_long(entity, "receiverDenied", dc->receiverDenied) &&
        !qd_entity_set_long(entity, "dynamicSrcDenied", dc->dynamicSrcDenied) &&
        !qd_entity_set_long(entity, "anonymousSenderDenied", dc->anonymousSenderDenied) &&
        !qd_entity_set_long(entity, "linkSourceDenied", dc->linkSourceDenied) &&
        !qd_entity_set_long(entity, "linkTargetDenied", dc->linkTargetDenied)
    )
        return QD_ERROR_NONE;
    return qd_error_code();
}


/** Update the statistics in qdrouterd.conf["policy"]
 * @param[in] entity pointer to the policy management object
 **/
qd_error_t qd_entity_refresh_policy(qd_entity_t* entity, void *unused) {
    // Return global stats
    if (!qd_entity_set_long(entity, "connectionsProcessed", n_processed) &&
        !qd_entity_set_long(entity, "connectionsDenied", n_denied) &&
        !qd_entity_set_long(entity, "connectionsCurrent", n_connections)
    )
        return QD_ERROR_NONE;
    return qd_error_code();
}


//
// Functions related to absolute connection counts.
// These handle connections at the socket level with
// no regard to user identity. Simple yes/no decisions
// are made and there is no AMQP channel for returning
// error conditions.
//

bool qd_policy_socket_accept(void *context, const char *hostname)
{
    qd_policy_t *policy = (qd_policy_t *)context;
    bool result = true;

    if (policy->max_connection_limit == 0) {
        // Policy not in force; connection counted and allowed
        n_connections += 1;
    } else {
        // Policy in force
        if (n_connections < policy->max_connection_limit) {
            // connection counted and allowed
            n_connections += 1;
            qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection '%s' allowed. N= %d", hostname, n_connections);
        } else {
            // connection denied
            result = false;
            n_denied += 1;
            qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection '%s' denied, N=%d", hostname, n_connections);
        }
    }
    n_processed += 1;
    return result;
}


//
//
void qd_policy_socket_close(void *context, const qd_connection_t *conn)
{
    qd_policy_t *policy = (qd_policy_t *)context;

    n_connections -= 1;
    assert (n_connections >= 0);
    if (policy->enableAccessRules) {
        // HACK ALERT: TODO: This should be deferred to a Python thread
        qd_python_lock_state_t lock_state = qd_python_lock();
        PyObject *module = PyImport_ImportModule("qpid_dispatch_internal.policy.policy_manager");
        if (module) {
            PyObject *close_connection = PyObject_GetAttrString(module, "policy_close_connection");
            if (close_connection) {
                PyObject *result = PyObject_CallFunction(close_connection, "(OK)",
                                                         (PyObject *)policy->py_policy_manager,
                                                          conn->connection_id);
                if (result) {
                    Py_XDECREF(result);
                } else {
                    qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection close failed: result");
                }
                Py_XDECREF(close_connection);
            } else {
                qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection close failed: close_connection");
            }
            Py_XDECREF(module);
        } else {
            qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection close failed: module");
        }
        qd_python_unlock(lock_state);
    }
    if (policy->max_connection_limit > 0) {
        const char *hostname = qdpn_connector_name(conn->pn_cxtr);
        qd_log(policy->log_source, POLICY_LOG_LEVEL, "Connection '%s' closed. N connections=%d", hostname, n_connections);
    }
}


//
// Functions related to authenticated connection denial.
// An AMQP Open has been received over some connection.
// Evaluate the connection auth and the Open fields to
// allow or deny the Open. Denied Open attempts are
// effected by returning Open and then Close_with_condition.
//
/** Look up user/host/app in python policyRuleset and give the AMQP Open
 *  a go-no_go decision. Return false if the mechanics of calling python
 *  fails. A policy lookup will deny the connection by returning a blank
 *  usergroup name in the name buffer.
 *  Connection and connection denial counting is done in the python code.
 * @param[in] policy pointer to policy
 * @param[in] username authenticated user name
 * @param[in] hostip numeric host ip address
 * @param[in] app application name received in remote AMQP Open.hostname
 * @param[in] conn_name connection name for tracking
 * @param[out] name_buf pointer to settings name buffer
 * @param[in] name_buf_size size of settings_buf
 **/
bool qd_policy_open_lookup_user(
    qd_policy_t *policy,
    const char *username,
    const char *hostip,
    const char *app,
    const char *conn_name,
    char       *name_buf,
    int         name_buf_size,
    uint64_t    conn_id,
    qd_policy_settings_t *settings)
{
    // Lookup the user/host/app for allow/deny and to get settings name
    bool res = false;
    qd_python_lock_state_t lock_state = qd_python_lock();
    PyObject *module = PyImport_ImportModule("qpid_dispatch_internal.policy.policy_manager");
    if (module) {
        PyObject *lookup_user = PyObject_GetAttrString(module, "policy_lookup_user");
        if (lookup_user) {
            PyObject *result = PyObject_CallFunction(lookup_user, "(OssssK)",
                                                     (PyObject *)policy->py_policy_manager,
                                                     username, hostip, app, conn_name, conn_id);
            if (result) {
                const char *res_string = PyString_AsString(result);
                strncpy(name_buf, res_string, name_buf_size);
                Py_XDECREF(result);
                res = true; // settings name returned
            } else {
                qd_log(policy->log_source, POLICY_LOG_LEVEL, "Internal: lookup_user: result");
            }
            Py_XDECREF(lookup_user);
        } else {
            qd_log(policy->log_source, POLICY_LOG_LEVEL, "Internal: lookup_user: lookup_user");
        }
    }
    if (!res) {
        if (module) {
            Py_XDECREF(module);
        }
        qd_python_unlock(lock_state);
        return false;
    }

    // 
    if (name_buf[0]) {
        // Go get the named settings
        res = false;
        PyObject *upolicy = PyDict_New();
        if (upolicy) {
            PyObject *lookup_settings = PyObject_GetAttrString(module, "policy_lookup_settings");
            if (lookup_settings) {
                PyObject *result2 = PyObject_CallFunction(lookup_settings, "(OssO)",
                                                        (PyObject *)policy->py_policy_manager,
                                                        app, name_buf, upolicy);
                if (result2) {
                    settings->maxFrameSize         = qd_entity_opt_long((qd_entity_t*)upolicy, "maxFrameSize", 0);
                    settings->maxMessageSize       = qd_entity_opt_long((qd_entity_t*)upolicy, "maxMessageSize", 0);
                    settings->maxSessionWindow     = qd_entity_opt_long((qd_entity_t*)upolicy, "maxSessionWindow", 0);
                    settings->maxSessions          = qd_entity_opt_long((qd_entity_t*)upolicy, "maxSessions", 0);
                    settings->maxSenders           = qd_entity_opt_long((qd_entity_t*)upolicy, "maxSenders", 0);
                    settings->maxReceivers         = qd_entity_opt_long((qd_entity_t*)upolicy, "maxReceivers", 0);
                    settings->allowAnonymousSender = qd_entity_opt_bool((qd_entity_t*)upolicy, "allowAnonymousSender", false);
                    settings->allowDynamicSrc      = qd_entity_opt_bool((qd_entity_t*)upolicy, "allowDynamicSrc", false);
                    settings->sources              = qd_entity_get_string((qd_entity_t*)upolicy, "sources");
                    settings->targets              = qd_entity_get_string((qd_entity_t*)upolicy, "targets");
                    settings->denialCounts         = (qd_policy_denial_counts_t*)
                                                    qd_entity_get_long((qd_entity_t*)upolicy, "denialCounts");
                    Py_XDECREF(result2);
                    res = true; // named settings content returned
                } else {
                    qd_log(policy->log_source, POLICY_LOG_LEVEL, "Internal: lookup_user: result2");
                }
                Py_XDECREF(lookup_settings);
            } else {
                qd_log(policy->log_source, POLICY_LOG_LEVEL, "Internal: lookup_user: lookup_settings");
            }
            Py_XDECREF(upolicy);
        } else {
            qd_log(policy->log_source, POLICY_LOG_LEVEL, "Internal: lookup_user: upolicy");
        }
    }
    Py_XDECREF(module);
    qd_python_unlock(lock_state);

    qd_log(policy->log_source, 
           POLICY_LOG_LEVEL, 
           "Policy AMQP Open lookup_user: %s, hostip: %s, app: %s, connection: %s. Usergroup: '%s'%s",
           username, hostip, app, conn_name, name_buf, (res ? "" : " Internal error."));

    return res;
}

/** Set the error condition and close the connection.
 * Over the wire this will send an open frame followed
 * immediately by a close frame with the error condition.
 * @param[in] conn proton connection being closed
 * @param[in] cond_name condition name
 * @param[in] cond_descr condition description
 **/ 
void qd_policy_private_deny_amqp_connection(pn_connection_t *conn, const char *cond_name, const char *cond_descr)
{
    pn_condition_t * cond = pn_connection_condition(conn);
    (void) pn_condition_set_name(       cond, cond_name);
    (void) pn_condition_set_description(cond, cond_descr);
    pn_connection_close(conn);
}


/** Internal function to deny an amqp session
 * The session is closed with a condition and the denial is counted.
 * @param[in,out] ssn proton session
 * @param[in,out] qd_conn dispatch connection
 */
void qd_policy_deny_amqp_session(pn_session_t *ssn, qd_connection_t *qd_conn)
{
    pn_condition_t * cond = pn_session_condition(ssn);
    (void) pn_condition_set_name(       cond, RESOURCE_LIMIT_EXCEEDED);
    (void) pn_condition_set_description(cond, SESSION_DISALLOWED);
    pn_session_close(ssn);

    pn_connection_t *conn = qd_connection_pn(qd_conn);
    qd_dispatch_t *qd = qd_conn->server->qd;
    qd_policy_t *policy = qd->policy;
    pn_transport_t *pn_trans = pn_connection_transport(conn);
    const char *username = pn_transport_get_user(pn_trans);
    const char *hostip = qdpn_connector_hostip(qd_conn->pn_cxtr);
    const char *app = pn_connection_remote_hostname(conn);
    qd_log(policy->log_source, 
           POLICY_LOG_LEVEL, 
           "Policy AMQP Begin Session denied due to session limit. user: %s, hostip: %s, app: %s", 
           username, hostip, app);

    qd_conn->policy_settings->denialCounts->sessionDenied++;
}


//
//
bool qd_policy_approve_amqp_session(pn_session_t *ssn, qd_connection_t *qd_conn)
{
    if (qd_conn->policy_settings) {
        if (qd_conn->policy_settings->maxSessions) {
            if (qd_conn->n_sessions == qd_conn->policy_settings->maxSessions) {
                qd_policy_deny_amqp_session(ssn, qd_conn);
                return false;
            }
        }
    }
    return true;
}


//
//
void qd_policy_apply_session_settings(pn_session_t *ssn, qd_connection_t *qd_conn)
{
    if (qd_conn->policy_settings && qd_conn->policy_settings->maxSessionWindow) {
        pn_session_set_incoming_capacity(ssn, qd_conn->policy_settings->maxSessionWindow);
    } else {
        pn_session_set_incoming_capacity(ssn, 1000000);
    }
}

//
//
void _qd_policy_deny_amqp_link(pn_link_t *link, qd_connection_t *qd_conn, char * s_or_r)
{
    pn_condition_t * cond = pn_link_condition(link);
    (void) pn_condition_set_name(       cond, RESOURCE_LIMIT_EXCEEDED);
    (void) pn_condition_set_description(cond, LINK_DISALLOWED);
    pn_link_close(link);

    pn_connection_t *conn = qd_connection_pn(qd_conn);
    qd_dispatch_t *qd = qd_conn->server->qd;
    qd_policy_t *policy = qd->policy;
    pn_transport_t *pn_trans = pn_connection_transport(conn);
    const char *username = pn_transport_get_user(pn_trans);
    const char *hostip = qdpn_connector_hostip(qd_conn->pn_cxtr);
    const char *app = pn_connection_remote_hostname(conn);
    qd_log(policy->log_source, 
           POLICY_LOG_LEVEL, 
           "Policy AMQP Attach Link denied due to %s limit. user: %s, hostip: %s, app: %s", 
           s_or_r, username, hostip, app);
}


//
//
void _qd_policy_deny_amqp_sender_link(pn_link_t *pn_link, qd_connection_t *qd_conn)
{
    _qd_policy_deny_amqp_link(pn_link, qd_conn, "sender");
    qd_conn->policy_settings->denialCounts->senderDenied++;
}


//
//
void _qd_policy_deny_amqp_receiver_link(pn_link_t *pn_link, qd_connection_t *qd_conn)
{
    _qd_policy_deny_amqp_link(pn_link, qd_conn, "receiver");
    qd_conn->policy_settings->denialCounts->receiverDenied++;
}


//
//
bool qd_policy_approve_amqp_sender_link(pn_link_t *pn_link, qd_connection_t *qd_conn)
{
    if (qd_conn->policy_settings->maxSenders) {
        if (qd_conn->n_senders == qd_conn->policy_settings->maxSenders) {
            // Max sender limit specified and violated.
            _qd_policy_deny_amqp_sender_link(pn_link, qd_conn);
            return false;
        } else {
            // max sender limit not violated
        }
    } else {
        // max sender limit not specified
    }
    // TODO: Deny sender link based on target
    return true;
}


bool qd_policy_approve_amqp_receiver_link(pn_link_t *pn_link, qd_connection_t *qd_conn)
{
    if (qd_conn->policy_settings->maxReceivers) {
        if (qd_conn->n_receivers == qd_conn->policy_settings->maxReceivers) {
            // Max sender limit specified and violated.
            _qd_policy_deny_amqp_receiver_link(pn_link, qd_conn);
            return false;
        } else {
            // max receiver limit not violated
        }
    } else {
        // max receiver limit not specified
    }
    // TODO: Deny receiver link based on source
    return true;
}


//
//
void qd_policy_amqp_open(void *context, bool discard)
{
    qd_connection_t *qd_conn = (qd_connection_t *)context;
    if (!discard) {
        pn_connection_t *conn = qd_connection_pn(qd_conn);
        qd_dispatch_t *qd = qd_conn->server->qd;
        qd_policy_t *policy = qd->policy;
        bool connection_allowed = true;

        if (policy->enableAccessRules) {
            // Open connection or not based on policy.
            // username = pn_connection_get_user(conn) returns blank when
            // the transport returns 'anonymous'.
            pn_transport_t *pn_trans = pn_connection_transport(conn);
            const char *username = pn_transport_get_user(pn_trans);

            const char *hostip = qdpn_connector_hostip(qd_conn->pn_cxtr);
            const char *app = pn_connection_remote_hostname(conn);
            const char *conn_name = qdpn_connector_name(qd_conn->pn_cxtr);
#define SETTINGS_NAME_SIZE 256
            char settings_name[SETTINGS_NAME_SIZE];
            uint32_t conn_id = qd_conn->connection_id;
            qd_conn->policy_settings = NEW(qd_policy_settings_t); // TODO: memory pool for settings
            memset(qd_conn->policy_settings, 0, sizeof(qd_policy_settings_t));

            if (qd_policy_open_lookup_user(policy, username, hostip, app, conn_name, 
                                           settings_name, SETTINGS_NAME_SIZE, conn_id,
                                           qd_conn->policy_settings) &&
                settings_name[0]) {
                // This connection is allowed by policy.
                // Apply tranport policy settings
                if (qd_conn->policy_settings->maxFrameSize > 0)
                    pn_transport_set_max_frame(pn_trans, qd_conn->policy_settings->maxFrameSize);
                if (qd_conn->policy_settings->maxSessions > 0)
                    pn_transport_set_channel_max(pn_trans, qd_conn->policy_settings->maxSessions);
            } else {
                // This connection is denied by policy.
                connection_allowed = false;
                qd_policy_private_deny_amqp_connection(conn, RESOURCE_LIMIT_EXCEEDED, CONNECTION_DISALLOWED);
            }
        } else {
            // This connection not subject to policy and implicitly allowed.
            // Note that connections not goverened by policy have no policy_settings.
        }
        if (connection_allowed) {
            if (pn_connection_state(conn) & PN_LOCAL_UNINIT)
                pn_connection_open(conn);
            qd_connection_manager_connection_opened(qd_conn);
        }
    }
    qd_connection_set_event_stall(qd_conn, false);
}