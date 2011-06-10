// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

// C
#include <stdint.h>

// STL
#include <tr1/functional>

// Google Log
#include <glog/logging.h>

// e
#include <e/guard.h>
#include <e/timer.h>

// HyperDex
#include <hyperdex/buffer.h>
#include <hyperdex/city.h>
#include <hyperdex/hyperspace.h>
#include <hyperdex/replication.h>
#include <hyperdex/stream_no.h>

hyperdex :: replication :: replication(datalayer* data, logical* comm)
    : m_data(data)
    , m_comm(comm)
    , m_config()
    , m_lock()
    , m_chainlink_calculators_lock()
    , m_chainlink_calculators()
    , m_keyholders_lock()
    , m_keyholders()
    , m_clientops()
    , m_shutdown(false)
    , m_retransmitter(std::tr1::bind(&replication::retransmit, this))
{
    m_retransmitter.start();
}

hyperdex :: replication :: ~replication() throw ()
{
    if (!m_shutdown)
    {
        shutdown();
    }

    m_retransmitter.join();
}

void
hyperdex :: replication :: prepare(const configuration&, const instance&)
{
    // Do nothing.
}

void
hyperdex :: replication :: reconfigure(const configuration& newconfig, const instance& us)
{
    // XXX need to convert "mayack" messages to disk for every region for which
    // we are the row leader.

    // When reconfiguring, we need to drop all messages which we deferred (e.g.,
    // for arriving out-of-order) because they may no longer be from a valid
    // sender.
    //
    // We also drop all resources associated with regions for which we no longer
    // hold responsibility.
    //
    // XXX If we track the old/new entity->instance mappings for deferred
    // messages, we can selectively drop them.
    m_config = newconfig;
    std::set<regionid> regions;

    for (std::map<entityid, instance>::const_iterator e = m_config.entity_mapping().begin();
            e != m_config.entity_mapping().end(); ++e)
    {
        if (e->second == us)
        {
            regions.insert(e->first.get_region());
        }
    }

    std::map<keypair, e::intrusive_ptr<keyholder> >::iterator khiter;
    khiter = m_keyholders.begin();

    while (khiter != m_keyholders.end())
    {
        khiter->second->deferred_updates.clear();

        if (regions.find(khiter->first.region) == regions.end())
        {
            std::map<keypair, e::intrusive_ptr<keyholder> >::iterator to_erase;
            to_erase = khiter;
            ++khiter;
            m_keyholders.erase(to_erase);
        }
        else
        {
            ++khiter;
        }
    }

    std::map<regionid, e::intrusive_ptr<chainlink_calculator> >::iterator clciter;
    clciter = m_chainlink_calculators.begin();

    while (clciter != m_chainlink_calculators.end())
    {
        if (regions.find(clciter->first) == regions.end())
        {
            std::map<regionid, e::intrusive_ptr<chainlink_calculator> >::iterator to_erase;
            to_erase = clciter;
            ++clciter;
            m_chainlink_calculators.erase(to_erase);
        }
        else
        {
            ++clciter;
        }
    }
}

void
hyperdex :: replication :: cleanup(const configuration&, const instance&)
{
    // Do nothing.
}

void
hyperdex :: replication :: shutdown()
{
    m_shutdown = true;
}

void
hyperdex :: replication :: client_put(const entityid& from,
                                      const entityid& to,
                                      uint32_t nonce,
                                      const e::buffer& key,
                                      const std::vector<e::buffer>& newvalue)
{
    client_common(PUT, from, to, nonce, key, newvalue);
}

void
hyperdex :: replication :: client_del(const entityid& from,
                                      const entityid& to,
                                      uint32_t nonce,
                                      const e::buffer& key)
{
    client_common(DEL, from, to, nonce, key, std::vector<e::buffer>());
}

void
hyperdex :: replication :: chain_put(const entityid& from,
                                     const entityid& to,
                                     uint64_t newversion,
                                     bool fresh,
                                     const e::buffer& key,
                                     const std::vector<e::buffer>& newvalue)
{
    chain_common(PUT, from, to, newversion, fresh, key, newvalue);
}

void
hyperdex :: replication :: chain_del(const entityid& from,
                                     const entityid& to,
                                     uint64_t newversion,
                                     const e::buffer& key)
{
    chain_common(DEL, from, to, newversion, false, key, std::vector<e::buffer>());
}

void
hyperdex :: replication :: chain_pending(const entityid& /*from*/,
                                         const entityid& to,
                                         uint64_t version,
                                         const e::buffer& key)
{
    // PENDING messages may only go to the first subspace.
    if (to.subspace != 0)
    {
        return;
    }

    // Grab the lock that protects this keypair.
    keypair kp(to.get_region(), key);
    po6::threads::mutex::hold hold(get_lock(kp));

    // Get a reference to the keyholder for the keypair.
    e::intrusive_ptr<keyholder> kh = get_keyholder(kp);

    // Check that the reference is valid.
    if (!kh)
    {
        return;
    }

    std::map<uint64_t, e::intrusive_ptr<pending> >::iterator to_allow_ack;
    to_allow_ack = kh->pending_updates.find(version);

    if (to_allow_ack == kh->pending_updates.end())
    {
        return;
    }

    // XXX We should possibly check that the PENDING comes from a valid party, but
    // it's not essential for now.
    e::intrusive_ptr<pending> pend = to_allow_ack->second;
    pend->mayack = true;

    // We are point-leader for this subspace.
    if (to.number == 0)
    {
        handle_point_leader_work(to.get_region(), version, key, kh, pend);
    }
    else
    {
        e::buffer info;
        info.pack() << version << key;
        m_comm->send(to, entityid(to.get_region(), to.number - 1), stream_no::PENDING, info);
    }
}

void
hyperdex :: replication :: chain_ack(const entityid& /*from*/,
                                     const entityid& to,
                                     uint64_t version,
                                     const e::buffer& key)
{
    // Grab the lock that protects this keypair.
    keypair kp(to.get_region(), key);
    po6::threads::mutex::hold hold(get_lock(kp));

    // Get a reference to the keyholder for the keypair.
    e::intrusive_ptr<keyholder> kh = get_keyholder(kp);

    // Check that the reference is valid.
    if (!kh)
    {
        return;
    }

    std::map<uint64_t, e::intrusive_ptr<pending> >::iterator to_ack;
    to_ack = kh->pending_updates.find(version);

    if (to_ack == kh->pending_updates.end())
    {
        return;
    }

    // XXX We should possibly check that the ACK comes from a valid party, but
    // it's not essential for now.

    e::intrusive_ptr<pending> pend = to_ack->second;

    // If we may not ack the message yet, just drop it.
    if (!pend->mayack)
    {
        return;
    }

    send_ack(to.get_region(), version, key, pend);
    pend->acked = true;

    while (!kh->pending_updates.empty() && kh->pending_updates.begin()->second->acked)
    {
        uint64_t ver = kh->pending_updates.begin()->first;
        e::intrusive_ptr<pending> commit = kh->pending_updates.begin()->second;

        kh->pending_updates.erase(kh->pending_updates.begin());

        if (!commit->ondisk && commit->op == PUT)
        {
            m_data->put(to.get_region(), key, commit->value, ver);
        }
        else if (!commit->ondisk && commit->op == DEL)
        {
            m_data->del(to.get_region(), key);
        }
    }

    unblock_messages(to.get_region(), key, kh);

    if (kh->pending_updates.empty())
    {
        erase_keyholder(kp);
    }
}

// XXX It'd be a good idea to have the same thread which does periodic
// retransmission also handle cleaning of deferred updates which are too old.
// At any time it's acceptable to drop a deferred update, so this won't hurt
// correctness, and does make it easy to reclaim keyholders (if
// pending/blocked/deferred are empty then drop it).

void
hyperdex :: replication :: client_common(op_t op,
                                         const entityid& from,
                                         const entityid& to,
                                         uint32_t nonce,
                                         const e::buffer& key,
                                         const std::vector<e::buffer>& value)
{
    // Make sure this message is from a client.
    if (from.space != UINT32_MAX)
    {
        return;
    }

    clientop co(to.get_region(), from, nonce);

    // Make sure this message is to the point-leader.
    if (to.subspace != 0 || to.number != 0)
    {
        respond_negatively_to_client(co, ERROR);
        return;
    }

    // If we have seen this (from, nonce) before and it is still active.
    if (m_clientops.contains(co))
    {
        return;
    }

    // Automatically respond with "ERROR" whenever we return without g.dismiss()
    e::guard g = e::makeobjguard(*this, &replication::respond_negatively_to_client, co, ERROR);
    m_clientops.insert(co);

    // Get a reference to the chainlink calculator.
    e::intrusive_ptr<chainlink_calculator> clc = get_chainlink_calculator(to.get_region());

    // Grab the lock that protects this keypair.
    keypair kp(to.get_region(), key);
    po6::threads::mutex::hold hold(get_lock(kp));

    // Get a reference to the keyholder for the keypair.
    e::intrusive_ptr<keyholder> kh = get_keyholder(kp);

    // Check that the two references are valid.
    if (!clc || !kh)
    {
        return;
    }

    // Check that a client's put matches the dimensions of the space.
    if (op == PUT && clc->expected_dimensions() != value.size() + 1)
    {
        // Tell the client they sent an invalid message.
        respond_negatively_to_client(co, INVALID);
        g.dismiss();
        return;
    }

    // Find the pending or committed version with the largest number.
    bool blocked = false;
    uint64_t oldversion = 0;
    bool have_oldvalue = false;
    std::vector<e::buffer> oldvalue;

    if (kh->blocked_updates.empty())
    {
        if (kh->pending_updates.empty())
        {
            // Get old version from disk.
            if (!from_disk(to.get_region(), key, &have_oldvalue, &oldvalue, &oldversion))
            {
                return;
            }
        }
        else
        {
            // Get old version from memory.
            std::map<uint64_t, e::intrusive_ptr<pending> >::reverse_iterator biggest;
            biggest = kh->pending_updates.rbegin();
            oldversion = biggest->first;
            have_oldvalue = biggest->second->op == PUT;
            oldvalue = biggest->second->value;
        }

        blocked = false;
    }
    else
    {
        // Get old version from memory.
        std::map<uint64_t, e::intrusive_ptr<pending> >::reverse_iterator biggest;
        biggest = kh->blocked_updates.rbegin();
        oldversion = biggest->first;
        have_oldvalue = biggest->second->op == PUT;
        oldvalue = biggest->second->value;
        blocked = true;
    }

    e::intrusive_ptr<pending> newpend;
    newpend = new pending(op, value, co);

    // Figure out the four regions we could send to.
    if (op == PUT && have_oldvalue)
    {
        clc->four_regions(key, oldvalue, value, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
    }
    else if (op == PUT && !have_oldvalue)
    {
        clc->four_regions(key, value, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
        // This is a put without a previous value.  If this is the result of it
        // being the first put for this key, then we need to tag it as fresh.
        // If it is the result of a PUT that is concurrent with a DEL (client's
        // perspective), then we need to block it.
        blocked = true;
        newpend->fresh = true;
    }
    else if (op == DEL && have_oldvalue)
    {
        clc->four_regions(key, oldvalue, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
    }
    else if (op == DEL && !have_oldvalue)
    {
        respond_positively_to_client(co, 0);
        return;
    }
    else
    {
        LOG(INFO) << "Unhandled case in client_common.";
        return;
    }

    // Check that the message was sent to the appropriate region.
    // Puts must not cause a change of region in the first subspace, therefore
    // we check that the put maps to both (which is a sanity check on our code
    // and not strictly necessary).
    if (!overlap(to, newpend->thisold) || !overlap(to, newpend->thisnew))
    {
        respond_negatively_to_client(co, INVALID);
        g.dismiss();
        return;
    }

    if (blocked)
    {
        kh->blocked_updates.insert(std::make_pair(oldversion + 1, newpend));
        unblock_messages(to.get_region(), key, kh);
    }
    else
    {
        kh->pending_updates.insert(std::make_pair(oldversion + 1, newpend));
        send_update(to.get_region(), oldversion + 1, key, newpend);
    }

    g.dismiss();
}

void
hyperdex :: replication :: chain_common(op_t op,
                                        const entityid& from,
                                        const entityid& to,
                                        uint64_t version,
                                        bool fresh,
                                        const e::buffer& key,
                                        const std::vector<e::buffer>& value)
{
    if (from.get_space() != to.get_space())
    {
        return;
    }

    if (to.number != 0 && from.get_region() != to.get_region())
    {
        LOG(INFO) << "Dropping message from another region which doesn't go to region-leader.";
        return;
    }

    // We need to get it from our predecessor if in a chain.
    if (from.get_region() == to.get_region() && from.number + 1 != to.number)
    {
        LOG(INFO) << "Someone is skipping ahead in the chain " << to.get_region();
        return;
    }

    // We cannot receive fresh messages from others within our subspace.
    if (fresh && from.get_subspace() == to.get_subspace() && from.get_region() != to.get_region())
    {
        LOG(INFO) << "Fresh message from someone in the same subspace as us " << to.get_subspace();
        return;
    }

    // Get a reference to the chainlink calculator.
    e::intrusive_ptr<chainlink_calculator> clc = get_chainlink_calculator(to.get_region());

    // Grab the lock that protects this keypair.
    keypair kp(to.get_region(), key);
    po6::threads::mutex::hold hold(get_lock(kp));

    // Get a reference to the keyholder for the keypair.
    e::intrusive_ptr<keyholder> kh = get_keyholder(kp);

    // Check that the two references are valid.
    if (!clc || !kh)
    {
        return;
    }

    // Check that a chain's put matches the dimensions of the space.
    if (op == PUT && clc->expected_dimensions() != value.size() + 1)
    {
        return;
    }

    // Figure out what to do with the update
    const uint64_t oldversion = version - 1;
    bool have_oldvalue = false;
    std::vector<e::buffer> oldvalue;
    std::map<uint64_t, e::intrusive_ptr<pending> >::iterator smallest_pending;
    std::map<uint64_t, e::intrusive_ptr<pending> >::iterator olditer;
    smallest_pending = kh->pending_updates.begin();

    // If nothing is pending.
    if (smallest_pending == kh->pending_updates.end())
    {
        uint64_t diskversion = 0;
        bool have_diskvalue = false;
        std::vector<e::buffer> diskvalue;

        // Get old version from disk.
        if (!from_disk(to.get_region(), key, &have_diskvalue, &diskvalue, &diskversion))
        {
            return;
        }

        if (diskversion >= version)
        {
            send_ack(to.get_region(), from, key, version);
            return;
        }
        else if (diskversion == oldversion)
        {
            have_oldvalue = have_diskvalue;
            oldvalue = diskvalue;
            // Fallthrough to set pending.
        }
        else
        {
            // XXX DEFER THE UPDATE.
            return;
        }
    }
    // If the version is committed.
    else if (smallest_pending->first > version)
    {
        send_ack(to.get_region(), from, key, version);
        return;
    }
    // If the update is pending already.
    else if (kh->pending_updates.find(version) != kh->pending_updates.end())
    {
        return;
    }
    // Fresh updates or oldversion is pending
    else if (fresh || (olditer = kh->pending_updates.find(oldversion)) != kh->pending_updates.end())
    {
        have_oldvalue = olditer->second->op == PUT;
        oldvalue = olditer->second->value;
        // Fallthrough to set pending.
    }
    // Oldversion is committed
    else if (smallest_pending->first > oldversion)
    {
        // This is an interesting case because oldversion is considered
        // committed (all pending updates are greater), but there is not a
        // pending update for version.  This means that an update >= to this one
        // was tagged fresh.  That should only happen when the point-leader has
        // seen the update immediately prior to the fresh one.  This means that
        // the point-leader is misbehaving.
        LOG(INFO) << "Dropping update which should have come before currently pending updates.";
        return;
    }
    else
    {
        // XXX DEFER THE UPDATE.
        return;
    }

    // Create a new pending object to set as pending.
    e::intrusive_ptr<pending> newpend;
    newpend = new pending(op, value);

    // Figure out the four regions we could send to.
    if (op == PUT && have_oldvalue)
    {
        clc->four_regions(key, oldvalue, value, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
    }
    else if (op == PUT && !have_oldvalue)
    {
        clc->four_regions(key, value, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
    }
    else if (op == DEL && have_oldvalue)
    {
        clc->four_regions(key, oldvalue, &newpend->prev,
                          &newpend->thisold, &newpend->thisnew, &newpend->next);
    }
    else if (op == DEL && !have_oldvalue)
    {
        LOG(INFO) << "Chain region sees double-delete.";
        return;
    }
    else
    {
        LOG(INFO) << "Unhandled case in chain_common.";
        return;
    }

    if (overlap(to, newpend->thisold) && overlap(to, newpend->thisnew))
    {
        if (to.number == 0 && !overlap(from, newpend->prev))
        {
            LOG(INFO) << "Dropping message from region which doesn't contain point.";
            return;
        }
    }
    else if (overlap(to, newpend->thisold))
    {
        if (to.number == 0 && !overlap(from, newpend->prev))
        {
            LOG(INFO) << "Dropping message from region which doesn't contain point.";
            return;
        }
    }
    else if (overlap(to, newpend->thisnew))
    {
        if (to.number == 0 && !overlap(from, newpend->thisold))
        {
            LOG(INFO) << "Dropping message from region which doesn't contain point.";
            return;
        }
    }
    else
    {
        LOG(INFO) << "Dropping message which doesn't fall within our responsibility.";
        return;
    }

    // We may ack this if we are not currently in subspace 0.
    newpend->mayack = newpend->thisold.subspace != 0;

    // Clear out dead deferred updates
    while (kh->deferred_updates.begin() != kh->deferred_updates.end()
           && kh->deferred_updates.begin()->first <= version)
    {
        kh->deferred_updates.erase(kh->deferred_updates.begin());
    }

    kh->pending_updates.insert(std::make_pair(version, newpend));
    send_update(to.get_region(), version, key, newpend);
    move_deferred_to_pending(kh);
}

po6::threads::mutex*
hyperdex :: replication :: get_lock(const keypair& /*kp*/)
{
    return &m_lock;
}

e::intrusive_ptr<hyperdex::replication::chainlink_calculator>
hyperdex :: replication :: get_chainlink_calculator(const regionid& r)
{
    typedef std::map<regionid, e::intrusive_ptr<chainlink_calculator> >::iterator clc_iter_t;
    po6::threads::mutex::hold hold(&m_chainlink_calculators_lock);
    clc_iter_t i;

    if ((i = m_chainlink_calculators.find(r)) != m_chainlink_calculators.end())
    {
        return i->second;
    }
    else
    {
        e::intrusive_ptr<chainlink_calculator> clc;
        clc = new chainlink_calculator(m_config, r);
        m_chainlink_calculators.insert(std::make_pair(r, clc));
        return clc;
    }
}

e::intrusive_ptr<hyperdex::replication::keyholder>
hyperdex :: replication :: get_keyholder(const keypair& kp)
{
    typedef std::map<keypair, e::intrusive_ptr<keyholder> >::iterator kh_iter_t;
    po6::threads::mutex::hold hold(&m_keyholders_lock);
    kh_iter_t i;

    if ((i = m_keyholders.find(kp)) != m_keyholders.end())
    {
        return i->second;
    }
    else
    {
        e::intrusive_ptr<keyholder> kh;
        kh = new keyholder();
        m_keyholders.insert(std::make_pair(kp, kh));
        return kh;
    }
}

void
hyperdex :: replication :: erase_keyholder(const keypair& kp)
{
    po6::threads::mutex::hold hold(&m_keyholders_lock);
    m_keyholders.erase(kp);
}

bool
hyperdex :: replication :: from_disk(const regionid& r,
                                     const e::buffer& key,
                                     bool* have_value,
                                     std::vector<e::buffer>* value,
                                     uint64_t* version)
{
    switch (m_data->get(r, key, value, version))
    {
        case SUCCESS:
            *have_value = true;
            return true;
        case NOTFOUND:
            *version = 0;
            *have_value = false;
            return true;
        case INVALID:
            LOG(INFO) << "Data layer returned INVALID when queried for old value.";
            return false;
        case ERROR:
            LOG(INFO) << "Data layer returned ERROR when queried for old value.";
            return false;
        default:
            LOG(WARNING) << "Data layer returned unknown result when queried for old value.";
            return false;
    }
}

void
hyperdex :: replication :: unblock_messages(const regionid& r,
                                            const e::buffer& key,
                                            e::intrusive_ptr<keyholder> kh)
{
    // We cannot unblock so long as there are messages pending.
    if (!kh->pending_updates.empty())
    {
        return;
    }

    if (kh->blocked_updates.empty())
    {
        return;
    }

    assert(kh->blocked_updates.begin()->second->fresh);

    do
    {
        std::map<uint64_t, e::intrusive_ptr<pending> >::iterator pend;
        pend = kh->blocked_updates.begin();
        kh->pending_updates.insert(std::make_pair(pend->first, pend->second));
        send_update(r, pend->first, key, pend->second);
        kh->blocked_updates.erase(pend);
    }
    while (!kh->blocked_updates.empty() && !kh->blocked_updates.begin()->second->fresh);
}

void
hyperdex :: replication :: move_deferred_to_pending(e::intrusive_ptr<keyholder> kh)
{
    // XXX We just drop deferred messages as it doesn't hurt correctness
    // (however, a bad move from deferred to pending will).
    kh->deferred_updates.clear();
}

void
hyperdex :: replication :: handle_point_leader_work(const regionid& pending_in,
                                                    uint64_t version,
                                                    const e::buffer& key,
                                                    e::intrusive_ptr<keyholder> kh,
                                                    e::intrusive_ptr<pending> update)
{
    send_ack(pending_in, version, key, update);

    if (!update->ondisk && update->op == PUT)
    {
        m_data->put(pending_in, key, update->value, version);
    }
    else if (!update->ondisk && update->op == DEL)
    {
        m_data->del(pending_in, key);
    }

    std::map<uint64_t, e::intrusive_ptr<pending> >::iterator penditer;

    for (penditer = kh->pending_updates.begin();
         penditer != kh->pending_updates.end() && penditer->first <= version;
         ++penditer)
    {
        penditer->second->ondisk = true;
    }

    if (update->co.from.space == UINT32_MAX)
    {
        clientop co = update->co;
        update->co = clientop();
        respond_positively_to_client(co, version);
    }
}

void
hyperdex :: replication :: send_update(const hyperdex::regionid& pending_in,
                                       uint64_t version, const e::buffer& key,
                                       e::intrusive_ptr<pending> update)
{
    uint8_t fresh = update->fresh ? 1 : 0;

    e::buffer info;
    info.pack() << version << key;

    e::buffer msg;
    e::packer pack(&msg);
    pack << version << fresh << key;

    hyperdex::regionid next;

    if (overlap(pending_in, update->thisold) && overlap(pending_in, update->thisnew))
    {
        next = update->next;
    }
    else if (overlap(pending_in, update->thisold))
    {
        next = update->thisnew;
    }
    else if (overlap(pending_in, update->thisnew))
    {
        next = update->next;
    }
    else
    {
        return;
    }

    stream_no::stream_no_t act;

    if (update->op == PUT)
    {
        pack << update->value;
        act = stream_no::PUT_PENDING;
    }
    else
    {
        act = stream_no::DEL_PENDING;
    }

    // If we are at the last subspace in the chain.
    if (next.subspace == 0)
    {
        // If we there is someone else in the chain, send the PUT or DEL pending
        // message to them.  If not, then send a PENDING message to the tail of
        // the first subspace.
        m_comm->send_forward_else_tail(pending_in, act, msg, next, stream_no::PENDING, info);
    }
    // Else we're in the middle of the chain.
    else
    {
        m_comm->send_forward_else_head(pending_in, act, msg, next, act, msg);
    }
}

void
hyperdex :: replication :: send_ack(const regionid& pending_in,
                                    uint64_t version,
                                    const e::buffer& key,
                                    e::intrusive_ptr<pending> update)
{
    e::buffer msg;
    msg.pack() << version << key;
    hyperdex::regionid prev;

    if (overlap(pending_in, update->thisold) && overlap(pending_in, update->thisnew))
    {
        prev = update->prev;
    }
    else if (overlap(pending_in, update->thisnew))
    {
        prev = update->thisold;
    }
    else if (overlap(pending_in, update->thisold))
    {
        prev = update->prev;
    }
    else
    {
        return;
    }

    m_comm->send_backward_else_tail(pending_in, stream_no::ACK, msg,
                                    prev, stream_no::ACK, msg);
}

void
hyperdex :: replication :: send_ack(const regionid& from,
                                    const entityid& to,
                                    const e::buffer& key,
                                    uint64_t version)
{
    e::buffer msg;
    msg.pack() << version << key;
    m_comm->send(from, to, stream_no::ACK, msg);
}

void
hyperdex :: replication :: respond_positively_to_client(clientop co,
                                                        uint64_t /*version*/)
{
    e::buffer msg;
    uint8_t result = static_cast<uint8_t>(SUCCESS);
    msg.pack() << co.nonce << result;
    m_clientops.remove(co);
    m_comm->send(co.region, co.from, stream_no::RESULT, msg);
}

void
hyperdex :: replication :: respond_negatively_to_client(clientop co,
                                                        result_t r)
{
    e::buffer msg;
    uint8_t result = static_cast<uint8_t>(r);
    msg.pack() << co.nonce << result;
    m_clientops.remove(co);
    m_comm->send(co.region, co.from, stream_no::RESULT, msg);
}

void
hyperdex :: replication :: retransmit()
{
    while (!m_shutdown)
    {
        try
        {
            std::set<keypair> kps;

            // Hold the lock just long enough to copy of all current key pairs.
            {
                po6::threads::mutex::hold hold(&m_keyholders_lock);
                std::map<keypair, e::intrusive_ptr<keyholder> >::iterator khiter;

                for (khiter = m_keyholders.begin(); khiter != m_keyholders.end(); ++khiter)
                {
                    kps.insert(khiter->first);
                }
            }

            for (std::set<keypair>::iterator kp = kps.begin(); kp != kps.end(); ++kp)
            {
                // Grab the lock that protects this keypair.
                po6::threads::mutex::hold hold(get_lock(*kp));

                // Get a reference to the keyholder for the keypair.
                e::intrusive_ptr<keyholder> kh = get_keyholder(*kp);

                if (!kh)
                {
                    continue;
                }

                if (kh->pending_updates.empty())
                {
                    unblock_messages(kp->region, kp->key, kh);
                }

                if (kh->pending_updates.empty())
                {
                    erase_keyholder(*kp);
                    continue;
                }

                uint64_t version = kh->pending_updates.begin()->first;
                e::intrusive_ptr<pending> pend = kh->pending_updates.begin()->second;

                send_update(kp->region, version, kp->key, pend);

                if (kp->region.subspace == 0)
                {
                    e::buffer info;
                    info.pack() << version << kp->key;
                    m_comm->send_backward(kp->region, stream_no::PENDING, info);
                }
            }
        }
        catch (std::exception& e)
        {
            LOG(INFO) << "Uncaught exception in retransmit thread: " << e.what();
        }

        e::sleep_ms(250);
    }
}

bool
hyperdex :: replication :: clientop :: operator < (const clientop& rhs) const
{
    const clientop& lhs(*this);

    if (lhs.region < rhs.region)
    {
        return true;
    }
    else if (lhs.region == rhs.region)
    {
        if (lhs.from < rhs.from)
        {
            return true;
        }
        else if (lhs.from == rhs.from)
        {
            return lhs.nonce < rhs.nonce;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

bool
hyperdex :: replication :: keypair :: operator < (const keypair& rhs) const
{
    const keypair& lhs(*this);

    if (lhs.region < rhs.region)
    {
        return true;
    }
    else if (lhs.region > rhs.region)
    {
        return false;
    }

    return lhs.key < rhs.key;
}

hyperdex :: replication :: pending :: pending(op_t o,
                                              const std::vector<e::buffer>& val,
                                              const clientop& c)
    : op(o)
    , value(val)
    , co(c)
    , fresh(false)
    , acked(false)
    , ondisk(false)
    , mayack(false)
    , prev()
    , thisold()
    , thisnew()
    , next()
    , m_ref(0)
{
}

hyperdex :: replication ::
chainlink_calculator :: chainlink_calculator(const configuration& config,
                                             const regionid& r)
    : m_ref(0)
    , m_region(r)
    , m_prev_subspace(0)
    , m_next_subspace(0)
    , m_prev_dims()
    , m_this_dims()
    , m_next_dims()
{
    // Count the number of subspaces for this space.
    size_t subspaces;

    if (!config.subspaces(r.get_space(), &subspaces))
    {
        throw std::runtime_error("Creating keyholder for non-existant subspace");
    }

    // Discover the dimensions of the space which correspond to the
    // subspace in which "to" is located, and the subspaces before and
    // after it.
    const_cast<uint16_t&>(m_prev_subspace) = r.subspace > 0 ? r.subspace - 1 : subspaces - 1;
    const_cast<uint16_t&>(m_next_subspace) = r.subspace < subspaces - 1 ? r.subspace + 1 : 0;

    if (!config.dimensions(r.get_subspace(), const_cast<std::vector<bool>*>(&m_this_dims))
        || !config.dimensions(subspaceid(r.space, m_prev_subspace), const_cast<std::vector<bool>*>(&m_prev_dims))
        || !config.dimensions(subspaceid(r.space, m_next_subspace), const_cast<std::vector<bool>*>(&m_next_dims)))
    {
        throw std::runtime_error("Could not determine dimensions of neighboring subspaces.");
    }
}

void
hyperdex :: replication ::
chainlink_calculator :: four_regions(const e::buffer& key,
                                     const std::vector<e::buffer>& oldvalue,
                                     const std::vector<e::buffer>& value,
                                     regionid* prev,
                                     regionid* thisold,
                                     regionid* thisnew,
                                     regionid* next)
{
    assert(oldvalue.size() == value.size());
    std::vector<uint64_t> oldhashes;
    std::vector<uint64_t> newhashes;
    uint64_t keyhash = CityHash64(key);
    oldhashes.push_back(keyhash);
    newhashes.push_back(keyhash);

    for (size_t i = 0; i < value.size(); ++i)
    {
        oldhashes.push_back(CityHash64(oldvalue[i]));
        newhashes.push_back(CityHash64(value[i]));
    }

    *prev = regionid(m_region.space, m_prev_subspace, 64, makepoint(newhashes, m_prev_dims));
    *thisold = regionid(m_region.get_subspace(), 64, makepoint(oldhashes, m_this_dims));
    *thisnew = regionid(m_region.get_subspace(), 64, makepoint(newhashes, m_this_dims));
    *next = regionid(m_region.space, m_next_subspace, 64, makepoint(oldhashes, m_next_dims));
}


void
hyperdex :: replication ::
chainlink_calculator :: four_regions(const e::buffer& key,
                                     const std::vector<e::buffer>& value,
                                     regionid* prev,
                                     regionid* thisold,
                                     regionid* thisnew,
                                     regionid* next)
{
    std::vector<uint64_t> hashes;
    uint64_t keyhash = CityHash64(key);
    hashes.push_back(keyhash);

    for (size_t i = 0; i < value.size(); ++i)
    {
        hashes.push_back(CityHash64(value[i]));
    }

    *prev = regionid(m_region.space, m_prev_subspace, 64, makepoint(hashes, m_prev_dims));
    *thisold = regionid(m_region.get_subspace(), 64, makepoint(hashes, m_this_dims));
    *thisnew = regionid(m_region.get_subspace(), 64, makepoint(hashes, m_this_dims));
    *next = regionid(m_region.space, m_next_subspace, 64, makepoint(hashes, m_next_dims));
}