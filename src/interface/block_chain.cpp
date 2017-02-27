/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/interface/block_chain.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database.hpp>
#include <bitcoin/blockchain/settings.hpp>
#include <bitcoin/blockchain/populate/populate_chain_state.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::config;
using namespace bc::message;
using namespace bc::database;
using namespace std::placeholders;

#define NAME "block_chain"

block_chain::block_chain(threadpool& pool,
    const blockchain::settings& chain_settings,
    const database::settings& database_settings, bool relay_transactions)
  : stopped_(true),
    settings_(chain_settings),
    spin_lock_sleep_(asio::milliseconds(1)),
    chain_state_populator_(*this, chain_settings),
    database_(database_settings),
    priority_pool_(thread_ceiling(chain_settings.cores),
        priority(chain_settings.priority)),
    dispatch_(priority_pool_, NAME "_priority"),
    transaction_organizer_(mutex_, dispatch_, pool, *this, chain_settings),
    block_organizer_(mutex_, dispatch_, pool, *this, chain_settings,
        relay_transactions)
{
}

// ============================================================================
// FAST CHAIN
// ============================================================================

// Readers.
// ----------------------------------------------------------------------------

bool block_chain::get_gaps(block_database::heights& out_gaps) const
{
    database_.blocks().gaps(out_gaps);
    return true;
}

bool block_chain::get_block_exists(const hash_digest& block_hash) const
{
    return database_.blocks().get(block_hash);
}

bool block_chain::get_block_hash(hash_digest& out_hash, size_t height) const
{
    const auto result = database_.blocks().get(height);

    if (!result)
        return false;

    out_hash = result.hash();
    return true;
}

bool block_chain::get_branch_work(uint256_t& out_work,
    const uint256_t& maximum, size_t from_height) const
{
    size_t top;
    if (!database_.blocks().top(top))
        return false;

    out_work = 0;
    for (auto height = from_height; height <= top && out_work < maximum;
        ++height)
    {
        const auto result = database_.blocks().get(height);
        if (!result)
            return false;

        out_work += chain::block::proof(result.bits());
    }

    return true;
}

bool block_chain::get_header(chain::header& out_header, size_t height) const
{
    auto result = database_.blocks().get(height);
    if (!result)
        return false;

    out_header = result.header();
    return true;
}

bool block_chain::get_height(size_t& out_height,
    const hash_digest& block_hash) const
{
    auto result = database_.blocks().get(block_hash);
    if (!result)
        return false;

    out_height = result.height();
    return true;
}

bool block_chain::get_bits(uint32_t& out_bits, const size_t& height) const
{
    auto result = database_.blocks().get(height);
    if (!result)
        return false;

    out_bits = result.bits();
    return true;
}

bool block_chain::get_timestamp(uint32_t& out_timestamp,
    const size_t& height) const
{
    auto result = database_.blocks().get(height);
    if (!result)
        return false;

    out_timestamp = result.timestamp();
    return true;
}

bool block_chain::get_version(uint32_t& out_version,
    const size_t& height) const
{
    auto result = database_.blocks().get(height);
    if (!result)
        return false;

    out_version = result.version();
    return true;
}

bool block_chain::get_last_height(size_t& out_height) const
{
    return database_.blocks().top(out_height);
}

bool block_chain::get_output(chain::output& out_output, size_t& out_height,
    bool& out_coinbase, const chain::output_point& outpoint,
    size_t branch_height, bool require_confirmed) const
{
    // This includes a cached value for spender height (or not_spent).
    // Get the highest tx with matching hash, at or below the branch height.
    return database_.transactions().get_output(out_output, out_height,
        out_coinbase, outpoint, branch_height, require_confirmed);
}

bool block_chain::get_is_unspent_transaction(const hash_digest& hash,
    size_t branch_height, bool require_confirmed) const
{
    const auto result = database_.transactions().get(hash, branch_height,
        require_confirmed);

    return result && !result.is_spent(branch_height);
}

bool block_chain::get_transaction_position(size_t& out_height,
    size_t& out_position, const hash_digest& hash,
    bool require_confirmed) const
{
    const auto result = database_.transactions().get(hash, max_size_t,
        require_confirmed);

    if (!result)
        return false;

    out_height = result.height();
    out_position = result.position();
    return true;
}

transaction_ptr block_chain::get_transaction(size_t& out_block_height,
    const hash_digest& hash, bool require_confirmed) const
{
    const auto result = database_.transactions().get(hash, max_size_t,
        require_confirmed);

    if (!result)
        return nullptr;

    out_block_height = result.height();
    return std::make_shared<transaction>(result.transaction());
}

// Writers
// ----------------------------------------------------------------------------

bool block_chain::begin_insert() const
{
    return database_.begin_insert();
}

bool block_chain::end_insert() const
{
    return database_.end_insert();
}

bool block_chain::insert(block_const_ptr block, size_t height)
{
    return database_.insert(*block, height) == error::success;
}

void block_chain::push(transaction_const_ptr tx, dispatcher&,
    result_handler handler)
{
    // Transaction push is currently sequential so dispatch is not used.
    handler(database_.push(*tx, chain_state()->enabled_forks()));
}

void block_chain::reorganize(const checkpoint& fork_point,
    block_const_ptr_list_const_ptr incoming_blocks,
    block_const_ptr_list_ptr outgoing_blocks, dispatcher& dispatch,
    result_handler handler)
{
    if (incoming_blocks->empty())
    {
        handler(error::operation_failed);
        return;
    }

    // The top (back) block is used to update the chain state.
    const auto complete =
        std::bind(&block_chain::handle_reorganize,
            this, _1, incoming_blocks->back(), handler);

    database_.reorganize(fork_point, incoming_blocks, outgoing_blocks,
        dispatch, complete);
}

void block_chain::handle_reorganize(const code& ec, block_const_ptr top,
    result_handler handler)
{
    if (!ec)
        set_chain_state(top->validation.state);

    handler(ec);
}

// Properties.
// ----------------------------------------------------------------------------

// For tx validator, call only from inside validate critical section.
chain::chain_state::ptr block_chain::chain_state() const
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    shared_lock lock(pool_state_mutex_);

    // Initialized on start and updated after each successful organization.
    return pool_state_;
    ///////////////////////////////////////////////////////////////////////////
}

// For block validator, call only from inside validate critical section.
chain::chain_state::ptr block_chain::chain_state(
    branch::const_ptr branch) const
{
    // Promote from cache if branch is same height as pool (most typical).
    // Generate from branch/store if the promotion is not successful.
    // If the organize is successful pool state will be updated accordingly.
    return chain_state_populator_.populate(chain_state(), branch);
}

// private.
code block_chain::set_chain_state(chain::chain_state::ptr previous)
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(pool_state_mutex_);

    pool_state_ = chain_state_populator_.populate(previous);
    return pool_state_ ? error::success : error::operation_failed;
    ///////////////////////////////////////////////////////////////////////////
}

// ============================================================================
// SAFE CHAIN
// ============================================================================

// Startup and shutdown.
// ----------------------------------------------------------------------------

bool block_chain::start()
{
    stopped_ = false;

    if (!database_.open())
        return false;

    // Initialize chain state after database start but before organizers.
    pool_state_ = chain_state_populator_.populate();

    return pool_state_ && transaction_organizer_.start() &&
        block_organizer_.start();
}

bool block_chain::stop()
{
    stopped_ = true;

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);

    // This cannot call organize or stop (lock safe).
    auto result = transaction_organizer_.stop() && block_organizer_.stop();

    // The priority pool must not be stopped while organizing.
    priority_pool_.shutdown();
    return result;
    ///////////////////////////////////////////////////////////////////////////
}

// Close is idempotent and thread safe.
// Optional as the blockchain will close on destruct.
bool block_chain::close()
{
    const auto result = stop();
    priority_pool_.join();
    return result && database_.close();
}

block_chain::~block_chain()
{
    close();
}

// Queries.
// ----------------------------------------------------------------------------

void block_chain::fetch_block(size_t height, block_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto block_result = database_.blocks().get(height);

        if (!block_result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto high = block_result.height();
        const auto count = block_result.transaction_count();
        transaction::list transactions;
        transactions.reserve(count);

        for (size_t position = 0; position < count; ++position)
        {
            const auto tx_result = database_.transactions().get(
                block_result.transaction_hash(position), max_size_t, true);

            if (!tx_result)
                return finish_read(slock, handler, error::operation_failed,
                nullptr, 0);

            BITCOIN_ASSERT(tx_result.height() == high);
            BITCOIN_ASSERT(tx_result.position() == position);
            transactions.emplace_back(tx_result.transaction());
        }

        const auto block = std::make_shared<message::block>(
            block_result.header(), std::move(transactions));
        return finish_read(slock, handler, error::success, block, high);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_block(const hash_digest& hash,
    block_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto block_result = database_.blocks().get(hash);

        if (!block_result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto high = block_result.height();
        const auto count = block_result.transaction_count();
        transaction::list transactions;
        transactions.reserve(count);

        for (size_t position = 0; position < count; ++position)
        {
            const auto tx_result = database_.transactions().get(
                block_result.transaction_hash(position), max_size_t, true);

            if (!tx_result)
                return finish_read(slock, handler, error::operation_failed,
                    nullptr, 0);

            BITCOIN_ASSERT(tx_result.height() == high);
            BITCOIN_ASSERT(tx_result.position() == position);
            transactions.emplace_back(tx_result.transaction());
        }

        const auto block = std::make_shared<message::block>(
            block_result.header(), std::move(transactions));
        return finish_read(slock, handler, error::success, block, high);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_block_header(size_t height,
    block_header_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.blocks().get(height);

        if (!result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto header = std::make_shared<message::header>(result.header());

        return finish_read(slock, handler, error::success, header,
            result.height());
    };
    read_serial(do_fetch);
}

void block_chain::fetch_block_header(const hash_digest& hash,
    block_header_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.blocks().get(hash);

        if (!result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto header = std::make_shared<message::header>(result.header());

        return finish_read(slock, handler, error::success, header,
            result.height());
    };
    read_serial(do_fetch);
}

void block_chain::fetch_merkle_block(size_t height,
    merkle_block_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.blocks().get(height);

        if (!result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto merkle = std::make_shared<merkle_block>(result.header(),
            result.transaction_count(), to_hashes(result), data_chunk{});

        return finish_read(slock, handler, error::success, merkle,
            result.height());
    };
    read_serial(do_fetch);
}

void block_chain::fetch_merkle_block(const hash_digest& hash,
    merkle_block_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.blocks().get(hash);

        if (!result)
            return finish_read(slock, handler, error::not_found, nullptr, 0);

        const auto merkle = std::make_shared<merkle_block>(result.header(),
            result.transaction_count(), to_hashes(result), data_chunk{});

        return finish_read(slock, handler, error::success, merkle,
            result.height());
    };
    read_serial(do_fetch);
}

void block_chain::fetch_compact_block(size_t height,
    compact_block_fetch_handler handler) const
{
    // TODO: implement compact blocks.
    handler(error::not_implemented, {}, 0);
}

void block_chain::fetch_compact_block(const hash_digest& hash,
    compact_block_fetch_handler handler) const
{
    // TODO: implement compact blocks.
    handler(error::not_implemented, {}, 0);
}

void block_chain::fetch_block_height(const hash_digest& hash,
    block_height_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.blocks().get(hash);
        return result ?
            finish_read(slock, handler, error::success, result.height()) :
            finish_read(slock, handler, error::not_found, 0);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_last_height(last_height_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        size_t last_height;
        return database_.blocks().top(last_height) ?
            finish_read(slock, handler, error::success, last_height) :
            finish_read(slock, handler, error::not_found, 0);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_transaction(const hash_digest& hash,
    bool require_confirmed, transaction_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr, 0, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.transactions().get(hash, max_size_t,
            require_confirmed);

        if (!result)
            return finish_read(slock, handler, error::not_found, nullptr, 0, 0);

        const auto tx = std::make_shared<transaction>(result.transaction());
        return finish_read(slock, handler, error::success, tx, result.height(),
            result.position());
    };
    read_serial(do_fetch);
}

// This is only used for the server API, need to document sentinel/forks.
// This is same as fetch_transaction but skips the tx payload.
void block_chain::fetch_transaction_position(const hash_digest& hash,
    bool require_confirmed, transaction_index_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, 0, 0);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        auto result = database_.transactions().get(hash, max_size_t,
            require_confirmed);

        return result ?
            finish_read(slock, handler, error::success, result.position(),
                result.height()) :
            finish_read(slock, handler, error::not_found, 0, 0);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_output(const chain::output_point& outpoint,
    bool require_confirmed, output_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto result = database_.transactions().get(outpoint.hash(),
            max_size_t, require_confirmed);

        if (!result)
            return finish_read(slock, handler, error::not_found,
                chain::output{});

        const auto output = result.output(outpoint.index());
        const auto ec = output.is_valid() ? error::success : error::not_found;
        return finish_read(slock, handler, ec, std::move(output));
    };
    read_serial(do_fetch);
}

void block_chain::fetch_spend(const chain::output_point& outpoint,
    spend_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        const auto point = database_.spends().get(outpoint);
        return point.hash() != null_hash ?
            finish_read(slock, handler, error::success, point) :
            finish_read(slock, handler, error::not_found, point);
    };
    read_serial(do_fetch);
}

void block_chain::fetch_history(const wallet::payment_address& address,
    size_t limit, size_t from_height, history_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        return finish_read(slock, handler, error::success,
            database_.history().get(address.hash(), limit, from_height));
    };
    read_serial(do_fetch);
}

void block_chain::fetch_stealth(const binary& filter, size_t from_height,
    stealth_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        return finish_read(slock, handler, error::success,
            database_.stealth().scan(filter, from_height));
    };
    read_serial(do_fetch);
}

// This may generally execute 29+ queries.
void block_chain::fetch_block_locator(const block::indexes& heights,
    block_locator_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr);
        return;
    }

    const auto do_fetch = [&](size_t slock)
    {
        size_t top;
        code ec(error::operation_failed);

        if (!database_.blocks().top(top))
            return finish_read(slock, handler, ec, nullptr);

        // Caller can cast down to get_blocks.
        auto get_headers = std::make_shared<message::get_headers>();
        auto& hashes = get_headers->start_hashes();
        hashes.reserve(heights.size());
        ec = error::success;

        for (const auto height: heights)
        {
            const auto result = database_.blocks().get(height);

            if (!result)
            {
                ec = error::not_found;
                hashes.clear();
                break;
            }

            hashes.push_back(result.header().hash());
        }

        hashes.shrink_to_fit();
        return finish_read(slock, handler, ec, get_headers);
    };
    read_serial(do_fetch);
}

// This may execute over 500 queries.
void block_chain::fetch_locator_block_hashes(get_blocks_const_ptr locator,
    const hash_digest& threshold, size_t limit,
    inventory_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, nullptr);
        return;
    }

    // This is based on the idea that looking up by block hash to get heights
    // will be much faster than hashing each retrieved block to test for stop.
    const auto do_fetch = [&](size_t slock)
    {
        // Find the first block height.
        // If no start block is on our chain we start with block 0.
        size_t start = 0;
        for (const auto& hash: locator->start_hashes())
        {
            const auto result = database_.blocks().get(hash);
            if (result)
            {
                start = result.height();
                break;
            }
        }

        // Find the stop block height.
        // The maximum stop block is 501 blocks after start (to return 500).
        const auto begin = safe_add(start, size_t(1));
        auto stop = safe_add(begin, limit);

        if (locator->stop_hash() != null_hash)
        {
            // If the stop block is not on chain we treat it as a null stop.
            const auto stop_result = database_.blocks().get(locator->stop_hash());
            if (stop_result)
                stop = std::min(stop_result.height(), stop);
        }

        // Find the threshold block height.
        // If the threshold is above the start it becomes the new start.
        if (threshold != null_hash)
        {
            const auto start_result = database_.blocks().get(threshold);
            if (start_result)
                start = std::max(start_result.height(), start);
        }

        auto hashes = std::make_shared<inventory>();
        const auto size = floor_subtract(stop, begin);
        hashes->inventories().reserve(size);

        // Build the hash list until we hit last or the blockchain top.
        for (auto index = begin; index < stop; ++index)
        {
            const auto result = database_.blocks().get(index);

            // If not found then we are at our top.
            if (!result)
                break;

            const auto& header = result.header();
            static const auto id = inventory::type_id::block;
            hashes->inventories().push_back({ id, header.hash() });
        }

        hashes->inventories().shrink_to_fit();
        return finish_read(slock, handler, error::success, hashes);
    };
    read_serial(do_fetch);
}

// This may execute over 2000 queries.
void block_chain::fetch_locator_block_headers(get_headers_const_ptr locator,
    const hash_digest& threshold, size_t limit,
    locator_block_headers_fetch_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped, {});
        return;
    }

    // This is based on the idea that looking up by block hash to get heights
    // will be much faster than hashing each retrieved block to test for stop.
    const auto do_fetch = [&](size_t slock)
    {
        // TODO: consolidate this portion with fetch_locator_block_hashes.
        //---------------------------------------------------------------------
        // Find the first block height.
        // If no start block is on our chain we start with block 0.
        size_t start = 0;
        for (const auto& hash: locator->start_hashes())
        {
            const auto result = database_.blocks().get(hash);
            if (result)
            {
                start = result.height();
                break;
            }
        }

        // Find the stop block height.
        // The maximum stop block is 501 blocks after start (to return 500).
        const auto begin = safe_add(start, size_t(1));
        auto stop = safe_add(begin, limit);

        if (locator->stop_hash() != null_hash)
        {
            // If the stop block is not on chain we treat it as a null stop.
            const auto stop_result = database_.blocks().get(
                locator->stop_hash());

            if (stop_result)
                stop = std::min(stop_result.height(), stop);
        }

        // Find the threshold block height.
        // If the threshold is above the start it becomes the new start.
        if (threshold != null_hash)
        {
            const auto start_result = database_.blocks().get(threshold);
            if (start_result)
                start = std::max(start_result.height(), start);
        }

        //---------------------------------------------------------------------

        const auto headers = std::make_shared<message::headers>();
        const auto size = floor_subtract(stop, begin);
        headers->elements().reserve(size);

        // Build the hash list until we hit last or the blockchain top.
        for (auto index = begin; index < stop; ++index)
        {
            const auto result = database_.blocks().get(index);

            // If not found then we are at our top.
            if (!result)
                break;

            headers->elements().push_back(result.header());
        }

        headers->elements().shrink_to_fit();
        return finish_read(slock, handler, error::success, headers);
    };
    read_serial(do_fetch);
}

// Transaction Pool.
//-----------------------------------------------------------------------------

// Same as fetch_mempool but also optimized for maximum possible block fee as
// limited by total bytes and signature operations.
void block_chain::fetch_template(merkle_block_fetch_handler handler) const
{
    transaction_organizer_.fetch_template(handler);
}

// Fetch a set of currently-valid unconfirmed txs in dependency order.
// All txs satisfy the fee minimum and are valid at the next chain state.
// The set of blocks is limited in count to size. The set may have internal
// dependencies but all inputs must be satisfied at the current height.
void block_chain::fetch_mempool(size_t count_limit, uint64_t minimum_fee,
    inventory_fetch_handler handler) const
{
    transaction_organizer_.fetch_mempool(count_limit, handler);
}

// Filters.
//-----------------------------------------------------------------------------

// This may execute up to 500 queries.
// This filters against the block pool and then the block chain.
void block_chain::filter_blocks(get_data_ptr message,
    result_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    const auto do_fetch = [this, message, handler](size_t slock)
    {
        block_organizer_.filter(message);

        auto& inventories = message->inventories();
        const auto& blocks = database_.blocks();

        for (auto it = inventories.begin(); it != inventories.end();)
            if (it->is_block_type() && blocks.get(it->hash()))
                it = inventories.erase(it);
            else
                ++it;


        return finish_read(slock, handler, error::success);
    };
    read_serial(do_fetch);
}

// This filters against all transactions (confirmed and unconfirmed).
void block_chain::filter_transactions(get_data_ptr message,
    result_handler handler) const
{
    if (stopped())
    {
        handler(error::service_stopped);
        return;
    }

    const auto do_fetch = [this, message, handler](size_t slock)
    {
        auto& inventories = message->inventories();
        const auto& transactions = database_.transactions();

        for (auto it = inventories.begin(); it != inventories.end();)
        {
            if (it->is_transaction_type() &&
                get_is_unspent_transaction(it->hash(), max_size_t, false))
                it = inventories.erase(it);
            else
                ++it;
        }

        return finish_read(slock, handler, error::success);
    };
    read_serial(do_fetch);
}

// Subscribers.
//-----------------------------------------------------------------------------

void block_chain::subscribe_reorganize(reorganize_handler&& handler)
{
    // Pass this through to the organizer, which issues the notifications.
    block_organizer_.subscribe_reorganize(std::move(handler));
}

void block_chain::subscribe_transaction(transaction_handler&& handler)
{
    // Pass this through to the tx pool, which issues the notifications.
    transaction_organizer_.subscribe_transaction(std::move(handler));
}

// Organizers.
//-----------------------------------------------------------------------------

void block_chain::organize(block_const_ptr block, result_handler handler)
{
    // This cannot call organize or stop (lock safe).
    block_organizer_.organize(block, handler);
}

void block_chain::organize(transaction_const_ptr tx, result_handler handler)
{
    // This cannot call organize or stop (lock safe).
    transaction_organizer_.organize(tx, handler);
}

// Properties (thread safe).
// ----------------------------------------------------------------------------

const settings& block_chain::chain_settings() const
{
    return settings_;
}

// protected
bool block_chain::stopped() const
{
    return stopped_;
}

// Locking helpers.
// ----------------------------------------------------------------------------
// private

template <typename Reader>
void block_chain::read_serial(const Reader& reader) const
{
    while (true)
    {
        // Get a read handle.
        const auto sequence = database_.begin_read();

        // If read handle indicates write or reader finishes false, wait.
        if (!database_.is_write_locked(sequence) && reader(sequence))
            break;

        // Sleep while waiting for write to complete.
        std::this_thread::sleep_for(spin_lock_sleep_);
    }
}

template <typename Handler, typename... Args>
bool block_chain::finish_read(handle sequence, Handler handler,
    Args... args) const
{
    // If the read sequence was interrupted by a write, return false (wait).
    if (!database_.is_read_valid(sequence))
        return false;

    // Handle the read (done).
    // To forward args we would need to use std::bind here, but not necessary
    // because all parameterizations use smart pointers or integral types.
    handler(args...);
    return true;
}

// Utilities.
//-----------------------------------------------------------------------------
// private

hash_list block_chain::to_hashes(const block_result& result)
{
    const auto count = result.transaction_count();
    hash_list hashes;
    hashes.reserve(count);

    for (size_t position = 0; position < count; ++position)
        hashes.push_back(result.transaction_hash(position));

    return hashes;
}

} // namespace blockchain
} // namespace libbitcoin
