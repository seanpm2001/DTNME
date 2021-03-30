/*
 *    Copyright 2006-2007 The MITRE Corporation
 * 
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 
 *        http://www.apache.org/licenses/LICENSE-2.0
 * 
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 *
 *    The US Government will not be charged any license fee and/or royalties
 *    related to this software. Neither name of The MITRE Corporation; nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 */

/*
 *    Modifications made to this file by the patch file dtn2_mfs-33289-1.patch
 *    are Copyright 2015 United States Government as represented by NASA
 *       Marshall Space Flight Center. All Rights Reserved.
 *
 *    Released under the NASA Open Source Software Agreement version 1.3;
 *    You may obtain a copy of the Agreement at:
 * 
 *        http://ti.arc.nasa.gov/opensource/nosa/
 * 
 *    The subject software is provided "AS IS" WITHOUT ANY WARRANTY of any kind,
 *    either expressed, implied or statutory and this agreement does not,
 *    in any manner, constitute an endorsement by government agency of any
 *    results, designs or products resulting from use of the subject software.
 *    See the Agreement for the specific language governing permissions and
 *    limitations.
 */

#ifdef HAVE_CONFIG_H
#  include <dtn-config.h>
#endif


#include <memory>
#include <iostream>
#include <map>
#include <vector>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <cctype>

#include "ExternalRouter.h"
#include "bundling/GbofId.h"
#include "bundling/BundleDaemon.h"
#include "bundling/BundleActions.h"
#include "bundling/BP6_MetadataBlockProcessor.h"
#include "contacts/ContactManager.h"
#include "contacts/NamedAttribute.h"
#include "conv_layers/TCPConvergenceLayer.h"
#include "conv_layers/UDPConvergenceLayer.h"
#include "reg/RegistrationTable.h"
#include "conv_layers/ConvergenceLayer.h"

#include "conv_layers/LTPUDPConvergenceLayer.h"
//#include "conv_layers/LTPUDPReplayConvergenceLayer.h"

#include <third_party/oasys/io/UDPClient.h>
#include <third_party/oasys/tclcmd/TclCommand.h>
#include <third_party/oasys/io/IO.h>
#include <third_party/oasys/io/NetUtils.h>

namespace dtn {


ExternalRouter::ExternalRouter()
    : BundleRouter("ExternalRouter", "external"),
      ExternalRouterServerIF("ExternalRouter"),
      initialized_(false)
{
    shutting_down_ = false;


    // CLANG forces initailization of these static parameters in the constructor 
    network_interface_   = htonl(INADDR_LOOPBACK);

    log_notice("Creating ExternalRouter");
}

ExternalRouter::~ExternalRouter()
{
    delete route_table_;

    if (reg_) {
        delete reg_;
    }
}

// Initialize inner classes
void
ExternalRouter::initialize()
{
    log_notice("Initializing ExternalRouter");

    send_seq_ctr_ = 0;

    BundleDaemon* bd = BundleDaemon::instance();

    if (bd->params_.announce_ipn_)
        set_server_eid(bd->local_eid_ipn().str());
    else
        set_server_eid(bd->local_eid().str());


    // Create the static route table
    route_table_ = new RouteTable("external");

    // Register as a client app with the forwarder
    //dzdebug reg_ = new ERRegistration(this);
    reg_ = nullptr;

    // Register the global shutdown function
    BundleDaemon::instance()->set_rtr_shutdown(
        external_rtr_shutdown, (void *) 0);

    // Start module server thread
    srv_ = QPtr_ModuleServer(new ModuleServer());
    srv_->start();

    // Create a hello timer
    hello_ = std::make_shared<HelloTimer>(this);

    initialized_ = true;

    // send an initial hello message
    server_send_hello_msg(0, 0);

    hello_->start(ExternalRouter::hello_interval, hello_);
}

void
ExternalRouter::shutdown()
{
    shutting_down_ = true;

    hello_->cancel();
    hello_.reset();

    std::string alert = "shuttingDown";
    server_send_alert_msg(alert);

    usleep(100000);

    //XXX/dz - TODO - shutdown cleanly - segfault in ModuleServer if delete while processing

    srv_->do_shutdown();
    srv_.reset();

}

// Format the given StringBuffer with static routing info
//----------------------------------------------------------------------
void
ExternalRouter::get_routing_state(oasys::StringBuffer* buf)
{
    if (!shutting_down_) {
        buf->appendf("Static route table for %s router(s):\n", name_.c_str());
        route_table_->dump(buf);
    }
}

//----------------------------------------------------------------------
std::string
ExternalRouter::lowercase(const char *c_str)
{
    std::string str(c_str);
    transform (str.begin(), str.end(), str.begin(), 
               [](unsigned char c) { return std::tolower(c); } );
    return str;
}

//----------------------------------------------------------------------
bool
ExternalRouter::can_delete_bundle(const BundleRef& bundle)
{
    // check if we haven't yet done anything with this bundle
    if (bundle->fwdlog()->get_count(ForwardingInfo::TRANSMITTED |
                                    ForwardingInfo::DELIVERED) == 0)
    {
        //log_debug("ExternalRouter::can_delete_bundle(%" PRIbid "): "
        //          "not yet transmitted or delivered",
        //          bundle->bundleid());
        return false;
    }

    // check if we have local custody
    if (bundle->local_custody() || bundle->bibe_custody()) {
        //log_debug("ExternalRouter::can_delete_bundle(%" PRIbid "): "
        //          "not deleting because we have custody",
        //          bundle->bundleid());
        return false;
    }

    if (bundle->ecos_critical()) {
        //log_debug("ExternalRouter::can_delete_bundle(%" PRIbid "): "
        //          "not deleting because ECOS critical bundles can be sent over multiple links",
        //          bundle->bundleid());
        return false;
    }


    //XXX/dz - Need to add support for sessions??
    //
    // check if the bundle is part of a session with subscribers
    //Session* session = get_session_for_bundle(bundle.object());
    //if (session && !session->subscribers().empty())
    //{
    //    log_debug("ExternalRouter::can_delete_bundle(%u): "
    //              "session has subscribers",
    //              bundle->bundleid());
    //    return false;
    //}

    return true;
}
 
bool
ExternalRouter::accept_custody(Bundle* bundle)
{
    // External part of this router will issue a request to accept 
    // custody of the bundle after getting the BundleReceived 
    // message if it decides it is warranted.

    (void)bundle;
    return false;
}



// Serialize events and UDP multicast to external routers
void
ExternalRouter::handle_event(BundleEvent *event)
{
    if (initialized_ && !shutting_down_) {
        dispatch_event(event);
    }
}


void
ExternalRouter::handle_bundle_received(BundleReceivedEvent *event)
{
    extrtr_bundle_ptr_t bundleptr;
    extrtr_bundle_vector_t bundle_vec;

    // build vector of one bundle

    bundleptr = std::make_shared<extrtr_bundle_t>();

    bundleptr->bundleid_ = event->bundleref_->bundleid();
    bundleptr->bp_version_ = event->bundleref_->bp_version();
    bundleptr->custodyid_ = event->bundleref_->custodyid();
    bundleptr->source_ = event->bundleref_->source().str();
    bundleptr->dest_ = event->bundleref_->dest().str();
//            bundleptr->custodian_ = bref->custodian().str();
//            bundleptr->replyto_ = event->bundleref_->replyto().str();
    bundleptr->gbofid_str_ = event->bundleref_->gbofid_str();
    bundleptr->prev_hop_ = event->bundleref_->prevhop().str();
    bundleptr->length_ = event->bundleref_->durable_size();
//            bundleptr->is_fragment_ = event->bundleref_->is_fragment();
//            bundleptr->is_admin_ = event->bundleref_->is_admin();
    bundleptr->priority_ = event->bundleref_->priority();
#ifdef ECOS_ENABLED
    bundleptr->ecos_flags_ = event->bundleref_->ecos_flags();
    bundleptr->ecos_ordinal_ = event->bundleref_->ecos_ordinal();
    bundleptr->ecos_flowlabel_ = event->bundleref_->ecos_flowlabel();
#endif
    bundleptr->custody_requested_ = event->bundleref_->custody_requested();
    bundleptr->local_custody_ = event->bundleref_->local_custody() || event->bundleref_->bibe_custody();
    bundleptr->singleton_dest_ = event->bundleref_->singleton_dest();

    bundleptr->expired_in_transit_ = event->expired_in_transit_;

    bundle_vec.push_back(bundleptr);


    // Pull the remote address from the CLInfo if it is available
    // which is only from TCP connections 
    std::string link_id = "";
    if (event->link_ != nullptr) {
        link_id = event->link_->name();
    }

    //log_always("handle_bundle_received - send message");

    server_send_bundle_received_msg(link_id, bundle_vec);
}

void
ExternalRouter::handle_bundle_transmitted(BundleTransmittedEvent* event)
{
    //log_always("handle_bundle_transmitted - send message");

    // Note: bytes_sent == 0 indicates failed transmission
    std::string link_id = event->link_->name_str();
    server_send_bundle_transmitted_msg(link_id, event->bundleref_->bundleid(), 
                                       event->bytes_sent_);
}

void
ExternalRouter::handle_bundle_delivered(BundleDeliveredEvent* event)
{
    server_send_bundle_delivered_msg(event->bundleref_->bundleid());
}

void
ExternalRouter::handle_bundle_expired(BundleExpiredEvent* event)
{
    //log_always("handle_bundle_expired - send message");

    server_send_bundle_expired_msg(event->bundleref_->bundleid());
}


void
ExternalRouter::handle_bundle_cancelled(BundleSendCancelledEvent* event)
{
    //log_always("handle_bundle_cancelled - send message");

    server_send_bundle_cancelled_msg(event->bundleref_->bundleid());
}

void
ExternalRouter::handle_custody_timeout(CustodyTimeoutEvent* event)
{
    //log_always("handle_custody_timeout - send message");

    server_send_custody_timeout_msg(event->bundle_id_);
}

void ExternalRouter::handle_bundle_custody_accepted(BundleCustodyAcceptedEvent* event)
{
    //log_always("handle_custody_accepted - send message");

    uint64_t custody_id = event->bundleref_->custodyid();
    server_send_custody_accepted_msg(event->bundleref_->bundleid(), custody_id);
}


void
ExternalRouter::handle_custody_signal(CustodySignalEvent* event)
{
    (void) event;
    //log_always("handle_custody_signal - send message");

    // this event is ignored and handle_custody_released is used for
    // BIBE releases, ACS releases and regular Custody Signals
    //server_send_custody_signal_msg(event->bundle_id_, event->data_.succeeded_, event->data_.reason_);
}


void
ExternalRouter::handle_custody_released(uint64_t bundleid, bool succeeded, int reason)
{
    //log_always("handle_custody_released - send message");

    server_send_custody_signal_msg(bundleid, succeeded, reason);
}




















void
ExternalRouter::handle_bundle_injected(BundleInjectedEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_contact_up(ContactUpEvent* event)
{
    Contact* contact = event->contact_.object();
    Link* link = contact->link().object();


    extrtr_link_ptr_t linkptr;
    extrtr_link_vector_t link_vec;

    // build vector of one link

    linkptr = std::make_shared<extrtr_link_t>();

    linkptr->link_id_ = link->name_str();

    linkptr->remote_eid_ = link->remote_eid().str();
    linkptr->conv_layer_ = link->cl_name_str();
    linkptr->link_state_ = lowercase(link->state_to_str(link->state()));
    linkptr->next_hop_ = link->nexthop();


    if (linkptr->conv_layer_.compare("tcp") == 0) {
        typedef TCPConvergenceLayer::TCPLinkParams tcp_params;
        tcp_params *params = dynamic_cast<tcp_params*>(link->cl_info());
        if (params) {
            //oasys::Intoa local_addr(params->local_addr_);
            //linkptr->local_addr_ = local_addr.buf();
            //linkptr->local_port_ = params->remote_port_;
            oasys::Intoa remote_addr(params->remote_addr_);
            linkptr->remote_addr_ = remote_addr.buf();
            linkptr->remote_port_ = params->remote_port_;
            // not applicable   linkptr->rate_ = params->rate_;
        }
    } else if (linkptr->conv_layer_.compare("udp") == 0) {
        typedef UDPConvergenceLayer::Params udp_params;
        udp_params *params = dynamic_cast<udp_params*>(link->cl_info());
        if (params) {
            //oasys::Intoa local_addr(params->local_addr_);
            //linkptr->local_addr_ = local_addr.buf();
            //linkptr->local_port_ = params->remote_port_;
            oasys::Intoa remote_addr(params->remote_addr_);
            linkptr->remote_addr_ = remote_addr.buf();
            linkptr->remote_port_ = params->remote_port_;
            linkptr->rate_ = params->rate_;
        }
    } else {

        if (linkptr->conv_layer_.compare("ltpudp") == 0) {
            typedef LTPUDPConvergenceLayer::Params ltp_params;
            ltp_params *params = dynamic_cast<ltp_params*>(link->cl_info());
            if (params) {
                //oasys::Intoa local_addr(params->local_addr_);
                //linkptr->local_addr_ = local_addr.buf();
                //linkptr->local_port_ = params->remote_port_;
                oasys::Intoa remote_addr(params->remote_addr_);
                linkptr->remote_addr_ = remote_addr.buf();
                linkptr->remote_port_ = params->remote_port_;
                linkptr->rate_ = params->rate_;
            }

        } else if (linkptr->conv_layer_.compare("ltpudpreplay") == 0) {
//dzdebug            typedef LTPUDPReplayConvergenceLayer::Params ltp_params;
//dzdebug            ltp_params *params = dynamic_cast<ltp_params*>(link->cl_info());
//dzdebug            if (params) {
//dzdebug                //oasys::Intoa local_addr(params->local_addr_);
//dzdebug                //linkptr->local_addr_ = local_addr.buf();
//dzdebug                //linkptr->local_port_ = params->remote_port_;
//dzdebug                oasys::Intoa remote_addr(params->remote_addr_);
//dzdebug                linkptr->remote_addr_ = remote_addr.buf();
//dzdebug                linkptr->remote_port_ = params->remote_port_;
//dzdebug                linkptr->rate_ = params->rate_;
//dzdebug            }
        }

    }

    link_vec.push_back(linkptr);

    server_send_link_opened_msg(link_vec);
}

void
ExternalRouter::handle_contact_down(ContactDownEvent* event)
{
    (void) event;
    std::string link_id = event->contact_->link()->name_str();
    server_send_link_closed_msg(link_id);
    return;
}

void
ExternalRouter::handle_link_created(LinkCreatedEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_link_deleted(LinkDeletedEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_link_available(LinkAvailableEvent *event)
{
    std::string link_id = event->link_->name();
    server_send_link_available_msg(link_id);
}

void
ExternalRouter::handle_link_unavailable(LinkUnavailableEvent *event)
{
    std::string link_id = event->link_->name();
    server_send_link_unavailable_msg(link_id);
}

void
ExternalRouter::handle_link_attribute_changed(LinkAttributeChangedEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_new_eid_reachable(NewEIDReachableEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_contact_attribute_changed(ContactAttributeChangedEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_registration_added(RegistrationAddedEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_registration_removed(RegistrationRemovedEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_registration_expired(RegistrationExpiredEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_route_add(RouteAddEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_route_del(RouteDelEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_external_router_acs(ExternalRouterAcsEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_link_report(LinkReportEvent *event)
{
    (void) event;

    BundleDaemon *bd = BundleDaemon::instance();
    oasys::ScopeLock l(bd->contactmgr()->lock(), __func__);


    const LinkSet *links = bd->contactmgr()->links();
    LinkSet::const_iterator iter = links->begin();
    LinkSet::const_iterator end = links->end();

    Contact* contact;

    extrtr_link_ptr_t linkptr;
    extrtr_link_vector_t link_vec;

    // build vector of links
    for(; iter != end; ++iter) {
        linkptr = std::make_shared<extrtr_link_t>();

        contact = (*iter)->contact().object();

        if ((contact == nullptr) || (contact->link() == nullptr)) {
            continue;
        }

        linkptr->link_id_ = contact->link()->name_str();

        linkptr->remote_eid_ = contact->link()->remote_eid().str();
        linkptr->conv_layer_ = contact->link()->cl_name_str();
        linkptr->link_state_ = lowercase(contact->link()->state_to_str(contact->link()->state()));
        linkptr->next_hop_ = contact->link()->nexthop();


        if (linkptr->conv_layer_.compare("tcp") == 0) {
            typedef TCPConvergenceLayer::TCPLinkParams tcp_params;
            tcp_params *params = dynamic_cast<tcp_params*>(contact->link()->cl_info());
            if (params) {
                //oasys::Intoa local_addr(params->local_addr_);
                //linkptr->local_addr_ = local_addr.buf();
                //linkptr->local_port_ = params->remote_port_;
                oasys::Intoa remote_addr(params->remote_addr_);
                linkptr->remote_addr_ = remote_addr.buf();
                linkptr->remote_port_ = params->remote_port_;
                // not applicable   linkptr->rate_ = params->rate_;
            }
        } else if (linkptr->conv_layer_.compare("udp") == 0) {
            typedef UDPConvergenceLayer::Params udp_params;
            udp_params *params = dynamic_cast<udp_params*>(contact->link()->cl_info());
            if (params) {
                //oasys::Intoa local_addr(params->local_addr_);
                //linkptr->local_addr_ = local_addr.buf();
                //linkptr->local_port_ = params->remote_port_;
                oasys::Intoa remote_addr(params->remote_addr_);
                linkptr->remote_addr_ = remote_addr.buf();
                linkptr->remote_port_ = params->remote_port_;
                linkptr->rate_ = params->rate_;
            }
        } else {

            if (linkptr->conv_layer_.compare("ltpudp") == 0) {
                typedef LTPUDPConvergenceLayer::Params ltp_params;
                ltp_params *params = dynamic_cast<ltp_params*>(contact->link()->cl_info());
                if (params) {
                    //oasys::Intoa local_addr(params->local_addr_);
                    //linkptr->local_addr_ = local_addr.buf();
                    //linkptr->local_port_ = params->remote_port_;
                    oasys::Intoa remote_addr(params->remote_addr_);
                    linkptr->remote_addr_ = remote_addr.buf();
                    linkptr->remote_port_ = params->remote_port_;
                    linkptr->rate_ = params->rate_;
                }

            } else if (linkptr->conv_layer_.compare("ltpudpreplay") == 0) {
//dzdebug                typedef LTPUDPReplayConvergenceLayer::Params ltp_params;
//dzdebug                ltp_params *params = dynamic_cast<ltp_params*>(contact->link()->cl_info());
//dzdebug                if (params) {
//dzdebug                    //oasys::Intoa local_addr(params->local_addr_);
//dzdebug                    //linkptr->local_addr_ = local_addr.buf();
//dzdebug                    //linkptr->local_port_ = params->remote_port_;
//dzdebug                    oasys::Intoa remote_addr(params->remote_addr_);
//dzdebug                    linkptr->remote_addr_ = remote_addr.buf();
//dzdebug                    linkptr->remote_port_ = params->remote_port_;
//dzdebug                    linkptr->rate_ = params->rate_;
//dzdebug                }
            }

        }

        link_vec.push_back(linkptr);
    }

    log_always("Sending Link Report to External Router TCP Client");

    // always send even if the list is empty
    server_send_link_report_msg(link_vec);
}

void
ExternalRouter::handle_link_attributes_report(LinkAttributesReportEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_contact_report(ContactReportEvent* event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_bundle_report(BundleReportEvent *event)
{
    (void) event;

    BundleDaemon *bd = BundleDaemon::instance();
    BundleRef bref("generate_bundle_report_from_map");

    //log_debug("generata_bundle_report_from_map - pending_bundles size %zu", bd->pending_bundles()->size());
    pending_bundles_t* bundles = bd->pending_bundles();

    bundles->lock()->lock(__func__);
    pending_bundles_t::iterator iter = bundles->begin();

    if (iter != bundles->end()) {
        bref = iter->second;
    }
    bundles->lock()->unlock();

    extrtr_bundle_ptr_t bundleptr;
    extrtr_bundle_vector_t bundle_vec;

    // build vector of bundles

    while (bref != nullptr) {

        // 25 bundles per message is ~15-20 KB (unless EIDs get large)
        // 2020-06-23 dz - changing to 10000 per message since only using TCP now
        //     10,000 bundles is about 800K
        while ((bref != nullptr) &&  (bundle_vec.size() < 10000)) {

            bundleptr = std::make_shared<extrtr_bundle_t>();

            bundleptr->bundleid_ = bref->bundleid();
            bundleptr->bp_version_ = bref->bp_version();
            bundleptr->custodyid_ = bref->custodyid();
            bundleptr->source_ = bref->source().str();
            bundleptr->dest_ = bref->dest().str();
//            bundleptr->custodian_ = bref->custodian().str();
//            bundleptr->replyto_ = bref->replyto().str();
            bundleptr->gbofid_str_ = bref->gbofid_str();
            bundleptr->prev_hop_ = bref->prevhop().str();
            bundleptr->length_ = bref->durable_size();
//            bundleptr->is_fragment_ = bref->is_fragment();
//            bundleptr->is_admin_ = bref->is_admin();
            bundleptr->priority_ = bref->priority();
#ifdef ECOS_ENABLED
            bundleptr->ecos_flags_ = bref->ecos_flags();
            bundleptr->ecos_ordinal_ = bref->ecos_ordinal();
            bundleptr->ecos_flowlabel_ = bref->ecos_flowlabel();
#endif
            bundleptr->custody_requested_ = bref->custody_requested();
            bundleptr->local_custody_ = bref->local_custody() || bref->bibe_custody();
            bundleptr->singleton_dest_ = bref->singleton_dest();

            bundle_vec.push_back(bundleptr);


            bref = bundles->find_next(bref->bundleid());
        }

        if (bundle_vec.size() > 0) {
            server_send_bundle_report_msg(bundle_vec);

            //XXX/dz could get real efficient and re-use the loaded vector...
            bundle_vec.clear();
        }
    }
}


void
ExternalRouter::handle_bundle_attributes_report(BundleAttributesReportEvent *event)
{
    (void) event;
    //dzdebug
    return;
}

void
ExternalRouter::handle_route_report(RouteReportEvent* event)
{
    (void) event;
    //dzdebug
    return;
}


void
ExternalRouter::send_msg(std::string* msg)
{
    srv_->post_to_send(msg);
}

const char *
ExternalRouter::reason_to_str(int reason)
{
    switch(reason) {
        case ContactEvent::NO_INFO:     return "no_info";
        case ContactEvent::USER:        return "user";
        case ContactEvent::SHUTDOWN:    return "shutdown";
        case ContactEvent::BROKEN:      return "broken";
        case ContactEvent::CL_ERROR:    return "cl_error";
        case ContactEvent::CL_VERSION:  return "cl_version";
        case ContactEvent::RECONNECT:   return "reconnect";
        case ContactEvent::IDLE:        return "idle";
        case ContactEvent::TIMEOUT:     return "timeout";
        default: return "";
    }
}

ExternalRouter::ModuleServer::ModuleServer()
    : Thread("/router/external/moduleserver"),
      Logger("ExternalRouter::ModuleServer", "/router/external/moduleserver"),
      ExternalRouterServerIF("ExtRtrModuleServer"),
      eventq_(logpath_)
{
    set_logpath("/router/external/moduleserver");

    // we always delete the thread object when we exit
    //Thread::set_flag(Thread::DELETE_ON_EXIT);

    last_recv_seq_ctr_ = 0;

    tcp_interface_ = QPtr_TcpInterface(new TcpInterface(this, ExternalRouter::network_interface_, ExternalRouter::server_port));

    //tcp_interface->logpathf("%s/iface/%s", logpath_, iface->name().c_str());

    int ret = tcp_interface_->bind(ExternalRouter::network_interface_, ExternalRouter::server_port);

    // be a little forgiving -- if the address is in use, wait for a
    // bit and try again
    int ctr = 0;
    while ((ret != 0) && (ctr++ < 100) && (errno == EADDRINUSE) && !should_stop()) {
        if (ctr == 1) {
            tcp_interface_->logf(oasys::LOG_ERR,
                           "WARNING: error binding ExternalRouter to requested socket: %s  -- retrying up to 10 seconds",
                           strerror(errno));
        }
        usleep(100000);
    
        ret = tcp_interface_->bind(ExternalRouter::network_interface_, ExternalRouter::server_port);
    }
   
    if (ret != 0) {
        tcp_interface_->logf(oasys::LOG_ERR,
                       "ERROR: error binding ExternalRouter to requested socket: %s  -- aborting",
                           strerror(errno));
        return; // error already logged
    }

    // start listening and then start the thread to loop calling accept()
    tcp_interface_->listen();
    tcp_interface_->start();

}

ExternalRouter::ModuleServer::~ModuleServer()
{
    do_shutdown();

    // free all pending events
    std::string *event;
    while (eventq_.try_pop(&event))
        delete event;
}

void
ExternalRouter::ModuleServer::do_shutdown()
{
    set_should_stop();

    if (tcp_interface_ != nullptr) {
        tcp_interface_->do_shutdown();
        int ctr = 0;
        while ((tcp_interface_ != nullptr) && (++ctr < 20)) {
            usleep(100000);
        }
    }

    tcp_interface_.reset();
}


/// Post a string to send 
void
ExternalRouter::ModuleServer::post(std::string* event)
{
    eventq_.push_back(event);
}

/// Post a string to send 
void
ExternalRouter::ModuleServer::post_to_send(std::string* event)
{
    if (tcp_interface_ != nullptr)
    {
        tcp_interface_->post_to_send(event);
    }
    else
    {
        delete event;
    }
}

// ModuleServer main loop
void
ExternalRouter::ModuleServer::run() 
{
    char threadname[16] = "ExtRtrModlSrvr";
    pthread_setname_np(pthread_self(), threadname);

    // block on input from event queue
    struct pollfd pollfds[1];

    struct pollfd* event_poll = &pollfds[0];
    event_poll->fd = eventq_.read_fd();
    event_poll->events = POLLIN;
    event_poll->revents = 0;

    while (1) {
        if (should_stop()) return;

        // block waiting...
        int ret = oasys::IO::poll_multiple(pollfds, 1, 10);

        if (ret == oasys::IOINTR) {
            //log_debug("module server interrupted");
            set_should_stop();
            continue;
        }

        // check for an event
        if (event_poll->revents & POLLIN) {
            std::string *event;
            if (eventq_.try_pop(&event)) {
                ASSERT(event != nullptr)

                process_action(event);

                delete event;
            }    
        }
    }
}

// Handle a message from an external router
int
ExternalRouter::ModuleServer::process_action(std::string* msg)
{
    //dzdebug
    //log_always("ModuleServer::process_action - entered with msg of length %zu", msg->length());

    bool msg_processed = false;

    CborParser parser;
    CborValue cvMessage;
    CborValue cvElement;
    CborError err;

    err = cbor_parser_init((const uint8_t*) msg->c_str(), msg->length(), 0, &parser, &cvMessage);
    CBORUTIL_CHECK_CBOR_DECODE_ERR_RETURN

    err = cbor_value_enter_container(&cvMessage, &cvElement);
    CBORUTIL_CHECK_CBOR_DECODE_ERR_RETURN

    uint64_t msg_type;
    uint64_t msg_version;
    int status = decode_client_msg_header(cvElement, msg_type, msg_version);
    CHECK_CBORUTIL_STATUS_RETURN


    switch (msg_type) {
        case EXTRTR_MSG_LINK_QUERY:
            // this message has no data for version 0
            if (msg_version == 0) {
                msg_processed = true;
                BundleDaemon::post(new LinkQueryRequest());
            }
            break;

        case EXTRTR_MSG_BUNDLE_QUERY:
            // this message has no data for version 0
            if (msg_version == 0) {
                msg_processed = true;
                BundleDaemon::post(new BundleQueryRequest());
            }
            break;

        case EXTRTR_MSG_TRANSMIT_BUNDLE_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_transmit_bundle_req_msg_v0(cvElement);
            }
            break;

        case EXTRTR_MSG_LINK_RECONFIGURE_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_link_reconfigure_req_msg_v0(cvElement);
            }
            break;

        case EXTRTR_MSG_LINK_CLOSE_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_link_close_req_msg_v0(cvElement);
            }
            break;

        case EXTRTR_MSG_TAKE_CUSTODY_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_take_custody_req_msg_v0(cvElement);
            }
            break;

        case EXTRTR_MSG_DELETE_BUNDLE_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_delete_bundle_req_msg_v0(cvElement);
            }
            break;

        case EXTRTR_MSG_SHUTDOWN_REQ:
            if (msg_version == 0) {
                msg_processed = true;
                process_shutdown_req_msg_v0(cvElement);
            }
            break;


        default: break;
    }


    if (msg_processed) {
        //log_always("ExternalRouterIF Msg Type; %" PRIu64 " (%s) - processed",
        //        msg_type, msg_type_to_str(msg_type));

        return CBORUTIL_SUCCESS;
    } else {
        log_err("ExternalRouterIF Msg Type; %" PRIu64 " (%s) - unsupported type or version: %" PRIu64,
                msg_type, msg_type_to_str(msg_type), msg_version);

        return CBORUTIL_FAIL;
    }


}


void
ExternalRouter::ModuleServer::process_transmit_bundle_req_msg_v0(CborValue& cvElement)
{
    bundleid_t bid;
    std::string link_id;

    if (CBORUTIL_SUCCESS != decode_transmit_bundle_req_msg_v0(cvElement, bid, link_id))
    {
        log_err("process_transmit_bundle_req_msg_v0: error parsing CBOR");
        return;
    }

    int action = ForwardingInfo::FORWARD_ACTION;

    BundleDaemon *bd = BundleDaemon::instance();
    BundleRef br = bd->all_bundles()->find_for_storage(bid);
    if (br.object() && br->expired()) {
        br.release();
    }

    if (br.object()) {
        //dzdebug
        //log_always("Issuing request to send *%p on link %s",
        //           br.object(), link_id.c_str());

        BundleSendRequest *request = new BundleSendRequest(br, link_id, action);
        BundleDaemon::post(request);
    } else {
        //TODO: send a message back indicating bunndle ID not found???
        //dzdebug
        //log_always("Issuing request to send unknown bundle id (%" PRIbid " on link %s",
        //           bid, link_id.c_str());

    }
}

void
ExternalRouter::ModuleServer::process_link_reconfigure_req_msg_v0(CborValue& cvElement)
{
    std::string link_id;
    extrtr_key_value_vector_t kv_vec;

    if (CBORUTIL_SUCCESS != decode_link_reconfigure_req_msg_v0(cvElement, link_id, kv_vec))
    {
        log_err("process_link_reconfigure_req_msg_v0: error parsing CBOR");
        return;
    }
        //dzdebug
        //log_always("process_link_reconfigure_req_msg_v0: num key-value pairs = %zu", kv_vec.size());


    BundleDaemon *bd = BundleDaemon::instance();
    LinkRef link = bd->contactmgr()->find_link(link_id.c_str());

    if (link.object() != 0) {
        AttributeVector params;

        extrtr_key_value_vector_iter_t iter = kv_vec.begin();
        while (iter != kv_vec.end()) {
            extrtr_key_value_ptr_t kvptr = *iter;

            if (kvptr->value_type_ == KEY_VAL_BOOL) {
                params.push_back(NamedAttribute(kvptr->key_, kvptr->value_bool_));
            } else if (kvptr->value_type_ == KEY_VAL_UINT) {
                params.push_back(NamedAttribute(kvptr->key_, kvptr->value_uint_));
            } else if (kvptr->value_type_ == KEY_VAL_INT) {
                params.push_back(NamedAttribute(kvptr->key_, kvptr->value_int_));
            } else if (kvptr->value_type_ == KEY_VAL_STRING) {
                params.push_back(NamedAttribute(kvptr->key_, kvptr->value_string_));
            }  else {
                log_warn("unknown value type in key-value pair");
            }

            ++iter;
        }

        // Post at head in case the event queue is backed up
        // XXX/dz configure start/stop transmitting should not wait
        BundleDaemon::post_at_head(new LinkReconfigureRequest(link, params));
    } else {
        log_err("attempt to reconfigure link %s that doesn't exist!",
                 link_id.c_str());
    }
}


void
ExternalRouter::ModuleServer::process_link_close_req_msg_v0(CborValue& cvElement)
{
    std::string link_id;

    if (CBORUTIL_SUCCESS != decode_link_close_req_msg_v0(cvElement, link_id))
    {
        log_err("process_link_close_req_msg_v0: error parsing CBOR");
        return;
    }


    BundleDaemon *bd = BundleDaemon::instance();
    LinkRef link = bd->contactmgr()->find_link(link_id.c_str());

    if (link.object() != 0) {
            BundleDaemon::post_at_head(
                new LinkStateChangeRequest(link, Link::CLOSED,
                                           ContactEvent::NO_INFO));
    } else {
        log_warn("attempt to close link %s that doesn't exist!",
                 link_id.c_str());
    }
}

void
ExternalRouter::ModuleServer::process_take_custody_req_msg_v0(CborValue& cvElement)
{
    uint64_t bundleid;

    if (CBORUTIL_SUCCESS != decode_take_custody_req_msg_v0(cvElement, bundleid))
    {
        log_err("process_take_custody_req_msg_v0: error parsing CBOR");
        return;
    }


    BundleDaemon *bd = BundleDaemon::instance();
    BundleRef br = bd->all_bundles()->find_for_storage(bundleid);
    if (br.object() && br->expired()) {
        br.release();
    }

    if (br.object()) {
        BundleTakeCustodyRequest *request =
            new BundleTakeCustodyRequest(br);
        BundleDaemon::post(request);
    }
    else {
        log_warn("attempt to take custody of nonexistent bundle: %" PRIbid, bundleid);
    }
}

void
ExternalRouter::ModuleServer::process_delete_bundle_req_msg_v0(CborValue& cvElement)
{
    extrtr_bundle_id_vector_t bid_vec;

    if (CBORUTIL_SUCCESS != decode_delete_bundle_req_msg_v0(cvElement, bid_vec))
    {
        log_err("process_delete_bundle_req_msg_v0: error parsing CBOR");
        return;
    }
        //dzdebug
        //log_always("process_delete_bundle_req_msg_v0: # of bundle_ids: %zu", bid_vec.size());


    BundleDaemon *bd = BundleDaemon::instance();

    extrtr_bundle_id_vector_iter_t iter = bid_vec.begin();
    while (iter != bid_vec.end()) {
        extrtr_bundle_id_ptr_t bidptr = *iter;

        bundleid_t bid = bidptr->bundleid_;

        BundleRef br = bd->all_bundles()->find_for_storage(bid);
        if (br.object())
        {
            br->set_manually_deleting(true);

            if (br.object() && br->expired()) {
                br.release();
            }

            if (br.object()) {
                BundleDeleteRequest *request =
                    new BundleDeleteRequest(br,
                                            BundleProtocol::REASON_NO_ADDTL_INFO);
                BundleDaemon::post(request);
            }
        }
        else {
            log_warn("attempt to delete nonexistent bundle: %" PRIbid, bid);
        }


        ++iter;
    }

}

void
ExternalRouter::ModuleServer::process_shutdown_req_msg_v0(CborValue& cvElement)
{
    // Version zero of this message does not have any additional data
    (void) cvElement;

    log_info("Initiating Shutdown procedure");
    // XXX/dz Note that if running with the console then
    // the program does not actually exit until the user hits
    // <Return> because it is waiting in a readline for input
    oasys::TclCommandInterp::instance()->exec_command("shutdown");
    oasys::TclCommandInterp::instance()->exit_event_loop();
    oasys::TclCommandInterp::instance()->exec_command("shutdown");

}





















//----------------------------------------------------------------------
ExternalRouter::ModuleServer::TcpInterface::TcpInterface(ModuleServer* parent, in_addr_t listen_addr, u_int16_t listen_port)
    : TCPServerThread("ExternalRouter::ModuleServer::TcpInterface",
            "/router/external/moduleserver/TcpInterface")
{
    logfd_       = false;

    // we always delete the thread object when we exit
    //Thread::set_flag(Thread::DELETE_ON_EXIT);

    parent_ = parent;
    listen_addr_ = listen_addr;
    listen_port_ = listen_port;

    router_client_ = nullptr;
}

//----------------------------------------------------------------------
ExternalRouter::ModuleServer::TcpInterface::~TcpInterface()
{
    do_shutdown();
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpInterface::do_shutdown()
{
    stop();

    oasys::ScopeLock l(&lock_, __func__);

    if (router_client_ != nullptr)
    {
        router_client_->set_should_stop();
    }
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpInterface::accepted(int fd, in_addr_t addr, u_int16_t port)
{
    oasys::ScopeLock l(&lock_, __func__);

    if (router_client_ == nullptr)
    {
      router_client_ = new TcpConnection(parent_, fd, addr, port);
      log_info("External Router accepted TCP connection from %s:%d", intoa(addr), port);
      router_client_->start();
    }
    else
    {
      log_warn("Rejected External Router TCP connection while already connectd");
      ::close(fd);
    }
}


//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::client_closed ( TcpConnection* cl )
{
    if (tcp_interface_ != nullptr)
    {
      tcp_interface_->client_closed(cl);
    }
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpInterface::client_closed ( TcpConnection* cl )
{
    oasys::ScopeLock l(&lock_, __func__);

    if (router_client_ == cl)
    {
      router_client_ = nullptr;
      log_info("External Router closed TCP connection");
      //log_info("External Router closed TCP connection from %s:%d", intoa(addr), port);
    }
    else
    {
      log_crit("External Router attempt to close unknown TCP connection??");
    }
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpInterface::post_to_send(std::string* event)
{
    oasys::ScopeLock l(&lock_, __func__);

    if (router_client_ != nullptr )
    {
        router_client_->post_to_send(event);
    }
    else
    {
        delete event;
        log_debug("Message dropped since external router is not connected");
    }
}

//----------------------------------------------------------------------
ExternalRouter::ModuleServer::TcpConnection::TcpConnection(ModuleServer* parent,
                                             int fd, in_addr_t client_add,
                                             u_int16_t client_port) 
            : TCPClient(fd, client_add, client_port),
              Thread("ExternalRouterTcpConnection", oasys::Thread::DELETE_ON_EXIT) 
{
    parent_ = parent;
    client_addr_ = client_add;
    client_port_ = client_port;
    tcp_sender_ = nullptr;
}

//----------------------------------------------------------------------
ExternalRouter::ModuleServer::TcpConnection::~TcpConnection()
{
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpConnection::do_shutdown()
{
    set_should_stop();

    oasys::ScopeLock l(&lock_, __func__);

    if (tcp_sender_ != nullptr)
    {
        tcp_sender_->set_should_stop();
    }

    l.unlock();

    int ctr = 0;
    while ((tcp_sender_ != nullptr) && (++ctr < 20)) {
        usleep(100000);
    }

    delete this;
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpConnection::process_data(u_char* bp, size_t len)
{
    (void) bp;
    (void) len;
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpConnection::post_to_send(std::string* event)
{
    if (!should_stop()) {
        oasys::ScopeLock l(&lock_, __func__);

        if (tcp_sender_ != nullptr)
        {
            tcp_sender_->post(event);
        }
        else
        {
            log_err("Deleting message posted while TCPSender thread not started");
            delete event;
        }
    }
    else {
        log_err("Deleting message posted while stopping");
        delete event;
    }
}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpConnection::run()
{
    char threadname[16] = "ExtRtrModSvrRcv";
    pthread_setname_np(pthread_self(), threadname);

    int ret;

    bool got_magic_number = false;

    int all_data     = 0;
    int data_ptr     = 0;
    int data_to_read = 0;

    union US {
       u_char char_size[4];
       int    int_size;
    };

    US union_size;

    oasys::ScratchBuffer<u_char*, 0> buffer;

    // spawn a sender thread
    lock_.lock(__func__);
    tcp_sender_ = new TcpSender(this);
    lock_.unlock();
    tcp_sender_->start();


    while (!should_stop()) {
        
        data_to_read = 4;       
        //
        // get the size off the wire and then read the data
        //
        data_ptr     = 0;
        while (!should_stop() && (data_to_read > 0))
        {
            ret = timeout_read((char *) &(union_size.char_size[data_ptr]), data_to_read, 100);
            if ((ret == oasys::IOEOF) || (ret == oasys::IOERROR)) {   
                close();
                set_should_stop();
                break;
            }
            if (ret > 0) {
                data_ptr     += ret;
                data_to_read -= ret;
            }
        }

        if (should_stop())
            break;

        if (!got_magic_number) {
            // first 4 bytes read must be the client side magic number or we abort
            union_size.int_size = ntohl(union_size.int_size);
            if (union_size.int_size != 0x58434C54) {   // XCLT
                log_err("Aborting - did not receive ExternalRouter client-side magic number");
                close();
                set_should_stop();
                break;
            }
            got_magic_number = true;

            continue; // now try to read a size value
        }



        data_ptr = 0;
        all_data = data_to_read = ntohl(union_size.int_size);
        if (all_data == 0) {
            continue;
        }
        buffer.clear();
        buffer.reserve(data_to_read+1); // reserve storage

        while (!should_stop() && (data_to_read > 0)) {
            ret = timeout_read((char*) buffer.buf()+data_ptr, data_to_read, 100);
            if ((ret == oasys::IOEOF) || (ret == oasys::IOERROR)) {   
                close();
                set_should_stop();
                break;
            }
            if (ret > 0) {
                buffer.incr_len(ret);
                data_to_read -= ret;
                data_ptr     += ret;
            }
        } 


        if (should_stop())
            break;


        //log_debug("ExternalRouter TCP Connection got %d byte packet", all_data);

        parent_->post(new std::string((const char*) buffer.buf(), all_data));
    }

    tcp_sender_->set_should_stop();

    close();

    // inform parent we are exiting
    parent_->client_closed ( this );

}

//----------------------------------------------------------------------
void
ExternalRouter::ModuleServer::TcpConnection::sender_closed ( TcpSender* sender )
{
    oasys::ScopeLock l(&lock_, __func__);

    if (tcp_sender_ == sender)
    {
      tcp_sender_ = nullptr;
    }
}

ExternalRouter::ModuleServer::TcpConnection::TcpSender::TcpSender(TcpConnection* parent)
    : Thread("/router/external/moduleserver/tcpsndr"),
      Logger("ExternalRouter::ModuleServer::TcpConnection::TcpSender", "/router/external/moduleserver/tcpsndr"),
      eventq_(logpath_)
{
    set_logpath("/router/external/moduleserver/tcpsndr");

    parent_ = parent;

    // we always delete the thread object when we exit
    Thread::set_flag(Thread::DELETE_ON_EXIT);
}

ExternalRouter::ModuleServer::TcpConnection::TcpSender::~TcpSender()
{
    // free all pending events
    std::string *event;
    while (eventq_.try_pop(&event))
        delete event;
}

void
ExternalRouter::ModuleServer::TcpConnection::TcpSender::post(std::string* event)
{
    oasys::ScopeLock l(&eventq_lock_, __func__);

    eventq_.push_back(event);

    eventq_bytes_ += event->length();
    if (eventq_bytes_ > eventq_bytes_max_)
    {
        eventq_bytes_max_ += eventq_bytes_;
//        log_always("ExtRtrTcpSndr - new max queued to be sent via TCP = %zu", eventq_bytes_max_);
    }
}

void
ExternalRouter::ModuleServer::TcpConnection::TcpSender::run() 
{
    char threadname[16] = "ExtRtrModSvrSnd";
    pthread_setname_np(pthread_self(), threadname);

    int cc;

    union US { 
       char       chars[4];
       int32_t    int_size;
    };   

    US union_size;

    // send server side magic number first thing
    union_size.int_size = 0x58525452; // XRTR
    union_size.int_size = htonl(union_size.int_size);

    cc = parent_->writeall(union_size.chars, 4);
    if (cc != 4) {
        log_err("error sending ExternalRouter server-side magic number: %s", strerror(errno));
    }


    // block on input from the socket and
    // on input from the bundle event list
    struct pollfd pollfds[1];

    struct pollfd* event_poll = &pollfds[0];
    event_poll->fd = eventq_.read_fd();
    event_poll->events = POLLIN;
    event_poll->revents = 0;

    while (1) {
        if (should_stop()) return;

        // block waiting...
        int ret = oasys::IO::poll_multiple(pollfds, 1, 10);

        if (ret == oasys::IOINTR) {
            //log_debug("module server TcpSender interrupted");
            set_should_stop();
            continue;
        }

        if (ret == oasys::IOERROR) {
            //log_debug("module server TcpSender error");
            set_should_stop();
            continue;
        }

        // check for an event
        if (event_poll->revents & POLLIN) {
            std::string *event;
            if (eventq_.try_pop(&event)) {
                ASSERT(event != nullptr)

                union_size.int_size = htonl(event->size());

                cc = parent_->writeall(union_size.chars, 4);
                if (cc != 4) {
                    log_err("error writing msg: %s", strerror(errno));
                } else {
                    //log_debug("Transmit message of length: %zu", event->size());


                    cc = parent_->writeall( const_cast< char * >(event->c_str()), event->size());
                    if (cc != (int) event->size())
                    {
                        log_err("error writing msg: %s", strerror(errno));
                    }
                }

                oasys::ScopeLock l(&eventq_lock_, __func__);
                eventq_bytes_ -= event->length();

                delete event;
            }
        }
    }

//    parent_->sender_closed ( this );
}


ExternalRouter::HelloTimer::HelloTimer(ExternalRouter *router)
    : router_(router)
{
}

ExternalRouter::HelloTimer::~HelloTimer()
{
    cancel();
}

void
ExternalRouter::HelloTimer::start(uint32_t seconds, SPtr_HelloTimer& sptr)
{
    seconds_ = seconds;
    sptr_ = sptr;

    if (seconds_ == 0) {
        seconds_ = 1;
    }
    oasys::SharedTimer::schedule_in(seconds_*1000, sptr_);
}

void
ExternalRouter::HelloTimer::cancel()
{
    if (sptr_ != nullptr) {
        oasys::SharedTimer::cancel(sptr_);
        sptr_ = nullptr;
    }
}

// Timeout callback for the hello timer
void
ExternalRouter::HelloTimer::timeout(const struct timeval &)
{
    uint64_t bundles_received = BundleDaemon::instance()->get_received_bundles();
    uint64_t bundles_pending = BundleDaemon::instance()->pending_bundles()->size();
    
    if (!cancelled()) {
        router_->server_send_hello_msg(bundles_received, bundles_pending);

        oasys::SharedTimer::schedule_in(seconds_ * 1000, sptr_);
    }
}

ExternalRouter::ERRegistration::ERRegistration(ExternalRouter *router)
    : Registration(Registration::EXTERNALROUTER_REGID,
                    EndpointID(BundleDaemon::instance()->local_eid().str() +
                        EXTERNAL_ROUTER_SERVICE_TAG),
                   Registration::DEFER, 0, 0),
      router_(router)
{
    logpathf("/reg/admin");
    
    BundleDaemon::post(new RegistrationAddedEvent(this, EVENTSRC_ADMIN));
}

// deliver a bundle to external routers
void
ExternalRouter::ERRegistration::deliver_bundle(Bundle *bundle)
{
    //dzdebug

    BundleDaemon::post(new BundleDeliveredEvent(bundle, this));
}

// Global shutdown callback function
void external_rtr_shutdown(void *)
{
    BundleDaemon::instance()->router()->shutdown();
}

// Initialize ExternalRouter parameters
u_int16_t ExternalRouter::server_port          = 8001;
u_int16_t ExternalRouter::hello_interval       = 30;
in_addr_t ExternalRouter::network_interface_   = htonl(INADDR_LOOPBACK);

} // namespace dtn
