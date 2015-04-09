// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/immediate_consistency/backfiller.hpp"

#include "clustering/immediate_consistency/history.hpp"
#include "rdb_protocol/protocol.hpp"
#include "store_view.hpp"

/* `ITEM_PIPELINE_SIZE` is the maximum combined size of the items that we send to the
backfillee that it hasn't consumed yet. `ITEM_CHUNK_SIZE` is the typical size of an item
message we send over the network. */
static const size_t ITEM_PIPELINE_SIZE = 4 * MEGABYTE;
static const size_t ITEM_CHUNK_SIZE = 100 * KILOBYTE;

backfiller_t::backfiller_t(
        mailbox_manager_t *_mailbox_manager,
        branch_history_manager_t *_branch_history_manager,
        store_view_t *_store) :
    mailbox_manager(_mailbox_manager),
    branch_history_manager(_branch_history_manager),
    store(_store),
    registrar(mailbox_manager, this)
    { }

backfiller_t::client_t::client_t(
        backfiller_t *_parent,
        const backfiller_bcard_t::intro_1_t &_intro,
        signal_t *interruptor) :
    parent(_parent),
    intro(_intro),
    full_region(intro.initial_version.get_domain()),
    pre_items(full_region.beg, full_region.end,
        key_range_t::right_bound_t(full_region.inner.left)),
    item_throttler(ITEM_PIPELINE_SIZE),
    item_throttler_acq(&item_throttler, 0),
    pre_items_mailbox(parent->mailbox_manager,
        std::bind(&client_t::on_pre_items, this, ph::_1, ph::_2, ph::_3)),
    begin_session_mailbox(parent->mailbox_manager,
        std::bind(&client_t::on_begin_session, this, ph::_1, ph::_2, ph::_3)),
    end_session_mailbox(parent->mailbox_manager,
        std::bind(&client_t::on_end_session, this, ph::_1, ph::_2)),
    ack_items_mailbox(parent->mailbox_manager,
        std::bind(&client_t::on_ack_items, this, ph::_1, ph::_2, ph::_3))
{
    /* Compute the common ancestor of our version and the backfillee's version */
    {
        region_map_t<binary_blob_t> our_version_blob;
        read_token_t read_token;
        parent->store->new_read_token(&read_token);
        parent->store->do_get_metainfo(order_token_t::ignore.with_read_mode(),
            &read_token, interruptor, &our_version_blob);
        region_map_t<version_t> our_version = to_version_map(our_version_blob);

        branch_history_combiner_t combined_history(
            parent->branch_history_manager,
            &intro.initial_version_history);

        std::vector<std::pair<region_t, state_timestamp_t> > common_pairs;
        for (const auto &pair1 : our_version) {
            for (const auto &pair2 : intro.initial_version.mask(pair1.first)) {
                for (const auto &pair3 : version_find_common(&combined_history,
                        pair1.second, pair2.second, pair2.first)) {
                    common_pairs.push_back(
                        std::make_pair(pair3.first, pair3.second.timestamp));
                }
            }
        }
        common_version = region_map_t<state_timestamp_t>(
            common_pairs.begin(), common_pairs.end());
    }

    backfiller_bcard_t::intro_2_t our_intro;
    our_intro.common_version = common_version;
    our_intro.pre_items_mailbox = pre_items_mailbox.get_address();
    our_intro.begin_session_mailbox = begin_session_mailbox.get_address();
    our_intro.end_session_mailbox = end_session_mailbox.get_address();
    our_intro.ack_items_mailbox = ack_items_mailbox.get_address();
    send(parent->mailbox_manager, intro.intro_mailbox, our_intro);
}

class backfiller_t::client_t::session_t {
public:
    session_t(client_t *_parent, const key_range_t::right_bound_t &_threshold) :
        parent(_parent), threshold(_threshold)
    {
        coro_t::spawn_sometime(std::bind(&session_t::run, this, drainer.lock()));
    }

    void on_pre_items() {
        if (pulse_when_pre_items_arrive.has()) {
            pulse_when_pre_items_arrive->pulse_if_not_already_pulsed();
        }
    }

private:
    void run(auto_drainer_t::lock_t keepalive) {
        try {
            while (threshold != parent->full_region.inner.right) {
                /* Wait until there's room in the semaphore for the chunk we're about to
                process */
                new_semaphore_acq_t sem_acq(&parent->item_throttler, ITEM_CHUNK_SIZE);
                wait_interruptible(
                    sem_acq.acquisition_signal(), keepalive.get_drain_signal());

                /* Set up a `region_t` describing the range that still needs to be
                backfilled */
                region_t subregion = parent->full_region;
                subregion.inner.left = threshold.key();

                /* Copy items from the store into `callback_t::items` until the total size
                hits `ITEM_CHUNK_SIZE`; we finish the backfill range; or we run out of
                pre-items. */

                backfill_item_seq_t<backfill_item_t> chunk(
                    parent->full_region.beg, parent->full_region.end,
                    threshold);
                region_map_t<version_t> metainfo = region_map_t<version_t>::empty();

                {
                    class producer_t :
                        public store_view_t::backfill_pre_item_producer_t {
                    public:
                        producer_t(
                                backfill_item_seq_t<backfill_pre_item_t> *_pre_items,
                                scoped_ptr_t<cond_t> *_pulse_when_pre_items_arrive) :
                            pre_items(_pre_items),
                            temp_buf(pre_items->get_beg_hash(),
                                pre_items->get_end_hash(), pre_items->get_left_key()),
                            pulse_when_pre_items_arrive(_pulse_when_pre_items_arrive)
                            { }
                        ~producer_t() {
                            temp_buf.concat(std::move(*pre_items));
                            *pre_items = std::move(temp_buf);
                        }
                        continue_bool_t next_pre_item(
                                backfill_pre_item_t const **next_out,
                                key_range_t::right_bound_t *edge_out)
                                THROWS_NOTHING {
                            if (!pre_items->empty_of_items()) {
                                *next_out = &pre_items->front();
                                return continue_bool_t::CONTINUE;
                            } else if (!pre_items->empty_domain()) {
                                *next_out = nullptr;
                                *edge_out = pre_items->get_right_key();
                                pre_items->delete_to_key(*edge_out);
                                temp_buf.push_back_nothing(*edge_out);
                                return continue_bool_t::CONTINUE;
                            } else {
                                pulse_when_pre_items_arrive->init(new cond_t);
                                return continue_bool_t::ABORT;
                            }
                        }
                        void release_pre_item() THROWS_NOTHING {
                            pre_items->pop_front_into(&temp_buf);
                        }
                    private:
                        backfill_item_seq_t<backfill_pre_item_t> *pre_items, temp_buf;
                        scoped_ptr_t<cond_t> *pulse_when_pre_items_arrive;
                    } producer(&parent->pre_items, &pulse_when_pre_items_arrive);

                    class consumer_t : public store_view_t::backfill_item_consumer_t {
                    public:
                        consumer_t(backfill_item_seq_t<backfill_item_t> *_chunk,
                                region_map_t<version_t> *_metainfo) :
                            chunk(_chunk), metainfo(_metainfo) { }
                        continue_bool_t on_item(
                                const region_map_t<binary_blob_t> &item_metainfo,
                                backfill_item_t &&item) THROWS_NOTHING {
                            rassert(key_range_t::right_bound_t(item.range.left) >=
                                chunk->get_right_key());
                            rassert(!item.range.is_empty());
                            region_t mask(chunk->get_beg_hash(), chunk->get_end_hash(),
                                item.get_range());
                            mask.inner.left = chunk->get_right_key().key();
                            metainfo->concat(to_version_map(item_metainfo.mask(mask)));
                            chunk->push_back(std::move(item));
                            if (chunk->get_mem_size() < ITEM_CHUNK_SIZE) {
                                return continue_bool_t::CONTINUE;
                            } else {
                                return continue_bool_t::ABORT;
                            }
                        }
                        continue_bool_t on_empty_range(
                                const region_map_t<binary_blob_t> &range_metainfo,
                                const key_range_t::right_bound_t &new_threshold)
                                THROWS_NOTHING {
                            rassert(new_threshold >= chunk->get_right_key());
                            if (chunk->get_right_key() == new_threshold) {
                                /* This is a no-op */
                                return continue_bool_t::CONTINUE;
                            }
                            region_t mask;
                            mask.beg = chunk->get_beg_hash();
                            mask.end = chunk->get_end_hash();
                            mask.inner.left = chunk->get_right_key().key();
                            mask.inner.right = new_threshold;
                            metainfo->concat(to_version_map(range_metainfo.mask(mask)));
                            chunk->push_back_nothing(new_threshold);
                            return continue_bool_t::CONTINUE;
                        }
                        backfill_item_seq_t<backfill_item_t> *chunk;
                        region_map_t<version_t> *metainfo;
                    } consumer(&chunk, &metainfo);

                    parent->parent->store->send_backfill(
                        parent->common_version.mask(subregion), &producer, &consumer,
                        keepalive.get_drain_signal());

                    /* `producer` goes out of scope here, so it restores `pre_items` to
                    what it was before */
                }

                /* Check if we actually got a non-trivial chunk */
                if (chunk.get_left_key() != chunk.get_right_key()) {
                    /* Adjust for the fact that `chunk.get_mem_size()` isn't precisely
                    equal to `ITEM_CHUNK_SIZE`, and then transfer the semaphore
                    ownership. */
                    sem_acq.change_count(chunk.get_mem_size());
                    parent->item_throttler_acq.transfer_in(std::move(sem_acq));

                    /* Update `threshold` */
                    guarantee(chunk.get_left_key() == threshold);
                    threshold = chunk.get_right_key();

                    /* Note: It's essential that we update `common_version` and
                    `pre_items_*` if and only if we send the chunk over the network. So
                    we shouldn't e.g. check the interruptor in between. */
                    try {
                        /* Send the chunk over the network */
                        send(parent->parent->mailbox_manager,
                            parent->intro.items_mailbox,
                            parent->fifo_source.enter_write(), metainfo, chunk);

                        /* Update `common_version` to reflect the changes that will
                        happen on the backfillee in response to the chunk */
                        parent->common_version.update(
                            region_map_transform<version_t, state_timestamp_t>(
                                metainfo,
                                [](const version_t &v) { return v.timestamp; }));

                        /* Discard pre-items we don't need anymore */
                        size_t old_size = parent->pre_items.get_mem_size();
                        parent->pre_items.delete_to_key(threshold);
                        size_t new_size = parent->pre_items.get_mem_size();

                        /* Notify the backfiller that it's OK to send us more pre items
                        */
                        send(parent->parent->mailbox_manager,
                            parent->intro.ack_pre_items_mailbox,
                            parent->fifo_source.enter_write(), old_size - new_size);
                    } catch (const interrupted_exc_t &) {
                        crash("We shouldn't be interrupted during this block");
                    }
                }

                if (pulse_when_pre_items_arrive.has()) {
                    /* The reason we stopped this chunk was because we ran out of pre
                    items. Block until more pre items are available. */
                    wait_interruptible(pulse_when_pre_items_arrive.get(),
                        keepalive.get_drain_signal());
                    pulse_when_pre_items_arrive.reset();
                }
            }
        } catch (const interrupted_exc_t &) {
            /* The backfillee sent us a stop message; or the backfillee was destroyed; or
            the backfiller was destroyed. */
        }
    }

    client_t *parent;
    key_range_t::right_bound_t threshold;
    scoped_ptr_t<cond_t> pulse_when_pre_items_arrive;
    auto_drainer_t drainer;
};

void backfiller_t::client_t::on_begin_session(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &write_token,
        const key_range_t::right_bound_t &threshold) {
    fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, write_token);
    wait_interruptible(&exit_write, interruptor);

    guarantee(threshold <= pre_items.get_left_key(), "Every key must be backfilled at "
        "least once");
    current_session.init(new session_t(this, threshold));
}

void backfiller_t::client_t::on_end_session(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &write_token) {
    fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, write_token);
    wait_interruptible(&exit_write, interruptor);

    guarantee(current_session.has());
    current_session.reset();
    send(parent->mailbox_manager, intro.ack_end_session_mailbox,
        fifo_source.enter_write());
}

void backfiller_t::client_t::on_ack_items(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &write_token,
        size_t mem_size) {
    fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, write_token);
    wait_interruptible(&exit_write, interruptor);

    guarantee(static_cast<int64_t>(mem_size) <= item_throttler_acq.count());
    item_throttler_acq.change_count(item_throttler_acq.count() - mem_size);
}

void backfiller_t::client_t::on_pre_items(
        signal_t *interruptor,
        const fifo_enforcer_write_token_t &write_token,
        backfill_item_seq_t<backfill_pre_item_t> &&chunk) {
    fifo_enforcer_sink_t::exit_write_t exit_write(&fifo_sink, write_token);
    wait_interruptible(&exit_write, interruptor);

    pre_items.concat(std::move(chunk));
    if (current_session.has()) {
        current_session->on_pre_items();
    }
}

