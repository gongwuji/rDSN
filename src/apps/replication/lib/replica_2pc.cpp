/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Microsoft Corporation
 * 
 * -=- Robust Distributed System Nucleus (rDSN) -=- 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "replica.h"
#include "mutation.h"
#include "mutation_log.h"
#include "replica_stub.h"

#define __TITLE__ "TwoPhaseCommit"

namespace dsn { namespace replication {


void replica::on_client_write(int code, message_ptr& request)
{
    check_hashed_access();

    if (PS_PRIMARY != status())
    {
        response_client_message(request, ERR_INVALID_STATE);
        return;
    }

    mutation_ptr mu = new_mutation(_prepare_list->max_decree() + 1);
    mu->set_client_request(code, request);
    init_prepare(mu);
}

void replica::init_prepare(mutation_ptr& mu)
{
    dassert (PS_PRIMARY == status(), "");

    error_code err = ERR_SUCCESS;
    uint8_t count = 0;

    if (static_cast<int>(_primary_states.membership.secondaries.size()) + 1 < _options.mutation_2pc_min_replica_count)
    {
        err = ERR_NOT_ENOUGH_MEMBER;
        goto ErrOut;
    }
            
    mu->data.header.last_committed_decree = last_committed_decree();
    if (mu->data.header.decree == invalid_decree)
    {
        mu->set_id(get_ballot(), _prepare_list->max_decree() + 1);
    }
    else
    {
        mu->set_id(get_ballot(), mu->data.header.decree);
    }

    if (mu->data.header.decree > _prepare_list->max_decree() && _prepare_list->count() >= _options.staleness_for_commit)
    {
        err = ERR_CAPACITY_EXCEEDED;
        goto ErrOut;
    }
 
    dassert (mu->data.header.decree > last_committed_decree(), "");

    // local prepare without log
    err = _prepare_list->prepare(mu, PS_PRIMARY);
    if (err != ERR_SUCCESS)
    {
        goto ErrOut;
    }
        
    ddebug("%s: mutation %s init_prepare", name(), mu->name());

    //
    // TODO: bounded staleness on secondaries
    //
    dassert (mu->data.header.decree <= last_committed_decree() + _options.staleness_for_commit, "");
    
    // remote prepare
    dassert (mu->remote_tasks().size() == 0, "");
    mu->set_left_secondary_ack_count((unsigned int)_primary_states.membership.secondaries.size());
    for (auto it = _primary_states.membership.secondaries.begin(); it != _primary_states.membership.secondaries.end(); it++)
    {
        send_prepare_message(*it, PS_SECONDARY, mu, _options.prepare_timeout_ms_for_secondaries);
    }

    count = 0;
    for (auto it = _primary_states.learners.begin(); it != _primary_states.learners.end(); it++)
    {
        if (it->second.prepare_start_decree != invalid_decree && mu->data.header.decree >= it->second.prepare_start_decree)
        {
            send_prepare_message(it->first, PS_POTENTIAL_SECONDARY, mu, _options.prepare_timeout_ms_for_potential_secondaries);
            count++;
        }
    }    
    mu->set_left_potential_secondary_ack_count(count);

    // local log
    dassert (mu->data.header.log_offset == invalid_offset, "");
    dassert (mu->log_task() == nullptr, "");
    mu->log_task() = _stub->_log->append(mu,
        LPC_WRITE_REPLICATION_LOG,
        this,
        std::bind(&replica::on_append_log_completed, this, mu, 
            std::placeholders::_1, 
            std::placeholders::_2),
        gpid_to_hash(get_gpid())
        );

    if (nullptr == mu->log_task())
    {
        err = ERR_FILE_OPERATION_FAILED;
        handle_local_failure(err);
        goto ErrOut;
    }

    return;

ErrOut:
    response_client_message(mu->client_request, err);
    return;
}

void replica::send_prepare_message(const end_point& addr, partition_status status, mutation_ptr& mu, int timeout_milliseconds)
{
    message_ptr msg = message::create_request(RPC_PREPARE, timeout_milliseconds, gpid_to_hash(get_gpid()));
    marshall(msg, get_gpid());
    
    replica_configuration rconfig;
    _primary_states.get_replica_config(status, rconfig);

    marshall(msg, rconfig);
    mu->write_to(msg);

    dbg_dassert (mu->remote_tasks().find(addr) == mu->remote_tasks().end());

    mu->remote_tasks()[addr] = rpc::call(addr, msg, 
        this,
        std::bind(&replica::on_prepare_reply, 
            this,
            std::make_pair(mu, rconfig.status),
            std::placeholders::_1, 
            std::placeholders::_2, 
            std::placeholders::_3),
        gpid_to_hash(get_gpid())
        );

    ddebug( 
        "%s: mutation %s send_prepare_message to %s:%d as %s", 
        name(), mu->name(),
        addr.name.c_str(), static_cast<int>(addr.port),
        enum_to_string(rconfig.status)
        );
}

void replica::do_possible_commit_on_primary(mutation_ptr& mu)
{
    dassert (_config.ballot == mu->data.header.ballot, "");
    dassert (PS_PRIMARY == status(), "");

    if (mu->is_ready_for_commit())
    {   
        _prepare_list->commit(mu->data.header.decree, false);

        //PerformanceCounters::Decrement(PerfCounters_TwoPhaseCommitOngoing, nullptr);
        //PerformanceCounters::Increment(PerfCounters_TwoPhaseCommitQps, nullptr);

        //uint64_t duration =now_ms() - mu->start_time_milliseconds();
        //PerformanceCounters::Set(PerfCounters_TwoPhaseCommitDurationMs, duration, nullptr);
    }
}

void replica::on_prepare(message_ptr& request)
{
    check_hashed_access();

    replica_configuration rconfig;
    unmarshall(request, rconfig);    

    mutation_ptr mu = mutation::read_from(request);
    decree decree = mu->data.header.decree;

    ddebug( "%s: mutation %s on_prepare", name(), mu->name());

    dassert (mu->data.header.ballot == rconfig.ballot, "");

    if (mu->data.header.ballot < get_ballot())
    {
        ddebug( "%s: mutation %s on_prepare skipped due to old view", name(), mu->name());
        return;
    }

    // update configuration when necessary
    else if (rconfig.ballot > get_ballot())
    {
        update_local_configuration(rconfig);
    }

    if (PS_INACTIVE == status() || PS_ERROR == status())
    {
        ddebug( 
            "%s: mutation %s on_prepare  to %s skipped",
            name(), mu->name(),
            enum_to_string(status())
            );
        ack_prepare_message(ERR_INVALID_STATE, mu);
        return;
    }

    else if (PS_POTENTIAL_SECONDARY == status())
    {
        if (_potential_secondary_states.LearningState != LearningWithPrepare && _potential_secondary_states.LearningState != LearningSucceeded)
        {
            ddebug( 
                "%s: mutation %s on_prepare to %s skipped, learnings state = %s",
                name(), mu->name(),
                enum_to_string(status()),
                enum_to_string(_potential_secondary_states.LearningState)
                );

            // do not retry as there may retries later
            return;
        }
    }

    dassert (rconfig.status == status(), "");    
    if (decree <= last_committed_decree())
    {
        ack_prepare_message(ERR_SUCCESS, mu);
        return;
    }
    
    // real prepare start
    auto mu2 = _prepare_list->get_mutation_by_decree(decree);
    if (mu2 != nullptr && mu2->data.header.ballot == mu->data.header.ballot)
    {
        ddebug( "%s: mutation %s redundant prepare skipped", name(), mu->name());

        if (mu2->is_prepared())
        {
            ack_prepare_message(ERR_SUCCESS, mu);
        }
        return;
    }

    int err = _prepare_list->prepare(mu, status());
    dassert (err == ERR_SUCCESS, "");

    if (PS_POTENTIAL_SECONDARY == status())
    {
        dassert (mu->data.header.decree <= last_committed_decree() + _options.staleness_for_start_prepare_for_potential_secondary, "");
    }
    else
    {
        dassert (PS_SECONDARY == status(), "");
        dassert (mu->data.header.decree <= last_committed_decree() + _options.staleness_for_commit, "");
    }
    
    // write log
    dassert (mu->log_task() == nullptr, "");
    mu->log_task() = _stub->_log->append(mu,
        LPC_WRITE_REPLICATION_LOG,
        this,
        std::bind(&replica::on_append_log_completed, this, mu, std::placeholders::_1, std::placeholders::_2),
        gpid_to_hash(get_gpid())
        );

    if (nullptr == mu->log_task())
    {
        err = ERR_FILE_OPERATION_FAILED;
        ack_prepare_message(err, mu);
        handle_local_failure(err);
    }
}

void replica::on_append_log_completed(mutation_ptr& mu, uint32_t err, uint32_t size)
{
    check_hashed_access();

    ddebug( "%s: mutation %s on_append_log_completed, err = %u", name(), mu->name(), err);

    if (err == ERR_SUCCESS)
    {
        mu->set_logged();
    }

    // skip old mutations
    if (mu->data.header.ballot < get_ballot() || status() == PS_INACTIVE)
    {
        return;
    }

    switch (status())
    {
    case PS_PRIMARY:
        if (err == ERR_SUCCESS)
        {
            do_possible_commit_on_primary(mu);
        }
        else
        {
            handle_local_failure(err);
        }
        break;
    case PS_SECONDARY:
    case PS_POTENTIAL_SECONDARY:
        if (err != ERR_SUCCESS)
        {
            handle_local_failure(err);
        }
        ack_prepare_message(err, mu);
        break;
    case PS_ERROR:
        break;
    default:
        dassert (false, "");
        break;
    }
}

void replica::on_prepare_reply(std::pair<mutation_ptr, partition_status> pr, int err, message_ptr& request, message_ptr& reply)
{
    check_hashed_access();

    mutation_ptr& mu = pr.first;
    partition_status targetStatus = pr.second;

    // skip callback for old mutations
    if (mu->data.header.ballot < get_ballot() || PS_PRIMARY != status())
        return;
    
    dassert (mu->data.header.ballot == get_ballot(), "");

    end_point node = request->header().to_address;
    partition_status st = _primary_states.GetNodeStatus(node);

    // handle reply
    prepare_ack resp;

    // handle error
    if (err)
    {
        resp.err = err;
    }
    else
    {
        unmarshall(reply, resp);        

        ddebug( 
            "%s: mutation %s on_prepare_reply from %s:%d", 
            name(), mu->name(),
            node.name.c_str(), static_cast<int>(node.port)
            );
    }
       
    if (resp.err == ERR_SUCCESS)
    {
        dassert (resp.ballot == get_ballot(), "");
        dassert (resp.decree == mu->data.header.decree, "");

        switch (targetStatus)
        {
        case PS_SECONDARY:
            dassert (_primary_states.check_exist(node, PS_SECONDARY), "");
            dassert (mu->left_secondary_ack_count() > 0, "");
            if (0 == mu->decrease_left_secondary_ack_count())
            {
                do_possible_commit_on_primary(mu);
            }
            break;
        case PS_POTENTIAL_SECONDARY:            
            dassert (mu->left_potential_secondary_ack_count() > 0, "");
            if (0 == mu->decrease_left_potential_secondary_ack_count())
            {
                do_possible_commit_on_primary(mu);
            }
            break;
        default:
            ddebug( 
                "%s: mutation %s prepare ack skipped coz the node is now inactive", name(), mu->name()
                );
            break;
        }
    }

    // failure handling
    else
    {
        // note targetStatus and (curent) status may diff
        if (targetStatus == PS_POTENTIAL_SECONDARY)
        {
            dassert (mu->left_potential_secondary_ack_count() > 0, "");
            if (0 == mu->decrease_left_potential_secondary_ack_count())
            {
                do_possible_commit_on_primary(mu);
            }
        }
        handle_remote_failure(st, node, resp.err);
    }
}

void replica::ack_prepare_message(int err, mutation_ptr& mu)
{
    prepare_ack resp;
    resp.gpid = get_gpid();
    resp.err = err;
    resp.ballot = get_ballot();
    resp.decree = mu->data.header.decree;

    // for PS_POTENTIAL_SECONDARY ONLY
    resp.last_committed_decree_in_app = _app->last_committed_decree(); 
    resp.last_committed_decree_in_prepare_list = last_committed_decree();

    dassert (nullptr != mu->owner_message(), "");
    reply(mu->owner_message(), resp);

    ddebug( "%s: mutation %s ack_prepare_message", name(), mu->name());
}

void replica::cleanup_preparing_mutations(bool is_primary)
{
    decree start = last_committed_decree() + 1;
    decree end = _prepare_list->max_decree();

    for (decree decree = start; decree <= end; decree++)
    {
        mutation_ptr mu = _prepare_list->get_mutation_by_decree(decree);
        if (mu != nullptr)
        {
            int c = mu->clear_prepare_or_commit_tasks();
            if (!is_primary)
            {
                dassert (0 == c, "");
            }
            else
            {
                ////PerformanceCounters::Decrement(PerfCounters_TwoPhaseCommitOngoing, nullptr);
            }

            mu->clear_log_task();
        }
    }
}

}} // namespace
