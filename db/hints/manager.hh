/*
 * Modified by ScyllaDB
 * Copyright (C) 2017 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <unordered_map>
#include <vector>
#include <list>
#include <chrono>
#include <seastar/core/gate.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/shared_mutex.hh>
#include "gms/gossiper.hh"
#include "db/commitlog/commitlog.hh"
#include "utils/loading_shared_values.hh"

namespace db {
namespace hints {

using node_to_hint_store_factory_type = utils::loading_shared_values<gms::inet_address, db::commitlog>;
using hints_store_ptr = node_to_hint_store_factory_type::entry_ptr;
using hint_entry_reader = commitlog_entry_reader;
using timer_clock_type = seastar::lowres_clock;

class manager {
private:
    struct stats {
        uint64_t size_of_hints_in_progress = 0;
        uint64_t written = 0;
        uint64_t errors = 0;
        uint64_t dropped = 0;
        uint64_t sent = 0;
    };

    class end_point_hints_manager {
    public:
        using key_type = gms::inet_address;

        class sender {
            // Important: clock::now() must be noexcept.
            // TODO: add the corresponding static_assert() when seastar::lowres_clock::now() is marked as "noexcept".
            using clock = seastar::lowres_clock;

            enum class state {
                stopping,               // stop() was called
                ep_state_is_not_normal, // destination Node state is not NORMAL - usually means that it has been decommissioned
            };

            using state_set = enum_set<super_enum<state,
                state::stopping,
                state::ep_state_is_not_normal>>;

            enum class send_state {
                segment_replay_failed,  // current segment sending failed
                restart_segment,        // segment sending failed and it has to be restarted from the beginning since we failed to store one or more RPs
            };

            using send_state_set = enum_set<super_enum<send_state,
                send_state::segment_replay_failed,
                send_state::restart_segment>>;

            struct send_one_file_ctx {
                std::unordered_map<table_schema_version, column_mapping> schema_ver_to_column_mapping;
                seastar::gate file_send_gate;
                std::unordered_set<db::replay_position> rps_set; // number of elements in this set is never going to be greater than the maximum send queue length
                send_state_set state;
            };

        private:
            std::list<sstring> _segments_to_replay;
            replay_position _last_not_complete_rp;
            state_set _state;
            future<> _stopped;
            clock::time_point _next_flush_tp;
            clock::time_point _next_send_retry_tp;
            key_type _ep_key;
            end_point_hints_manager& _ep_manager;
            manager& _shard_manager;
            service::storage_proxy& _proxy;
            database& _db;
            gms::gossiper& _gossiper;
            seastar::shared_mutex& _file_update_mutex;

        public:
            sender(end_point_hints_manager& parent, service::storage_proxy& local_storage_proxy, database& local_db, gms::gossiper& local_gossiper) noexcept;

            /// \brief A constructor that should be called from the copy/move-constructor of end_point_hints_manager.
            ///
            /// Make sure to properly reassign the references - especially to the \param parent and its internals.
            ///
            /// \param other the "sender" instance to copy from
            /// \param parent the parent object for this "sender" instance
            sender(const sender& other, end_point_hints_manager& parent) noexcept;

            /// \brief Start sending hints.
            ///
            /// Flush hints aggregated to far to the storage every hints_flush_period.
            /// If the _segments_to_replay is not empty sending send all hints we have.
            ///
            /// Sending is stopped when stop() is called.
            void start();

            /// \brief Stop the sender - make sure all background sending is complete.
            future<> stop() noexcept;

            /// \brief Add a new segment ready for sending.
            void add_segment(sstring seg_name);

            /// \brief Check if there are still unsent segments.
            /// \return TRUE if there are still unsent segments.
            bool have_segments() const noexcept { return !_segments_to_replay.empty(); };

        private:
            /// \brief Send hints collected so far.
            ///
            /// Send hints aggregated so far. This function is going to try to deplete
            /// the _segments_to_replay list. Once it's empty it's going to be repopulated during the next send_hints() call
            /// with the new hints files if any.
            ///
            /// send_hints() is going to stop sending if it sends for too long (longer than the timer period). In this case it's
            /// going to return and next send_hints() is going to continue from the point the previous call left.
            void send_hints_maybe() noexcept;

            /// \brief Try to send one hint read from the file.
            ///  - Limit the maximum memory size of hints "in the air" and the maximum total number of hints "in the air".
            ///  - Discard the hints that are older than the grace seconds value of the corresponding table.
            ///  - Limit the maximum time for sending hints.
            ///
            /// If sending fails we are going to clear the state::segment_replay_ok in the _state and \ref rp is going to be stored in the _rps_set.
            /// If sending is successful then \ref rp is going to be removed from the _rps_set.
            ///
            /// \param ctx_ptr shared pointer to the file sending context
            /// \param buf buffer representing the hint
            /// \param rp replay position of this hint in the file (see commitlog for more details on "replay position")
            /// \param secs_since_file_mod last modification time stamp (in seconds since Epoch) of the current hints file
            /// \param fname name of the hints file this hint was read from
            /// \return future that resolves when next hint may be sent
            future<> send_one_hint(lw_shared_ptr<send_one_file_ctx> ctx_ptr, temporary_buffer<char> buf, db::replay_position rp, gc_clock::duration secs_since_file_mod, const sstring& fname);

            /// \brief Send all hint from a single file and delete it after it has been successfully sent.
            /// Send all hints from the given file. Limit the maximum amount of time we are allowed to send.
            /// If we run out of time we will pick up in the next iteration from where we left in this one.
            ///
            /// \param fname file to send
            /// \param sending_began_at time when the sending timer started the current iteration
            /// \return TRUE if file has been successfully sent
            bool send_one_file(const sstring& fname);

            /// \brief Checks if we can still send hints.
            /// \return TRUE if the destination Node is either ALIVE or has left the NORMAL state (e.g. has been decommissioned).
            bool can_send() noexcept;

            /// \brief Restore a mutation object from the hints file entry.
            /// \param ctx_ptr pointer to the send context
            /// \param buf hints file entry
            /// \return The mutation object representing the original mutation stored in the hints file.
            mutation get_mutation(lw_shared_ptr<send_one_file_ctx> ctx_ptr, temporary_buffer<char>& buf);

            /// \brief Get a reference to the column_mapping object for a given frozen mutation.
            /// \param ctx_ptr pointer to the send context
            /// \param fm Frozen mutation object
            /// \param hr hint entry reader object
            /// \return
            const column_mapping& get_column_mapping(lw_shared_ptr<send_one_file_ctx> ctx_ptr, const frozen_mutation& fm, const hint_entry_reader& hr);

            /// \brief Perform a single mutation send atempt.
            ///
            /// If the original destination end point is still a replica for the given mutation - send the mutation directly
            /// to it, otherwise execute the mutation "from scratch" with CL=ANY.
            ///
            /// \param m mutation to send
            /// \param natural_endpoints current replicas for the given mutation
            /// \return future that resolves when the operation is complete
            future<> do_send_one_mutation(mutation m, const std::vector<gms::inet_address>& natural_endpoints) noexcept;

            /// \brief Send one mutation out.
            ///
            /// \param m mutation to send
            /// \return future that resolves when the mutation sending processing is complete.
            future<> send_one_mutation(mutation m);

            /// \brief Get the last modification time stamp for a given file.
            /// \param fname File name
            /// \return The last modification time stamp for \param fname.
            static future<timespec> get_last_file_modification(const sstring& fname);

            struct stats& shard_stats() {
                return _shard_manager._stats;
            }

            /// \brief Flush all pending hints to storage if hints_flush_period passed since the last flush event.
            /// \return Ready, never exceptional, future when operation is complete.
            future<> flush_maybe() noexcept;

            const key_type& end_point_key() const noexcept {
                return _ep_key;
            }

            /// \brief Return the amount of time we want to sleep after the current iteration.
            /// \return The time till the soonest event: flushing or re-sending.
            clock::duration next_sleep_duration() const;
        };

    private:
        key_type _key;
        manager& _shard_manager;
        hints_store_ptr _hints_store_anchor;
        seastar::gate _store_gate;
        seastar::shared_mutex _file_update_mutex;

        enum class state {
            can_hint,               // hinting is currently allowed (used by the space_watchdog)
            stopping                // stopping is in progress (stop() method has been called)
        };

        using state_set = enum_set<super_enum<state,
            state::can_hint,
            state::stopping>>;

        state_set _state;
        const boost::filesystem::path _hints_dir;
        uint64_t _hints_in_progress = 0;
        sender _sender;

    public:
        end_point_hints_manager(const key_type& key, manager& shard_manager);
        end_point_hints_manager(end_point_hints_manager&&);

        const key_type& end_point_key() const noexcept {
            return _key;
        }

        /// \brief Get the corresponding hints_store object. Create it if needed.
        /// \note Must be called under the \ref _file_update_mutex.
        /// \return The corresponding hints_store object.
        future<hints_store_ptr> get_or_load();

        /// \brief Store a single mutation hint.
        /// \param s column family descriptor
        /// \param fm frozen mutation object
        /// \param tr_state trace_state handle
        /// \return FALSE if hint is definitely not going to be stored
        bool store_hint(schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept;

        /// \brief Populates the _segments_to_replay list.
        ///  Populates the _segments_to_replay list with the names of the files in the <manager hints files directory> directory
        ///  in the order they should be sent out.
        ///
        /// \return Ready future when end point hints manager is initialized.
        future<> populate_segments_to_replay();

        /// \brief Waits till all writers complete and shuts down the hints store.
        /// \return Ready future when the store is shut down.
        future<> stop() noexcept;

        /// \brief Start the timer.
        void start();

        /// \return Number of in-flight (towards the file) hints.
        uint64_t hints_in_progress() const noexcept {
            return _hints_in_progress;
        }

        bool can_hint() const noexcept {
            return _state.contains(state::can_hint);
        }

        void allow_hints() noexcept {
            _state.set(state::can_hint);
        }

        void forbid_hints() noexcept {
            _state.remove(state::can_hint);
        }

        void set_stopping() noexcept {
            _state.set(state::stopping);
        }

        bool stopping() const noexcept {
            return _state.contains(state::stopping);
        }

        seastar::shared_mutex& file_update_mutex() {
            return _file_update_mutex;
        }

    private:
        /// \brief Creates a new hints store object.
        ///
        /// - Creates a hints store directory if doesn't exist: <shard_hints_dir>/<ep_key>
        /// - Creates a store object.
        /// - Populate _segments_to_replay if it's empty.
        ///
        /// \return A new hints store object.
        future<commitlog> add_store() noexcept;

        /// \brief Flushes all hints written so far to the disk.
        ///  - Repopulates the _segments_to_replay list if needed.
        ///
        /// \return Ready future when the procedure above completes.
        future<> flush_current_hints() noexcept;

        struct stats& shard_stats() {
            return _shard_manager._stats;
        }
    };

private:
    using ep_key_type = typename end_point_hints_manager::key_type;
    using ep_managers_map_type = std::unordered_map<ep_key_type, end_point_hints_manager>;

    class space_watchdog {
    private:
        static const std::chrono::seconds _watchdog_period;

    private:
        std::unordered_set<ep_key_type> _eps_with_pending_hints;
        size_t _total_size = 0;
        manager& _shard_manager;
        seastar::gate _gate;
        seastar::timer<timer_clock_type> _timer;
        int _files_count = 0;

    public:
        space_watchdog(manager& shard_manager);
        future<> stop() noexcept;
        void start();

    private:
        /// \brief Check that hints don't occupy too much disk space.
        ///
        /// Verifies that the whole \ref manager::_hints_dir occupies less than \ref manager::max_shard_disk_space_size.
        ///
        /// If it does, stop all end point managers that have more than one hints file - we don't want some DOWN Node to
        /// prevent hints to other Nodes from being generated (e.g. due to some temporary overload and timeout).
        ///
        /// This is a simplistic implementation of a manager for a limited shared resource with a minimum guarantied share for all
        /// participants.
        ///
        /// This implementation guaranties at least a single hint share for all end point managers.
        void on_timer();

        /// \brief Scan files in a single end point directory.
        ///
        /// Add sizes of files in the directory to _total_size. If number of files is greater than 1 add this end point ID
        /// to _eps_with_pending_hints so that we may block it if _total_size value becomes greater than the maximum allowed
        /// value.
        ///
        /// \param path directory to scan
        /// \param ep_name end point ID (as a string)
        /// \return future that resolves when scanning is complete
        future<> scan_one_ep_dir(boost::filesystem::path path, ep_key_type ep_name);
    };

public:
    static const std::string FILENAME_PREFIX;
    static const std::chrono::seconds hints_flush_period;
    static const std::chrono::seconds hint_file_write_timeout;
    static size_t max_shard_disk_space_size;

private:
    static constexpr uint64_t _max_size_of_hints_in_progress = 10 * 1024 * 1024; // 10MB
    static constexpr size_t _hint_segment_size_in_mb = 32;
    static constexpr size_t _max_hints_per_ep_size_mb = 128; // 4 files 32MB each
    static constexpr size_t _max_hints_send_queue_length = 128;
    const boost::filesystem::path _hints_dir;

    node_to_hint_store_factory_type _store_factory;
    std::unordered_set<sstring> _hinted_dcs;
    shared_ptr<service::storage_proxy> _proxy_anchor;
    shared_ptr<gms::gossiper> _gossiper_anchor;
    locator::snitch_ptr& _local_snitch_ptr;
    int64_t _max_hint_window_us = 0;
    database& _local_db;
    bool _stopping = false;

    // Limit the maximum size of in-flight (being sent) hints by 10% of the shard memory.
    // Also don't allow more than 128 in-flight hints to limit the collateral memory consumption as well.
    const size_t _max_send_in_flight_memory;
    const size_t _min_send_hint_budget;
    seastar::semaphore _send_limiter;

    space_watchdog _space_watchdog;
    ep_managers_map_type _ep_managers;
    stats _stats;
    seastar::metrics::metric_groups _metrics;

public:
    manager(sstring hints_directory, std::vector<sstring> hinted_dcs, int64_t max_hint_window_ms, distributed<database>& db);
    ~manager();
    future<> start(shared_ptr<service::storage_proxy> proxy_ptr, shared_ptr<gms::gossiper> gossiper_ptr);
    future<> stop();
    bool store_hint(gms::inet_address ep, schema_ptr s, lw_shared_ptr<const frozen_mutation> fm, tracing::trace_state_ptr tr_state) noexcept;

    /// \brief Check if a hint may be generated to the give end point
    /// \param ep end point to check
    /// \return true if we should generate the hint to the given end point if it becomes unavailable
    bool can_hint_for(ep_key_type ep) const noexcept;

    /// \brief Check if there aren't too many in-flight hints
    ///
    /// This function checks if there are too many "in-flight" hints on the current shard - hints that are being stored
    /// and which storing is not complete yet. This is meant to stabilize the memory consumption of the hints storing path
    /// which is initialed from the storage_proxy WRITE flow. storage_proxy is going to check this condition and if it
    /// returns TRUE it won't attempt any new WRITEs thus eliminating the possibility of new hints generation. If new hints
    /// are not generated the amount of in-flight hints amount and thus the memory they are consuming is going to drop eventualy
    /// because the hints are going to be either stored or dropped. After that the things are going to get back to normal again.
    ///
    /// Note that we can't consider the disk usage consumption here because the disk usage is not promissed to drop down shortly
    /// because it requires the remote node to be UP.
    ///
    /// \param ep end point to check
    /// \return TRUE if we are allowed to generate hint to the given end point but there are too many in-flight hints
    bool too_many_in_flight_hints_for(ep_key_type ep) const noexcept;

    /// \brief Check if DC \param ep belongs to is "hintable"
    /// \param ep End point identificator
    /// \return TRUE if hints are allowed to be generated to \param ep.
    bool check_dc_for(ep_key_type ep) const noexcept;

    /// \return Size of mutations of hints in-flight (to the disk) at the moment.
    uint64_t size_of_hints_in_progress() const noexcept {
        return _stats.size_of_hints_in_progress;
    }

    /// \brief Get the number of in-flight (to the disk) hints to a given end point.
    /// \param ep End point identificator
    /// \return Number of hints in-flight to \param ep.
    uint64_t hints_in_progress_for(ep_key_type ep) const noexcept {
        auto it = find_ep_manager(ep);
        if (it == ep_managers_end()) {
            return 0;
        }
        return it->second.hints_in_progress();
    }

    static future<> rebalance() {
        // TODO
        return make_ready_future<>();
    }

private:
    node_to_hint_store_factory_type& store_factory() noexcept {
        return _store_factory;
    }

    const boost::filesystem::path& hints_dir() const {
        return _hints_dir;
    }

    service::storage_proxy& local_storage_proxy() const noexcept {
        return *_proxy_anchor;
    }

    gms::gossiper& local_gossiper() const noexcept {
        return *_gossiper_anchor;
    }

    database& local_db() noexcept {
        return _local_db;
    }

    end_point_hints_manager& get_ep_manager(ep_key_type ep);
    bool have_ep_manager(ep_key_type ep) const noexcept;

private:
    ep_managers_map_type::iterator find_ep_manager(ep_key_type ep_key) noexcept {
        return _ep_managers.find(ep_key);
    }

    ep_managers_map_type::const_iterator find_ep_manager(ep_key_type ep_key) const noexcept {
        return _ep_managers.find(ep_key);
    }

    ep_managers_map_type::iterator ep_managers_end() noexcept {
        return _ep_managers.end();
    }

    ep_managers_map_type::const_iterator ep_managers_end() const noexcept {
        return _ep_managers.end();
    }
};

}
}
