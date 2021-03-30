/*
 *    Copyright 2015 United States Government as represented by NASA
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

#ifdef DTPC_ENABLED

#include "bundling/BundleEvent.h"

#include "DtpcDaemon.h"
#include "DtpcPayloadAggregationTimer.h"

namespace dtn {

DtpcPayloadAggregationTimer::DtpcPayloadAggregationTimer(std::string key, u_int64_t seq_ctr)
    : key_(key),
      seq_ctr_(seq_ctr)
{
}

void
DtpcPayloadAggregationTimer::set_sptr(SPtr_DtpcPayloadAggregationTimer& sptr)
{
    oasys::ScopeLock scoplok(&lock_, __func__);

    sptr_ = sptr;
}


void
DtpcPayloadAggregationTimer::start(int seconds)
{
    oasys::ScopeLock scoplok(&lock_, __func__);

    if (sptr_ != nullptr) {
        schedule_in(seconds, sptr_);
    } else {
        log_err_p("dtpc/topic/expiration", 
                  "Attempt to start DtpcPayloadAggregationTimer(%s - %" PRIu64 ") without a SPtr_Timer defined", 
                  key_.c_str(), seq_ctr_);
    }
}

bool
DtpcPayloadAggregationTimer::cancel()
{
    bool result = false;

    oasys::ScopeLock scoplok(&lock_, __func__);

    if (sptr_ != nullptr) {
        result = oasys::SharedTimer::cancel(sptr_);

        // clear the internal reference so this obejct will be deleted
        sptr_ = nullptr;
    }

    return result;
}

void
DtpcPayloadAggregationTimer::timeout(const struct timeval& now)
{
    (void)now;
    
    oasys::ScopeLock scoplok(&lock_, __func__);

    if (cancelled() || sptr_ == nullptr) {
        // clear the internal reference so this obejct will be deleted
        sptr_ = nullptr;
    } else {
        // post the expiration event
        log_debug_p("dtpc/timer/agg", "Payload aggregation timer expired: %s - seq: %" PRIu64, 
                    key_.c_str(), seq_ctr_);

        DtpcDaemon::post_at_head(new DtpcPayloadAggregationTimerExpiredEvent(key_, seq_ctr_));

        // clear the internal reference so this obejct will be deleted
        sptr_ = nullptr;
    }
}

} // namespace dtn

#endif // DTPC_ENABLED
