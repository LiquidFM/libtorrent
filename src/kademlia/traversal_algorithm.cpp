/*

Copyright (c) 2006-2016, Arvid Norberg & Daniel Wallin
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/time.hpp" // for total_seconds

#include <libtorrent/kademlia/traversal_algorithm.hpp>
#include <libtorrent/kademlia/routing_table.hpp>
#include <libtorrent/kademlia/rpc_manager.hpp>
#include <libtorrent/kademlia/node.hpp>
#include <libtorrent/kademlia/dht_observer.hpp> // for dht_logger
#include <libtorrent/session_status.hpp>
#include <libtorrent/socket_io.hpp> // for read_*_endpoint
#include <libtorrent/alert_types.hpp> // for dht_lookup
#include <libtorrent/aux_/time.hpp>

#ifndef TORRENT_DISABLE_LOGGING
#include <libtorrent/hex.hpp> // to_hex
#endif

#include <functional>

using namespace std::placeholders;

namespace libtorrent { namespace dht
{
using detail::address_size;
using detail::read_v4_endpoint;
#if TORRENT_USE_IPV6
using detail::read_v6_endpoint;
#endif

#if TORRENT_USE_ASSERTS
template <class It, class Cmp>
bool is_sorted(It b, It e, Cmp cmp)
{
	if (b == e) return true;

	typename std::iterator_traits<It>::value_type v = *b;
	++b;
	while (b != e)
	{
		if (cmp(*b, v)) return false;
		v = *b;
		++b;
	}
	return true;
}
#endif

observer_ptr traversal_algorithm::new_observer(udp::endpoint const& ep
	, node_id const& id)
{
	auto o = m_node.m_rpc.allocate_observer<null_observer>(self(), ep, id);
#if TORRENT_USE_ASSERTS
	if (o) o->m_in_constructor = false;
#endif
	return o;
}

traversal_algorithm::traversal_algorithm(
	node& dht_node
	, node_id target)
	: m_node(dht_node)
	, m_target(target)
	, m_invoke_count(0)
	, m_branch_factor(3)
	, m_responses(0)
	, m_timeouts(0)
{
#ifndef TORRENT_DISABLE_LOGGING
	if (get_node().observer())
	{
		char hex_target[41];
		aux::to_hex(target, hex_target);
		get_node().observer()->log(dht_logger::traversal, "[%p] NEW target: %s k: %d"
			, static_cast<void*>(this), hex_target, int(m_node.m_table.bucket_size()));
	}
#endif
}

void traversal_algorithm::resort_results()
{
	std::sort(m_results.begin(), m_results.end()
		, [this](observer_ptr const& lhs, observer_ptr const& rhs)
		{ return compare_ref(lhs->id(), rhs->id(), m_target); });
}

void traversal_algorithm::add_entry(node_id const& id, udp::endpoint addr, unsigned char flags)
{
	TORRENT_ASSERT(m_node.m_rpc.allocation_size() >= sizeof(find_data_observer));
	auto o = new_observer(addr, id);
	if (!o)
	{
#ifndef TORRENT_DISABLE_LOGGING
		if (get_node().observer())
		{
			get_node().observer()->log(dht_logger::traversal, "[%p] failed to allocate memory or observer. aborting!"
				, static_cast<void*>(this));
		}
#endif
		done();
		return;
	}
	if (id.is_all_zeros())
	{
		o->set_id(generate_random_id());
		o->flags |= observer::flag_no_id;
	}

	o->flags |= flags;

	TORRENT_ASSERT(libtorrent::dht::is_sorted(m_results.begin(), m_results.end()
		, [this](observer_ptr const& lhs, observer_ptr const& rhs)
		{ return compare_ref(lhs->id(), rhs->id(), m_target); }));

	std::vector<observer_ptr>::iterator iter = std::lower_bound(
		m_results.begin(), m_results.end(), o
		, [this](observer_ptr const& lhs, observer_ptr const& rhs)
		{ return compare_ref(lhs->id(), rhs->id(), m_target); });

	if (iter == m_results.end() || (*iter)->id() != id)
	{
		if (m_node.settings().restrict_search_ips
			&& !(flags & observer::flag_initial))
		{
#if TORRENT_USE_IPV6
			if (o->target_addr().is_v6())
			{
				address_v6::bytes_type addr_bytes = o->target_addr().to_v6().to_bytes();
				address_v6::bytes_type::const_iterator prefix_it = addr_bytes.begin();
				std::uint64_t const prefix6 = detail::read_uint64(prefix_it);

				if (m_peer6_prefixes.insert(prefix6).second)
					goto add_result;
			}
			else
#endif
			{
				// mask the lower octet
				std::uint32_t const prefix4
					= o->target_addr().to_v4().to_ulong() & 0xffffff00;

				if (m_peer4_prefixes.insert(prefix4).second)
					goto add_result;
			}

			// we already have a node in this search with an IP very
			// close to this one. We know that it's not the same, because
			// it claims a different node-ID. Ignore this to avoid attacks
#ifndef TORRENT_DISABLE_LOGGING
			if (get_node().observer())
			{
				char hex_id[41];
				aux::to_hex(o->id(), hex_id);
				get_node().observer()->log(dht_logger::traversal
					, "[%p] traversal DUPLICATE node. id: %s addr: %s type: %s"
					, static_cast<void*>(this), hex_id, print_address(o->target_addr()).c_str(), name());
			}
#endif
			return;
		}

	add_result:

		TORRENT_ASSERT((o->flags & observer::flag_no_id)
			|| std::none_of(m_results.begin(), m_results.end()
			, [&id](observer_ptr const& ob) { return ob->id() == id; }));

#ifndef TORRENT_DISABLE_LOGGING
		if (get_node().observer())
		{
			char hex_id[41];
			aux::to_hex(id, hex_id);
			get_node().observer()->log(dht_logger::traversal
				, "[%p] ADD id: %s addr: %s distance: %d invoke-count: %d type: %s"
				, static_cast<void*>(this), hex_id, print_endpoint(addr).c_str()
				, distance_exp(m_target, id), m_invoke_count, name());
		}
#endif
		iter = m_results.insert(iter, o);

		TORRENT_ASSERT(libtorrent::dht::is_sorted(m_results.begin(), m_results.end()
			, [this](observer_ptr const& lhs, observer_ptr const& rhs)
			{ return compare_ref(lhs->id(), rhs->id(), m_target); }));
	}

	if (m_results.size() > 100)
	{
		for (int i = 100; i < int(m_results.size()); ++i)
		{
			if ((m_results[i]->flags & (observer::flag_queried | observer::flag_failed | observer::flag_alive))
				== observer::flag_queried)
			{
				// set the done flag on any outstanding queries to prevent them from
				// calling finished() or failed()
				m_results[i]->flags |= observer::flag_done;
				TORRENT_ASSERT(m_invoke_count > 0);
				--m_invoke_count;
			}

#if TORRENT_USE_ASSERTS
			m_results[i]->m_was_abandoned = true;
#endif
		}
		m_results.resize(100);
	}
}

void traversal_algorithm::start()
{
	// in case the routing table is empty, use the
	// router nodes in the table
	if (m_results.size() < 3) add_router_entries();
	init();
	bool is_done = add_requests();
	if (is_done) done();
}

char const* traversal_algorithm::name() const
{
	return "traversal_algorithm";
}

void traversal_algorithm::traverse(node_id const& id, udp::endpoint addr)
{
#ifndef TORRENT_DISABLE_LOGGING
	if (id.is_all_zeros() && get_node().observer())
	{
		get_node().observer()->log(dht_logger::traversal
			, "[%p] WARNING node returned a list which included a node with id 0"
			, static_cast<void*>(this));
	}
#endif

	// let the routing table know this node may exist
	m_node.m_table.heard_about(id, addr);

	add_entry(id, addr, 0);
}

void traversal_algorithm::finished(observer_ptr o)
{
#if TORRENT_USE_ASSERTS
	auto i = std::find(m_results.begin(), m_results.end(), o);
	TORRENT_ASSERT(i != m_results.end() || m_results.size() == 100);
#endif

	// if this flag is set, it means we increased the
	// branch factor for it, and we should restore it
	if (o->flags & observer::flag_short_timeout)
	{
		TORRENT_ASSERT(m_branch_factor > 0);
		--m_branch_factor;
	}

	TORRENT_ASSERT(o->flags & observer::flag_queried);
	o->flags |= observer::flag_alive;

	++m_responses;
	TORRENT_ASSERT(m_invoke_count > 0);
	--m_invoke_count;
	bool is_done = add_requests();
	if (is_done) done();
}

// prevent request means that the total number of requests has
// overflown. This query failed because it was the oldest one.
// So, if this is true, don't make another request
void traversal_algorithm::failed(observer_ptr o, int flags)
{
	// don't tell the routing table about
	// node ids that we just generated ourself
	if ((o->flags & observer::flag_no_id) == 0)
		m_node.m_table.node_failed(o->id(), o->target_ep());

	if (m_results.empty()) return;

	TORRENT_ASSERT(o->flags & observer::flag_queried);
	if (flags & short_timeout)
	{
		// short timeout means that it has been more than
		// two seconds since we sent the request, and that
		// we'll most likely not get a response. But, in case
		// we do get a late response, keep the handler
		// around for some more, but open up the slot
		// by increasing the branch factor
		if ((o->flags & observer::flag_short_timeout) == 0)
			++m_branch_factor;
		o->flags |= observer::flag_short_timeout;
#ifndef TORRENT_DISABLE_LOGGING
		if (get_node().observer())
		{
			char hex_id[41];
			aux::to_hex(o->id(), hex_id);
			get_node().observer()->log(dht_logger::traversal
				, "[%p] 1ST_TIMEOUT id: %s distance: %d addr: %s branch-factor: %d "
				"invoke-count: %d type: %s"
				, static_cast<void*>(this), hex_id, distance_exp(m_target, o->id())
				, print_address(o->target_addr()).c_str(), m_branch_factor
				, m_invoke_count, name());
		}
#endif
	}
	else
	{
		o->flags |= observer::flag_failed;
		// if this flag is set, it means we increased the
		// branch factor for it, and we should restore it
		if (o->flags & observer::flag_short_timeout)
			--m_branch_factor;

#ifndef TORRENT_DISABLE_LOGGING
		if (get_node().observer())
		{
			char hex_id[41];
			aux::to_hex(o->id(), hex_id);
			get_node().observer()->log(dht_logger::traversal
				, "[%p] TIMEOUT id: %s distance: %d addr: %s branch-factor: %d "
				"invoke-count: %d type: %s"
				, static_cast<void*>(this), hex_id, distance_exp(m_target, o->id())
				, print_address(o->target_addr()).c_str(), m_branch_factor
				, m_invoke_count, name());
		}
#endif

		++m_timeouts;
		TORRENT_ASSERT(m_invoke_count > 0);
		--m_invoke_count;
	}

	if (flags & prevent_request)
	{
		--m_branch_factor;
		if (m_branch_factor <= 0) m_branch_factor = 1;
	}
	bool is_done = add_requests();
	if (is_done) done();
}

void traversal_algorithm::done()
{
#ifndef TORRENT_DISABLE_LOGGING
	int results_target = m_node.m_table.bucket_size();
	int closest_target = 160;
#endif

	for (auto const& o : m_results)
	{
		if ((o->flags & (observer::flag_queried | observer::flag_failed)) == observer::flag_queried)
		{
			// set the done flag on any outstanding queries to prevent them from
			// calling finished() or failed() after we've already declared the traversal
			// done
			o->flags |= observer::flag_done;
		}

#ifndef TORRENT_DISABLE_LOGGING
		if (results_target > 0 && (o->flags & observer::flag_alive) && get_node().observer())
		{
			TORRENT_ASSERT(o->flags & observer::flag_queried);
			char hex_id[41];
			aux::to_hex(o->id(), hex_id);
			get_node().observer()->log(dht_logger::traversal
				, "[%p] id: %s distance: %d addr: %s"
				, static_cast<void*>(this), hex_id, closest_target
				, print_endpoint(o->target_ep()).c_str());

			--results_target;
			int dist = distance_exp(m_target, o->id());
			if (dist < closest_target) closest_target = dist;
		}
#endif
	}

#ifndef TORRENT_DISABLE_LOGGING
	if (get_node().observer())
	{
		get_node().observer()->log(dht_logger::traversal
			, "[%p] COMPLETED distance: %d type: %s"
			, static_cast<void*>(this), closest_target, name());
	}
#endif

	// delete all our references to the observer objects so
	// they will in turn release the traversal algorithm
	m_results.clear();
	m_invoke_count = 0;
}

bool traversal_algorithm::add_requests()
{
	int results_target = m_node.m_table.bucket_size();

	// this only counts outstanding requests at the top of the
	// target list. This is <= m_invoke count. m_invoke_count
	// is the total number of outstanding requests, including
	// old ones that may be waiting on nodes much farther behind
	// the current point we've reached in the search.
	int outstanding = 0;

	// if we're doing aggressive lookups, we keep branch-factor
	// outstanding requests _at the tops_ of the result list. Otherwise
	// we just keep any branch-factor outstanding requests
	bool agg = m_node.settings().aggressive_lookups;

	// Find the first node that hasn't already been queried.
	// and make sure that the 'm_branch_factor' top nodes
	// stay queried at all times (obviously ignoring failed nodes)
	// and without surpassing the 'result_target' nodes (i.e. k=8)
	// this is a slight variation of the original paper which instead
	// limits the number of outstanding requests, this limits the
	// number of good outstanding requests. It will use more traffic,
	// but is intended to speed up lookups
	for (std::vector<observer_ptr>::iterator i = m_results.begin()
		, end(m_results.end()); i != end
		&& results_target > 0
		&& (agg ? outstanding < m_branch_factor
			: m_invoke_count < m_branch_factor);
		++i)
	{
		observer* o = i->get();
		if (o->flags & observer::flag_alive)
		{
			TORRENT_ASSERT(o->flags & observer::flag_queried);
			--results_target;
			continue;
		}
		if (o->flags & observer::flag_queried)
		{
			// if it's queried, not alive and not failed, it
			// must be currently in flight
			if ((o->flags & observer::flag_failed) == 0)
				++outstanding;

			continue;
		}

#ifndef TORRENT_DISABLE_LOGGING
		if (get_node().observer())
		{
			char hex_id[41];
			aux::to_hex(o->id(), hex_id);
			get_node().observer()->log(dht_logger::traversal
				, "[%p] INVOKE nodes-left: %d top-invoke-count: %d "
				"invoke-count: %d branch-factor: %d "
				"distance: %d id: %s addr: %s type: %s"
				, static_cast<void*>(this), int(m_results.end() - i), outstanding, int(m_invoke_count)
				, int(m_branch_factor), distance_exp(m_target, o->id()), hex_id
				, print_address(o->target_addr()).c_str(), name());
		}
#endif

		o->flags |= observer::flag_queried;
		if (invoke(*i))
		{
			TORRENT_ASSERT(m_invoke_count < (std::numeric_limits<std::uint16_t>::max)());
			++m_invoke_count;
			++outstanding;
		}
		else
		{
			o->flags |= observer::flag_failed;
		}
	}

	// this is the completion condition. If we found m_node.m_table.bucket_size()
	// (i.e. k=8) completed results, without finding any still
	// outstanding requests, we're done.
	// also, if invoke count is 0, it means we didn't even find 'k'
	// working nodes, we still have to terminate though.
	return (results_target == 0 && outstanding == 0) || m_invoke_count == 0;
}

void traversal_algorithm::add_router_entries()
{
#ifndef TORRENT_DISABLE_LOGGING
	if (get_node().observer())
	{
		get_node().observer()->log(dht_logger::traversal
			, "[%p] using router nodes to initiate traversal algorithm %d routers"
			, static_cast<void*>(this), int(std::distance(m_node.m_table.begin(), m_node.m_table.end())));
	}
#endif
	for (auto const& n : m_node.m_table)
		add_entry(node_id(), n, observer::flag_initial);
}

void traversal_algorithm::init()
{
	m_branch_factor = m_node.branch_factor();
	m_node.add_traversal_algorithm(this);
}

traversal_algorithm::~traversal_algorithm()
{
	m_node.remove_traversal_algorithm(this);
}

void traversal_algorithm::status(dht_lookup& l)
{
	l.timeouts = m_timeouts;
	l.responses = m_responses;
	l.outstanding_requests = m_invoke_count;
	l.branch_factor = m_branch_factor;
	l.type = name();
	l.nodes_left = 0;
	l.first_timeout = 0;

	int last_sent = INT_MAX;
	time_point now = aux::time_now();
	for (std::vector<observer_ptr>::iterator i = m_results.begin()
		, end(m_results.end()); i != end; ++i)
	{
		observer& o = **i;
		if (o.flags & observer::flag_queried)
		{
			last_sent = (std::min)(last_sent, int(total_seconds(now - o.sent())));
			if (o.has_short_timeout()) ++l.first_timeout;
			continue;
		}
		++l.nodes_left;
	}
	l.last_sent = last_sent;
}

void traversal_observer::reply(msg const& m)
{
	bdecode_node r = m.message.dict_find_dict("r");
	if (!r)
	{
#ifndef TORRENT_DISABLE_LOGGING
		if (get_observer())
		{
			get_observer()->log(dht_logger::traversal
				, "[%p] missing response dict"
				, static_cast<void*>(algorithm()));
		}
#endif
		return;
	}

#ifndef TORRENT_DISABLE_LOGGING
	if (get_observer())
	{
		bdecode_node nid = r.dict_find_string("id");
		char hex_id[41];
		aux::to_hex({nid.string_ptr(), 20}, hex_id);
		get_observer()->log(dht_logger::traversal
			, "[%p] RESPONSE id: %s invoke-count: %d addr: %s type: %s"
			, static_cast<void*>(algorithm()), hex_id, algorithm()->invoke_count()
			, print_endpoint(target_ep()).c_str(), algorithm()->name());
	}
#endif

	// look for nodes
#if TORRENT_USE_IPV6
	udp protocol = algorithm()->get_node().protocol();
#endif
	char const* nodes_key = algorithm()->get_node().protocol_nodes_key();
	bdecode_node n = r.dict_find_string(nodes_key);
	if (n)
	{
		char const* nodes = n.string_ptr();
		char const* end = nodes + n.string_length();

		while (end - nodes >= 20 + address_size(protocol) + 2)
		{
			node_id id;
			std::copy(nodes, nodes + 20, id.begin());
			nodes += 20;
			udp::endpoint ep;
#if TORRENT_USE_IPV6
			if (protocol == udp::v6())
				ep = read_v6_endpoint<udp::endpoint>(nodes);
			else
#endif
				ep = read_v4_endpoint<udp::endpoint>(nodes);
			algorithm()->traverse(id, ep);
		}
	}

	bdecode_node id = r.dict_find_string("id");
	if (!id || id.string_length() != 20)
	{
#ifndef TORRENT_DISABLE_LOGGING
		if (get_observer())
		{
			get_observer()->log(dht_logger::traversal, "[%p] invalid id in response"
				, static_cast<void*>(algorithm()));
		}
#endif
		return;
	}

	// in case we didn't know the id of this peer when we sent the message to
	// it. For instance if it's a bootstrap node.
	set_id(node_id(id.string_ptr()));
}

} } // namespace libtorrent::dht
