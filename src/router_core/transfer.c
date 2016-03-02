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

#include "router_core_private.h"
#include <qpid/dispatch/amqp.h>
#include <stdio.h>


static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_update_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard);

//==================================================================================
// Internal Functions
//==================================================================================


//==================================================================================
// Interface Functions
//==================================================================================

qdr_delivery_t *qdr_link_deliver(qdr_link_t *link, qd_message_t *msg, qd_field_iterator_t *ingress,
                                 bool settled, qd_bitmask_t *link_exclusion)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    dlv->link    = link;
    dlv->msg     = msg;
    dlv->to_addr = 0;
    dlv->origin  = ingress;
    dlv->settled = settled;

    action->args.connection.delivery = dlv;
    action->args.connection.link_exclusion = link_exclusion;
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to(qdr_link_t *link, qd_message_t *msg,
                                    qd_field_iterator_t *ingress, qd_field_iterator_t *addr,
                                    bool settled, qd_bitmask_t *link_exclusion)
{
    qdr_action_t   *action = qdr_action(qdr_link_deliver_CT, "link_deliver");
    qdr_delivery_t *dlv    = new_qdr_delivery_t();

    ZERO(dlv);
    dlv->link    = link;
    dlv->msg     = msg;
    dlv->to_addr = addr;
    dlv->origin  = ingress;
    dlv->settled = settled;

    action->args.connection.delivery = dlv;
    action->args.connection.link_exclusion = link_exclusion;
    qdr_action_enqueue(link->core, action);
    return dlv;
}


qdr_delivery_t *qdr_link_deliver_to_routed_link(qdr_link_t *link, qd_message_t *msg)
{
    // TODO - Implement this.  Bypass the CT?

    //
    // We might wish to run link-routed transfers and updates through the core in order to
    // track the number of outstanding deliveries and to have the ability to intervene in
    // flow control.
    //
    // Use case: Quiescing a broker.  To do this, all inbound links to the broker shall be
    // idled by preventing the propagation of flow credit out of the broker.  This will dry
    // the transfer of inbound deliveries, allow all existing deliveries to be settled, and
    // allow the router to know when it is safe to detach the inbound links.  Outbound links
    // can also be detached after all deliveries are settled and "drained" indications are
    // received.
    //
    // Waypoint disconnect procedure:
    //   1) Block flow-credit propagation for link outbound to waypoint.
    //   2) Wait for the number of unsettled outbound deliveries to go to zero.
    //   3) Detach the outbound link.
    //   4) Wait for inbound link to be drained with zero unsettled deliveries.
    //   5) Detach inbound link.
    //

    return 0;
}


void qdr_link_process_deliveries(qdr_core_t *core, qdr_link_t *link, int credit)
{
    qdr_connection_t *conn = link->conn;
    qdr_delivery_t   *dlv;
    bool              drained = false;
    int               offer;

    while (credit > 0 && !drained) {
        sys_mutex_lock(conn->work_lock);
        dlv = DEQ_HEAD(link->undelivered);
        if (dlv) {
            DEQ_REMOVE_HEAD(link->undelivered);
            if (!dlv->settled)
                DEQ_INSERT_TAIL(link->unsettled, dlv);
            credit--;
            link->total_deliveries++;
            offer = DEQ_SIZE(link->undelivered);
        } else
            drained = true;
        sys_mutex_unlock(conn->work_lock);

        if (dlv) {
            core->deliver_handler(core->user_context, link, dlv, dlv->settled);
            if (dlv->settled)
                qdr_delivery_free(dlv);
        }
    }

    if (drained)
        core->drained_handler(core->user_context, link);
    else
        core->offer_handler(core->user_context, link, offer);

    //
    // Handle disposition/settlement updates
    //
    qdr_delivery_ref_list_t updated_deliveries;
    sys_mutex_lock(conn->work_lock);
    DEQ_MOVE(link->updated_deliveries, updated_deliveries);
    sys_mutex_unlock(conn->work_lock);

    qdr_delivery_ref_t *ref = DEQ_HEAD(updated_deliveries);
    while (ref) {
        core->delivery_update_handler(core->user_context, ref->dlv, ref->dlv->disposition, ref->dlv->settled);
        if (ref->dlv->settled)
            qdr_delivery_free(ref->dlv);
        qdr_del_delivery_ref(&updated_deliveries, ref);
        ref = DEQ_HEAD(updated_deliveries);
    }
}


void qdr_link_flow(qdr_core_t *core, qdr_link_t *link, int credit, bool drain_mode)
{
    qdr_action_t *action = qdr_action(qdr_link_flow_CT, "link_flow");
    action->args.connection.link   = link;
    action->args.connection.credit = credit;
    action->args.connection.drain  = drain_mode;

    qdr_action_enqueue(core, action);
}


void qdr_send_to1(qdr_core_t *core, qd_message_t *msg, qd_field_iterator_t *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field_from_iter(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


void qdr_send_to2(qdr_core_t *core, qd_message_t *msg, const char *addr, bool exclude_inprocess, bool control)
{
    qdr_action_t *action = qdr_action(qdr_send_to_CT, "send_to");
    action->args.io.address           = qdr_field(addr);
    action->args.io.message           = qd_message_copy(msg);
    action->args.io.exclude_inprocess = exclude_inprocess;
    action->args.io.control           = control;

    qdr_action_enqueue(core, action);
}


void qdr_delivery_free(qdr_delivery_t *delivery)
{
    if (delivery->msg)
        qd_message_free(delivery->msg);
    if (delivery->to_addr)
        qd_field_iterator_free(delivery->to_addr);
    free_qdr_delivery_t(delivery);
}


void qdr_delivery_update_disposition(qdr_core_t *core, qdr_delivery_t *delivery, uint64_t disposition, bool settled)
{
    qdr_action_t *action = qdr_action(qdr_update_delivery_CT, "update_delivery");
    action->args.delivery.delivery    = delivery;
    action->args.delivery.disposition = disposition;
    action->args.delivery.settled     = settled;

    qdr_action_enqueue(core, action);
}


void qdr_delivery_set_context(qdr_delivery_t *delivery, void *context)
{
    delivery->context = context;
}


void *qdr_delivery_get_context(qdr_delivery_t *delivery)
{
    return delivery->context;
}


void qdr_delivery_tag(const qdr_delivery_t *delivery, const char **tag, int *length)
{
    *tag    = (const char*) &delivery->tag;
    *length = sizeof(uint64_t);
}


qd_message_t *qdr_delivery_message(const qdr_delivery_t *delivery)
{
    return delivery->msg;
}


//==================================================================================
// In-Thread Functions
//==================================================================================

/**
 * Check the link's accumulated credit.  If the credit given to the connection thread
 * has been issued to Proton, provide the next batch of credit to the connection thread.
 */
void qdr_link_issue_credit_CT(qdr_core_t *core, qdr_link_t *link, int credit)
{
    link->incremental_credit_CT += credit;

    if (link->incremental_credit_CT && link->incremental_credit == 0) {
        //
        // Move the credit from the core-thread value to the connection-thread value.
        //
        link->incremental_credit    = link->incremental_credit_CT;
        link->incremental_credit_CT = 0;

        //
        // Put this link on the connection's has-credit list.
        //
        sys_mutex_lock(link->conn->work_lock);
        qdr_add_link_ref(&link->conn->links_with_credit, link, QDR_LINK_LIST_CLASS_FLOW);
        sys_mutex_unlock(link->conn->work_lock);

        //
        // Activate the connection
        //
        qdr_connection_activate_CT(core, link->conn);
    }
}


static void qdr_link_flow_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_link_t *link = action->args.connection.link;
    int  credit = action->args.connection.credit;
    //bool drain  = action->args.connection.drain;
    bool activate = false;

    //
    // TODO - If this is a link-routed link, propagate the flow data downrange.
    //

    //
    // Handle the replenishing of credit outbound
    //
    if (link->link_direction == QD_OUTGOING && credit > 0) {
        sys_mutex_lock(link->conn->work_lock);
        if (DEQ_SIZE(link->undelivered) > 0) {
            qdr_add_link_ref(&link->conn->links_with_deliveries, link, QDR_LINK_LIST_CLASS_DELIVERY);
            activate = true;
        }
        sys_mutex_unlock(link->conn->work_lock);
    }

    if (activate)
        qdr_connection_activate_CT(core, link->conn);
}


static void qdr_link_deliver_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_delivery_t *dlv          = action->args.connection.delivery;
    qd_bitmask_t   *link_exclude = action->args.connection.link_exclusion;
    qdr_link_t     *link         = dlv->link;
    int             fanout       = 0;
    bool            presettled   = dlv->settled;

    //
    // NOTE: The link->undelivered list does not need to be protected by the
    //       connection's work lock for incoming links.  This protection is only
    //       needed for outgoing links.
    //

    if (DEQ_IS_EMPTY(link->undelivered)) {
        qdr_address_t *addr = link->owning_addr;
        if (!addr && dlv->to_addr)
            qd_hash_retrieve(core->addr_hash, dlv->to_addr, (void**) &addr);
        if (addr) {
            fanout = qdr_forward_message_CT(core, addr, dlv->msg, dlv, false,
                                            link->link_type == QD_LINK_CONTROL, link_exclude);
            if (link->link_type != QD_LINK_CONTROL && link->link_type != QD_LINK_ROUTER)
                addr->deliveries_ingress++;
            link->total_deliveries++;
        }
    }

    if (fanout == 0) {
        printf("TODO fanout == 0\n");
        if (link->owning_addr) {
            //
            // Message was not delivered and the link is not anonymous.
            // Queue the message for later delivery (when the address gets
            // a valid destination).
            //
            DEQ_INSERT_TAIL(link->undelivered, dlv);
        } else {
            //
            // TODO - Release the delivery
            //
        }
    } else if (fanout == 1) {
        if (dlv->settled) {
            //
            // The delivery is settled.  Keep it off the unsettled list and issue
            // replacement credit for it now.
            //
            qdr_link_issue_credit_CT(core, link, 1);

            //
            // If the delivery was pre-settled, free it now.
            //
            if (presettled) {
                assert(!dlv->peer);
                qdr_delivery_free(dlv);
            }
        } else
            DEQ_INSERT_TAIL(link->unsettled, dlv);
    } else {
        //
        // The fanout is greater than one.  Do something!  TODO
        //

        if (presettled) {
            qdr_link_issue_credit_CT(core, link, 1);
            assert(!dlv->peer);
            qdr_delivery_free(dlv);
        }
    }
}


static void qdr_send_to_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    qdr_field_t  *addr_field = action->args.io.address;
    qd_message_t *msg        = action->args.io.message;

    if (!discard) {
        qdr_address_t *addr;

        qd_address_iterator_reset_view(addr_field->iterator, ITER_VIEW_ADDRESS_HASH);
        qd_hash_retrieve(core->addr_hash, addr_field->iterator, (void**) &addr);
        if (addr) {
            //
            // Forward the message.  We don't care what the fanout count is.
            //
            (void) qdr_forward_message_CT(core, addr, msg, 0, action->args.io.exclude_inprocess,
                                          action->args.io.control, 0);
            addr->deliveries_from_container++;
        } else
            qd_log(core->log, QD_LOG_DEBUG, "In-process send to an unknown address");
    }

    qdr_field_free(addr_field);
    qd_message_free(msg);
}


static void qdr_update_delivery_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    qdr_delivery_t *dlv     = action->args.delivery.delivery;
    qdr_delivery_t *peer    = dlv->peer;
    bool            push    = false;
    uint64_t        disp    = action->args.delivery.disposition;
    bool            settled = action->args.delivery.settled;

    //
    // Logic:
    //
    // If disposition has changed and there is a peer link, set the disposition of the peer
    // If settled, the delivery must be unlinked and freed.
    // If settled and there is a peer, the peer shall be settled and unlinked.  It shall not
    //   be freed until the connection-side thread settles the PN delivery.
    //

    if (disp != dlv->disposition) {
        //
        // Disposition has changed, propagate the change to the peer delivery.
        //
        dlv->disposition = disp;
        if (peer) {
            peer->disposition = disp;
            push = true;
        }
    }

    if (settled) {
        if (peer) {
            peer->settled = true;
            push = true;
            peer->peer = 0;
            dlv->peer  = 0;
            if (peer->link) {
                sys_mutex_lock(peer->link->conn->work_lock);
                DEQ_REMOVE(peer->link->unsettled, peer);
                sys_mutex_unlock(peer->link->conn->work_lock);
                if (peer->link->link_direction == QD_INCOMING)
                    qdr_link_issue_credit_CT(core, peer->link, 1);
            }
        }

        if (dlv->link) {
            sys_mutex_lock(dlv->link->conn->work_lock);
            DEQ_REMOVE(dlv->link->unsettled, dlv);
            sys_mutex_unlock(dlv->link->conn->work_lock);
            if (dlv->link->link_direction == QD_INCOMING)
                qdr_link_issue_credit_CT(core, dlv->link, 1);
        }

        qdr_delivery_free(dlv);
    }

    if (push)
        qdr_delivery_push_CT(core, peer);
}


void qdr_delivery_push_CT(qdr_core_t *core, qdr_delivery_t *dlv)
{
    if (!dlv || !dlv->link)
        return;

    qdr_link_t *link = dlv->link;

    sys_mutex_lock(link->conn->work_lock);
    qdr_add_delivery_ref(&link->updated_deliveries, dlv);

    //
    // Put this link on the connection's list of links with delivery activity.
    //
    qdr_add_link_ref(&link->conn->links_with_deliveries, link, QDR_LINK_LIST_CLASS_DELIVERY);
    sys_mutex_unlock(link->conn->work_lock);

    //
    // Activate the connection
    //
    qdr_connection_activate_CT(core, link->conn);
}